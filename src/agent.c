#include "h264.h"
#include "hid.h"
#include "protocol.h"
#include "rtp_h264.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TAG "nanokvm-agent"
#define DEFAULT_CONTROL_PORT 3390U
#define DEFAULT_VIDEO_PORT 5004U
#define DEFAULT_WIDTH 1920U
#define DEFAULT_HEIGHT 1080U
#define DEFAULT_BITRATE 3000U
#define HEARTBEAT_INTERVAL_MS 1000U
#define HEARTBEAT_TIMEOUT_MS 5000U
#define CAPTURE_RETRY_SLEEP_NS 10000000L

enum KvmFrameKind { KVM_FRAME_SPS = 1, KVM_FRAME_PPS = 2, KVM_FRAME_IDR = 3, KVM_FRAME_P = 4 };

typedef struct
{
	void* handle;
	void (*init)(uint8_t level);
	int (*read_image)(uint16_t width, uint16_t height, uint8_t type, uint16_t quality,
	                  uint8_t** data, uint32_t* length);
	int (*free_data)(uint8_t** data);
	void (*set_frame_detect)(uint8_t enabled);
} KvmApi;

typedef struct
{
	const char* gateway;
	uint16_t control_port;
	uint16_t video_port;
	uint16_t width;
	uint16_t height;
	uint16_t bitrate;
	int control_fd;
	int video_fd;
	struct sockaddr_in video_address;
	KvmApi kvm;
	HidState hid;
	RtpH264Packetizer packetizer;
	atomic_bool streaming;
	atomic_bool wait_for_idr;
	uint32_t timestamp;
	atomic_uint_fast32_t sent_packets;
	atomic_uint_fast32_t dropped_packets;
	atomic_uint_fast32_t capture_frames;
	atomic_uint_fast32_t dropped_frames;
	uint64_t last_ping_at;
	uint64_t last_pong_at;
	uint64_t last_stats_at;
	pthread_t video_thread;
} Agent;

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int ignored)
{
	(void)ignored;
	stop_requested = 1;
}

static uint64_t monotonic_milliseconds(void)
{
	struct timespec now = { 0 };
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static bool kvm_load(KvmApi* api)
{
	const char* candidates[] = { "/kvmapp/server/dl_lib/libkvm.so", "/tmp/server/dl_lib/libkvm.so",
		"libkvm.so" };
	for (size_t index = 0; index < sizeof(candidates) / sizeof(candidates[0]); index++)
	{
		api->handle = dlopen(candidates[index], RTLD_NOW | RTLD_LOCAL);
		if (api->handle)
			break;
	}
	if (!api->handle)
		return false;
	*(void**)(&api->init) = dlsym(api->handle, "kvmv_init");
	*(void**)(&api->read_image) = dlsym(api->handle, "kvmv_read_img");
	*(void**)(&api->free_data) = dlsym(api->handle, "free_kvmv_data");
	*(void**)(&api->set_frame_detect) = dlsym(api->handle, "set_frame_detact");
	if (!api->init || !api->read_image || !api->free_data || !api->set_frame_detect)
		return false;
	return true;
}

static bool resolve_gateway(const char* gateway, struct in_addr* address)
{
	if (inet_pton(AF_INET, gateway, address) == 1)
		return true;
	struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct addrinfo* results = NULL;
	if (getaddrinfo(gateway, NULL, &hints, &results) != 0 || !results)
		return false;
	*address = ((const struct sockaddr_in*)results->ai_addr)->sin_addr;
	freeaddrinfo(results);
	return true;
}

static bool connect_control(Agent* agent)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	int enabled = 1;
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
	struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons(agent->control_port) };
	if (!resolve_gateway(agent->gateway, &address.sin_addr) ||
	    connect(fd, (const struct sockaddr*)&address, sizeof(address)) != 0)
	{
		(void)close(fd);
		return false;
	}
	agent->video_address.sin_addr = address.sin_addr;
	uint8_t hello[8] = { 0 };
	protocol_write_u16(hello, agent->width);
	protocol_write_u16(hello + 2, agent->height);
	protocol_write_u16(hello + 4, agent->video_port);
	protocol_write_u16(hello + 6, RTP_H264_DEFAULT_MTU);
	if (!protocol_send(fd, NANOKVM_CONTROL_HELLO, hello, sizeof(hello)))
	{
		(void)close(fd);
		return false;
	}
	agent->control_fd = fd;
	agent->last_ping_at = monotonic_milliseconds();
	agent->last_pong_at = agent->last_ping_at;
	agent->last_stats_at = agent->last_ping_at;
	return true;
}

