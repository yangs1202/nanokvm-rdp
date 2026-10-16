#include "http_api.h"

#include <assert.h>
#include <arpa/inet.h>
#include <libavutil/frame.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RESPONSE_CAPACITY (1024U * 1024U)

typedef struct
{
	unsigned control_calls;
	unsigned stream_calls;
	bool stream_requested;
} TestContext;

static bool send_control(void* context, uint8_t type, const void* payload, uint16_t length)
{
	TestContext* test = context;
	assert(type != 0);
	assert(payload != NULL || length == 0);
	test->control_calls++;
	return true;
}

static bool set_stream(void* context, bool requested)
{
	TestContext* test = context;
	test->stream_calls++;
	test->stream_requested = requested;
	return true;
}

static uint16_t free_port(void)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(fd >= 0);
	struct sockaddr_in address = { 0 };
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	assert(bind(fd, (struct sockaddr*)&address, sizeof(address)) == 0);
	socklen_t length = sizeof(address);
	assert(getsockname(fd, (struct sockaddr*)&address, &length) == 0);
	const uint16_t port = ntohs(address.sin_port);
	assert(close(fd) == 0);
	return port;
}

static size_t request(uint16_t port, const char* method, const char* path,
	                  const char* body, char* response, size_t capacity)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	assert(fd >= 0);
	struct sockaddr_in address = { 0 };
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(connect(fd, (struct sockaddr*)&address, sizeof(address)) == 0);
	char request_text[512];
	const size_t body_length = body ? strlen(body) : 0;
	const int request_length = snprintf(
	    request_text, sizeof(request_text),
	    "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n"
	    "Content-Length: %zu\r\n\r\n%s",
	    method, path, body_length, body ? body : "");
	assert(request_length > 0 && (size_t)request_length < sizeof(request_text));
	size_t sent = 0;
	while (sent < (size_t)request_length)
	{
		const ssize_t result = send(fd, request_text + sent, (size_t)request_length - sent, 0);
		assert(result > 0);
		sent += (size_t)result;
	}
	shutdown(fd, SHUT_WR);
	size_t used = 0;
	while (used < capacity)
	{
		const ssize_t got = recv(fd, response + used, capacity - used, 0);
		if (got == 0)
			break;
		assert(got > 0);
		used += (size_t)got;
	}
	assert(close(fd) == 0);
	assert(used < capacity);
	response[used] = 0;
	return used;
}

static const char* response_body(const char* response)
{
	const char* body = strstr(response, "\r\n\r\n");
	assert(body != NULL);
	return body + 4;
}

static unsigned response_frame_id(const char* response)
{
	const char* header = strstr(response, "X-Frame-Id: ");
	assert(header != NULL);
	return (unsigned)strtoul(header + strlen("X-Frame-Id: "), NULL, 10);
}

/* The viewer script is a C string literal, so a typo only shows up in the
 * browser. When NANOKVM_RDP_VIEWER_SCRIPT is set, write the served script
 * there so CTest can syntax-check it with node. */
static void export_viewer_script(const char* response)
{
	const char* path = getenv("NANOKVM_RDP_VIEWER_SCRIPT");
	const char* start = strstr(response_body(response), "<script>");
	assert(start != NULL);
	start += strlen("<script>");
	const char* end = strstr(start, "</script>");
	assert(end != NULL && end > start);
	if (!path || path[0] == 0)
		return;
	FILE* file = fopen(path, "w");
	assert(file != NULL);
	assert(fwrite(start, 1, (size_t)(end - start), file) == (size_t)(end - start));
	assert(fclose(file) == 0);
}

int main(void)
{
	const uint16_t port = free_port();
	TestContext context = { 0 };
	const HttpApiConfig config = {
		.bind_address = "127.0.0.1",
		.port = port,
		.width = 64,
		.height = 48,
		.jpeg_quality = 8,
		.context = &context,
		.send_control = send_control,
		.set_stream = set_stream,
	};
	HttpApi* api = http_api_create(&config);
	assert(api != NULL);
	assert(http_api_start(api));
	assert(context.stream_calls == 1 && context.stream_requested);
	char response[RESPONSE_CAPACITY];

	(void)request(port, "GET", "/", NULL, response, sizeof(response));
	assert(strncmp(response, "HTTP/1.1 200 OK", 15) == 0);
	assert(strstr(response, "Content-Type: text/html") != NULL);
	export_viewer_script(response);
	(void)request(port, "GET", "/snapshot.jpg", NULL, response, sizeof(response));
	assert(strncmp(response, "HTTP/1.1 503", 12) == 0);
	assert(strstr(response_body(response), "{\"ok\":false,\"error\":\"no-frame\"}") != NULL);

	AVFrame* frame = av_frame_alloc();
	assert(frame != NULL);
	frame->format = AV_PIX_FMT_YUV420P;
	frame->width = 64;
	frame->height = 48;
	assert(av_frame_get_buffer(frame, 32) == 0);
	assert(av_frame_make_writable(frame) == 0);
	for (int y = 0; y < frame->height; y++)
		memset(frame->data[0] + y * frame->linesize[0], 96, (size_t)frame->width);
	for (int y = 0; y < frame->height / 2; y++)
	{
		memset(frame->data[1] + y * frame->linesize[1], 128, (size_t)frame->width / 2);
		memset(frame->data[2] + y * frame->linesize[2], 128, (size_t)frame->width / 2);
	}
	assert(http_api_publish_frame(api, frame));
	(void)request(port, "GET", "/snapshot.jpg", NULL, response, sizeof(response));
	assert(strncmp(response, "HTTP/1.1 200 OK", 15) == 0);
	assert(strstr(response, "Content-Type: image/jpeg\r\n") != NULL);
	const unsigned first_id = response_frame_id(response);
	const uint8_t* jpeg = (const uint8_t*)response_body(response);
	assert(jpeg[0] == 0xff && jpeg[1] == 0xd8);

	(void)request(port, "GET", "/snapshot.jpg", NULL, response, sizeof(response));
	assert(response_frame_id(response) == first_id);
	assert(((const uint8_t*)response_body(response))[0] == 0xff);

	assert(http_api_publish_frame(api, frame));
	(void)request(port, "GET", "/snapshot.jpg", NULL, response, sizeof(response));
	assert(response_frame_id(response) == first_id + 1);

	(void)request(port, "GET", "/api/status", NULL, response, sizeof(response));
	assert(strncmp(response, "HTTP/1.1 200 OK", 15) == 0);
	assert(strstr(response_body(response), "\"frame_id\":2") != NULL);
	assert(strstr(response_body(response), "\"has_frame\":true") != NULL);
	assert(strstr(response_body(response), "\"desktop_width\":64") != NULL);

	(void)request(port, "POST", "/api/mouse", "{\"x\":10,\"y\":10,\"buttons\":1,\"down\":true}",
	              response, sizeof(response));
	assert(strncmp(response, "HTTP/1.1 200 OK", 15) == 0);
	assert(!http_api_input_idle(api));
	(void)request(port, "POST", "/api/mouse", "{\"x\":10,\"y\":10,\"buttons\":1,\"down\":false}",
	              response, sizeof(response));
	assert(strncmp(response, "HTTP/1.1 200 OK", 15) == 0);
	assert(http_api_input_idle(api));
	assert(context.control_calls >= 2);

	av_frame_free(&frame);
	http_api_stop(api);
	assert(context.stream_calls == 1);
	http_api_destroy(api);
	return 0;
}
