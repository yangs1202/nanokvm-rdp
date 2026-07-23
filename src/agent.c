#include "h264.h"
#include "hid.h"
#include "protocol.h"
#include "rtp_h264.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netinet/tcp.h>
#include <signal.h>
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

enum KvmFrameKind { KVM_FRAME_SPS = 1, KVM_FRAME_PPS = 2, KVM_FRAME_IDR = 3, KVM_FRAME_P = 4 };

typedef struct
{
	void* handle;
	void (*init)(uint8_t level);
	int (*read_image)(uint16_t width, uint16_t height, uint8_t type, uint16_t quality,
	                  uint8_t** data, uint32_t* length);
	int (*free_data)(uint8_t** data);
	void (*set_frame_detect)(uint8_t enabled);
	void (*deinit)(void);
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
	bool streaming;
	bool wait_for_idr;
	uint32_t timestamp;
	uint32_t sent_packets;
	uint32_t dropped_packets;
	uint32_t capture_frames;
	uint32_t dropped_frames;
	uint64_t last_ping_at;
	uint64_t last_pong_at;
	uint64_t last_stats_at;
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
	*(void**)(&api->deinit) = dlsym(api->handle, "kvmv_deinit");
	if (!api->init || !api->read_image || !api->free_data || !api->set_frame_detect || !api->deinit)
		return false;
	api->init(0);
	api->set_frame_detect(0);
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
	if (inet_pton(AF_INET, agent->gateway, &address.sin_addr) != 1 ||
	    connect(fd, (const struct sockaddr*)&address, sizeof(address)) != 0)
	{
		(void)close(fd);
		return false;
	}
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
	agent->streaming = false;
	agent->wait_for_idr = true;
}

static bool send_stats(Agent* agent)
{
	uint8_t payload[NANOKVM_STATS_PAYLOAD_SIZE] = { 0 };
	protocol_write_u32(payload, agent->sent_packets);
	protocol_write_u32(payload + 4, agent->dropped_packets);
	protocol_write_u32(payload + 8, agent->capture_frames);
	protocol_write_u32(payload + 12, agent->dropped_frames);
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
		agent->sent_packets++;
		return true;
	}
	agent->dropped_packets++;
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
	bool sent = false;
	size_t start = 0;
	for (size_t offset = 0; offset < length;)
	{
		const size_t code = start_code_length(data, length, offset);
		if (code == 0)
		{
			offset++;
			continue;
		}
		if (start != 0 && offset > start)
			sent |= rtp_h264_packetize(&agent->packetizer, data + start, offset - start,
			                            agent->timestamp, send_packet, agent);
		start = offset + code;
		offset = start;
	}
	if (start == 0)
		sent = rtp_h264_packetize(&agent->packetizer, data, length, agent->timestamp, send_packet, agent);
	else if (start < length)
		sent |= rtp_h264_packetize(&agent->packetizer, data + start, length - start, agent->timestamp,
		                           send_packet, agent);
	return sent;
}

static void handle_control(Agent* agent, const NanokvmControlMessage* message)
{
	switch (message->type)
	{
		case NANOKVM_CONTROL_START_STREAM:
			agent->streaming = true;
			agent->wait_for_idr = true;
			(void)fprintf(stderr, "%s: START_STREAM 수신 (%ux%u 원본 H.264 전송 시작)\n", TAG,
			              agent->width, agent->height);
			break;
		case NANOKVM_CONTROL_STOP_STREAM:
			agent->streaming = false;
			hid_release_all(&agent->hid);
			(void)fprintf(stderr, "%s: STOP_STREAM 수신\n", TAG);
			break;
		case NANOKVM_CONTROL_IDR_REQUEST:
			agent->wait_for_idr = true;
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
	(void)fprintf(stderr, "Usage: %s -gateway IPv4 [-control-port n] [-video-port n] [-width n] [-height n] [-bitrate n]\n", executable);
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
	if (agent.video_fd < 0 || inet_pton(AF_INET, agent.gateway, &agent.video_address.sin_addr) != 1)
		return 1;
	rtp_h264_packetizer_init(&agent.packetizer, RTP_H264_DEFAULT_MTU, (uint32_t)getpid());
	hid_init(&agent.hid, NULL, NULL, NULL);
	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);
	(void)signal(SIGPIPE, SIG_IGN);
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
		if (!agent.streaming)
			continue;
		uint8_t* data = NULL;
		uint32_t length = 0;
		const int kind = agent.kvm.read_image(agent.width, agent.height, 1, agent.bitrate, &data, &length);
		if (kind < 0 || !data || length == 0)
		{
			if (data)
				(void)agent.kvm.free_data(&data);
			continue;
		}
		if (kind == KVM_FRAME_IDR)
			agent.wait_for_idr = false;
		agent.capture_frames++;
		if ((kind == KVM_FRAME_SPS || kind == KVM_FRAME_PPS || kind == KVM_FRAME_IDR ||
		     (!agent.wait_for_idr && kind == KVM_FRAME_P)))
		{
			if (!send_h264(&agent, data, length))
				agent.dropped_frames++;
		}
		else if (kind == KVM_FRAME_P)
			agent.dropped_frames++;
		agent.timestamp += 9000U;
		(void)agent.kvm.free_data(&data);
	}
	hid_release_all(&agent.hid);
	if (agent.control_fd >= 0)
		(void)close(agent.control_fd);
	if (agent.video_fd >= 0)
		(void)close(agent.video_fd);
	agent.kvm.deinit();
	if (agent.kvm.handle)
		(void)dlclose(agent.kvm.handle);
	return 0;
}