static void disconnect_control(Agent* agent)
{
	hid_release_all(&agent->hid);
	if (agent->control_fd >= 0)
		(void)close(agent->control_fd);
	agent->control_fd = -1;
	atomic_store(&agent->streaming, false);
	atomic_store(&agent->wait_for_idr, true);
}

static bool send_stats(Agent* agent)
{
	uint8_t payload[NANOKVM_STATS_PAYLOAD_SIZE] = { 0 };
	protocol_write_u32(payload, atomic_load(&agent->sent_packets));
	protocol_write_u32(payload + 4, atomic_load(&agent->dropped_packets));
	protocol_write_u32(payload + 8, atomic_load(&agent->capture_frames));
	protocol_write_u32(payload + 12, atomic_load(&agent->dropped_frames));
	return protocol_send(agent->control_fd, NANOKVM_CONTROL_STATS, payload, sizeof(payload));
}

static bool send_packet(void* context, const uint8_t* packet, size_t length)
{
	Agent* agent = context;
	const ssize_t result = sendto(agent->video_fd, packet, length, MSG_DONTWAIT,
	                              (const struct sockaddr*)&agent->video_address,
	                              sizeof(agent->video_address));
	if (result == (ssize_t)length)
	{
		(void)atomic_fetch_add(&agent->sent_packets, 1);
		return true;
	}
	(void)atomic_fetch_add(&agent->dropped_packets, 1);
	return false;
}

static size_t start_code_length(const uint8_t* data, size_t length, size_t offset)
{
	if (offset + 3 <= length && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1)
		return 3;
	if (offset + 4 <= length && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 &&
	    data[offset + 3] == 1)
		return 4;
	return 0;
}

static bool send_h264(Agent* agent, const uint8_t* data, size_t length)
{
	size_t first = length;
	for (size_t offset = 0; offset < length; offset++)
	{
		const size_t code = start_code_length(data, length, offset);
		if (code != 0)
		{
			first = offset;
			break;
		}
	}
	if (first == length)
		return rtp_h264_packetize(&agent->packetizer, data, length, agent->timestamp, send_packet, agent);
	bool sent = false;
	for (size_t offset = first; offset < length;)
	{
		const size_t code = start_code_length(data, length, offset);
		if (code == 0)
			return false;
		const size_t start = offset + code;
		size_t next = start;
		while (next < length && start_code_length(data, length, next) == 0)
			next++;
		if (start < next)
			sent |= rtp_h264_packetize_marker(&agent->packetizer, data + start, next - start,
		                                   agent->timestamp, next == length, send_packet, agent);
		offset = next;
	}
	return sent;
}

static void handle_control(Agent* agent, const NanokvmControlMessage* message)
{
	switch (message->type)
	{
		case NANOKVM_CONTROL_START_STREAM:
			atomic_store(&agent->streaming, true);
			atomic_store(&agent->wait_for_idr, true);
			(void)fprintf(stderr, "%s: START_STREAM 수신 (%ux%u 원본 H.264 전송 시작)\n", TAG,
			              agent->width, agent->height);
			break;
		case NANOKVM_CONTROL_STOP_STREAM:
			atomic_store(&agent->streaming, false);
			hid_release_all(&agent->hid);
			(void)fprintf(stderr, "%s: STOP_STREAM 수신\n", TAG);
			break;
		case NANOKVM_CONTROL_IDR_REQUEST:
			atomic_store(&agent->wait_for_idr, true);
			break;
		case NANOKVM_CONTROL_KEY:
			if (message->length == 3)
				(void)hid_scancode(&agent->hid, message->payload[0], message->payload[1] != 0,
				                   message->payload[2] != 0);
			break;
		case NANOKVM_CONTROL_POINTER_ABS:
			if (message->length == 12)
				(void)hid_absolute(&agent->hid, protocol_read_u16(message->payload),
				                   protocol_read_u16(message->payload + 2),
				                   protocol_read_u16(message->payload + 4),
				                   protocol_read_u16(message->payload + 6),
				                   protocol_read_u16(message->payload + 8));
			break;
		case NANOKVM_CONTROL_POINTER_REL:
			if (message->length == 5)
				(void)hid_relative(&agent->hid, (int16_t)protocol_read_u16(message->payload),
				                   (int16_t)protocol_read_u16(message->payload + 2), message->payload[4]);
			break;
		case NANOKVM_CONTROL_WHEEL:
			if (message->length == 2)
				(void)hid_wheel(&agent->hid, protocol_read_u16(message->payload));
			break;
		case NANOKVM_CONTROL_RELEASE_ALL:
			hid_release_all(&agent->hid);
			break;
		case NANOKVM_CONTROL_PING:
			(void)protocol_send(agent->control_fd, NANOKVM_CONTROL_PONG, NULL, 0);
			break;
		case NANOKVM_CONTROL_PONG:
			agent->last_pong_at = monotonic_milliseconds();
			break;
		default: break;
	}
}

static void usage(const char* executable)
{
	(void)fprintf(stderr, "Usage: %s -gateway host-or-ipv4 [-control-port n] [-video-port n] [-width n] [-height n] [-bitrate n]\n", executable);
}

static void* video_loop(void* argument)
{
	Agent* agent = argument;
	bool capture_initialized = false;
	while (!stop_requested)
	{
		if (!atomic_load(&agent->streaming))
		{
			const struct timespec pause = { .tv_sec = 0, .tv_nsec = CAPTURE_RETRY_SLEEP_NS };
			(void)nanosleep(&pause, NULL);
			continue;
		}
		if (!capture_initialized)
		{
			(void)fprintf(stderr, "%s: gateway control 연결 후 libkvm 초기화 시작\n", TAG);
			agent->kvm.init(0);
			agent->kvm.set_frame_detect(0);
			capture_initialized = true;
			(void)fprintf(stderr, "%s: libkvm 초기화 완료\n", TAG);
		}
		uint8_t* data = NULL;
		uint32_t length = 0;
		const int kind = agent->kvm.read_image(agent->width, agent->height, 1, agent->bitrate,
		                                        &data, &length);
		if (kind < 0 || !data || length == 0)
		{
			if (data)
				(void)agent->kvm.free_data(&data);
			const struct timespec pause = { .tv_sec = 0, .tv_nsec = CAPTURE_RETRY_SLEEP_NS };
			(void)nanosleep(&pause, NULL);
			continue;
		}
		if (kind == KVM_FRAME_IDR)
			atomic_store(&agent->wait_for_idr, false);
		(void)atomic_fetch_add(&agent->capture_frames, 1);
		if (atomic_load(&agent->streaming) &&
		    (kind == KVM_FRAME_SPS || kind == KVM_FRAME_PPS || kind == KVM_FRAME_IDR ||
		     (!atomic_load(&agent->wait_for_idr) && kind == KVM_FRAME_P)))
		{
			if (!send_h264(agent, data, length))
				(void)atomic_fetch_add(&agent->dropped_frames, 1);
		}
		else if (kind == KVM_FRAME_P)
			(void)atomic_fetch_add(&agent->dropped_frames, 1);
		agent->timestamp += 9000U;
		(void)agent->kvm.free_data(&data);
	}
	return NULL;
}

int main(int argc, char* argv[])
{
	Agent agent = { .control_port = DEFAULT_CONTROL_PORT, .video_port = DEFAULT_VIDEO_PORT,
		.width = DEFAULT_WIDTH, .height = DEFAULT_HEIGHT, .bitrate = DEFAULT_BITRATE,
		.control_fd = -1, .video_fd = -1, .timestamp = 1 };
	for (int index = 1; index < argc; index++)
	{
		if (strcmp(argv[index], "-gateway") == 0 && index + 1 < argc)
			agent.gateway = argv[++index];
		else if (strcmp(argv[index], "-control-port") == 0 && index + 1 < argc)
			agent.control_port = (uint16_t)strtoul(argv[++index], NULL, 10);
		else if (strcmp(argv[index], "-video-port") == 0 && index + 1 < argc)
			agent.video_port = (uint16_t)strtoul(argv[++index], NULL, 10);
		else if (strcmp(argv[index], "-width") == 0 && index + 1 < argc)
			agent.width = (uint16_t)strtoul(argv[++index], NULL, 10);
		else if (strcmp(argv[index], "-height") == 0 && index + 1 < argc)
			agent.height = (uint16_t)strtoul(argv[++index], NULL, 10);
		else if (strcmp(argv[index], "-bitrate") == 0 && index + 1 < argc)
			agent.bitrate = (uint16_t)strtoul(argv[++index], NULL, 10);
		else { usage(argv[0]); return 2; }
	}
	if (!agent.gateway || agent.width == 0 || agent.height == 0 || agent.bitrate == 0)
	{
		usage(argv[0]);
		return 2;
	}
	if (!kvm_load(&agent.kvm))
	{
		(void)fprintf(stderr, "%s: libkvm.so를 열 수 없습니다\n", TAG);
		return 1;
	}
	agent.video_fd = socket(AF_INET, SOCK_DGRAM, 0);
	agent.video_address.sin_family = AF_INET;
	agent.video_address.sin_port = htons(agent.video_port);
	if (agent.video_fd < 0)
		return 1;
	rtp_h264_packetizer_init(&agent.packetizer, RTP_H264_DEFAULT_MTU, (uint32_t)getpid());
	hid_init(&agent.hid, NULL, NULL, NULL);
	atomic_init(&agent.streaming, false);
	atomic_init(&agent.wait_for_idr, true);
	atomic_init(&agent.sent_packets, 0);
	atomic_init(&agent.dropped_packets, 0);
	atomic_init(&agent.capture_frames, 0);
	atomic_init(&agent.dropped_frames, 0);
	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);
	(void)signal(SIGPIPE, SIG_IGN);
	if (pthread_create(&agent.video_thread, NULL, video_loop, &agent) != 0)
		return 1;
	while (!stop_requested)
	{
		if (agent.control_fd < 0)
		{
			if (!connect_control(&agent))
			{
				sleep(1);
				continue;
			}
			(void)fprintf(stderr, "%s: gateway control 연결 완료\n", TAG);
		}
		fd_set readable;
		FD_ZERO(&readable);
		FD_SET(agent.control_fd, &readable);
		struct timeval timeout = { .tv_sec = 0, .tv_usec = 5000 };
		if (select(agent.control_fd + 1, &readable, NULL, NULL, &timeout) > 0)
		{
			NanokvmControlMessage message = { 0 };
			if (!protocol_receive(agent.control_fd, &message))
			{
				disconnect_control(&agent);
				continue;
			}
			handle_control(&agent, &message);
		}
		const uint64_t now = monotonic_milliseconds();
		if (now - agent.last_pong_at > HEARTBEAT_TIMEOUT_MS ||
		    (now - agent.last_ping_at >= HEARTBEAT_INTERVAL_MS &&
		     !protocol_send(agent.control_fd, NANOKVM_CONTROL_PING, NULL, 0)))
		{
			disconnect_control(&agent);
			continue;
		}
		if (now - agent.last_ping_at >= HEARTBEAT_INTERVAL_MS)
			agent.last_ping_at = now;
		if (now - agent.last_stats_at >= HEARTBEAT_INTERVAL_MS)
		{
			if (!send_stats(&agent))
			{
				disconnect_control(&agent);
				continue;
			}
			agent.last_stats_at = now;
		}
	}
	(void)pthread_join(agent.video_thread, NULL);
	hid_release_all(&agent.hid);
	if (agent.control_fd >= 0)
		(void)close(agent.control_fd);
	if (agent.video_fd >= 0)
		(void)close(agent.video_fd);
	return 0;
}
