#include "http_api.h"

#include "input_router.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#define HTTP_API_MAX_HEADER 8192U
#define HTTP_API_MAX_BODY 65536U
#define HTTP_API_MAX_JPEG (8U * 1024U * 1024U)

struct HttpApi
{
	HttpApiConfig config;
	int listener;
	pthread_t thread;
	pthread_mutex_t lock;
	bool lock_ready;
	bool running;
	bool started;
	const AVCodec* jpeg_codec;
	AVCodecContext* jpeg_encoder;
	struct SwsContext* jpeg_scaler;
	AVFrame* jpeg_yuv;
	int jpeg_source_format;
	uint8_t* jpeg;
	size_t jpeg_length;
	uint32_t frame_id;
	uint16_t frame_width;
	uint16_t frame_height;
	InputRouter router;
};

static const char CRLF[] = { 13, 10, 0 };
static const char CRLF2[] = { 13, 10, 13, 10, 0 };

static bool send_all(int fd, const void* data, size_t length)
{
	const uint8_t* bytes = data;
	size_t sent = 0;
	while (sent < length)
	{
		const ssize_t wrote = send(fd, bytes + sent, length - sent, MSG_NOSIGNAL);
		if (wrote < 0)
		{
			if (errno == EINTR)
				continue;
			return false;
		}
		if (wrote == 0)
			return false;
		sent += (size_t)wrote;
	}
	return true;
}

static bool send_response(int fd, int status, const char* reason, const char* type,
                          const void* body, size_t length, const char* extra)
{
	char header[768];
	const int header_length = snprintf(
	    header, sizeof(header),
	    "HTTP/1.1 %d %s%sContent-Type: %s%sContent-Length: %zu%s"
	    "Cache-Control: no-store%sAccess-Control-Allow-Origin: *%s"
	    "Access-Control-Allow-Methods: GET, POST, OPTIONS%s"
	    "Access-Control-Allow-Headers: Content-Type%s%sConnection: close%s",
	    status, reason, CRLF, type, CRLF, length, CRLF, CRLF, CRLF, CRLF, CRLF,
	    extra ? extra : "", CRLF2);
	if (header_length < 0 || (size_t)header_length >= sizeof(header))
		return false;
	return send_all(fd, header, (size_t)header_length) &&
	       (length == 0 || send_all(fd, body, length));
}

static bool send_text(int fd, int status, const char* type, const char* body)
{
	char converted[1024];
	size_t used = 0;
	if (!body)
		body = "";
	for (const char* cursor = body; *cursor && used + 1 < sizeof(converted); cursor++)
		converted[used++] = *cursor == 39 ? 34 : *cursor;
	converted[used] = 0;
	return send_response(fd, status, status == 200 ? "OK" : "Error", type, converted, used, NULL);
}

static bool send_ok(int fd, bool ok)
{
	return send_text(fd, ok ? 200 : 503, "application/json", ok ? "{'ok':true}" : "{'ok':false}");
}

static bool send_error(int fd, int status, const char* error)
{
	char body[160];
	(void)snprintf(body, sizeof(body), "{'ok':false,'error':'%s'}", error);
	return send_text(fd, status, "application/json", body);
}

bool http_api_publish_frame(HttpApi* api, const struct AVFrame* source)
{
	const AVFrame* frame = source;
	if (!api || !api->running || !frame || frame->width <= 0 || frame->height <= 0 ||
	    !api->jpeg_codec)
		return false;
	AVPacket* packet = av_packet_alloc();
	bool ok = false;
	const char* failure = NULL;
	if (!packet)
	{
		failure = "packet";
		goto out;
	}
	if (!api->jpeg_encoder || api->jpeg_encoder->width != frame->width ||
	    api->jpeg_encoder->height != frame->height)
	{
		avcodec_free_context(&api->jpeg_encoder);
		api->jpeg_encoder = avcodec_alloc_context3(api->jpeg_codec);
		if (!api->jpeg_encoder)
		{
			failure = "encoder";
			goto out;
		}
		api->jpeg_encoder->width = frame->width;
		api->jpeg_encoder->height = frame->height;
		api->jpeg_encoder->pix_fmt = AV_PIX_FMT_YUVJ420P;
		api->jpeg_encoder->time_base = (AVRational){ 1, 25 };
		api->jpeg_encoder->color_range = AVCOL_RANGE_JPEG;
		if (av_opt_set_int(api->jpeg_encoder->priv_data, "q", api->config.jpeg_quality, 0) < 0 ||
		    avcodec_open2(api->jpeg_encoder, api->jpeg_codec, NULL) < 0)
		{
			failure = "open";
			avcodec_free_context(&api->jpeg_encoder);
			goto out;
		}
	}
	if (!api->jpeg_yuv || api->jpeg_yuv->width != frame->width ||
	    api->jpeg_yuv->height != frame->height)
	{
		av_frame_free(&api->jpeg_yuv);
		api->jpeg_yuv = av_frame_alloc();
		if (!api->jpeg_yuv)
		{
			failure = "yuv";
			goto out;
		}
		api->jpeg_yuv->format = AV_PIX_FMT_YUVJ420P;
		api->jpeg_yuv->width = frame->width;
		api->jpeg_yuv->height = frame->height;
		api->jpeg_yuv->color_range = AVCOL_RANGE_JPEG;
		if (av_frame_get_buffer(api->jpeg_yuv, 32) < 0)
		{
			failure = "buffer";
			av_frame_free(&api->jpeg_yuv);
			goto out;
		}
	}
	if (!api->jpeg_scaler || api->jpeg_source_format != frame->format ||
	    api->jpeg_encoder->width != frame->width)
	{
		api->jpeg_scaler = sws_getCachedContext(
		    api->jpeg_scaler, frame->width, frame->height, (enum AVPixelFormat)frame->format,
		    frame->width, frame->height, AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, NULL, NULL, NULL);
		api->jpeg_source_format = frame->format;
	}
	if (!api->jpeg_scaler)
	{
		failure = "scaler";
		goto out;
	}
	if (av_frame_make_writable(api->jpeg_yuv) < 0)
	{
		failure = "writable";
		goto out;
	}
	if (sws_scale(api->jpeg_scaler, (const uint8_t* const*)frame->data, frame->linesize,
	              0, frame->height, api->jpeg_yuv->data, api->jpeg_yuv->linesize) != frame->height)
	{
		failure = "scale";
		goto out;
	}
	const int send_status = avcodec_send_frame(api->jpeg_encoder, api->jpeg_yuv);
	if (send_status < 0)
	{
		failure = "send";
		goto out;
	}
	const int receive_status = avcodec_receive_packet(api->jpeg_encoder, packet);
	if (receive_status < 0 || packet->size <= 0 || (size_t)packet->size > HTTP_API_MAX_JPEG)
	{
		failure = "receive";
		goto out;
	}
	uint8_t* copy = malloc((size_t)packet->size);
	if (!copy)
	{
		failure = "copy";
		goto out;
	}
	memcpy(copy, packet->data, (size_t)packet->size);
	pthread_mutex_lock(&api->lock);
	free(api->jpeg);
	api->jpeg = copy;
	api->jpeg_length = (size_t)packet->size;
	api->frame_width = (uint16_t)frame->width;
	api->frame_height = (uint16_t)frame->height;
	api->frame_id++;
	pthread_mutex_unlock(&api->lock);
	ok = true;
out:
	if (!ok && failure)
	{
		char detail[160];
		(void)snprintf(detail, sizeof(detail),
		               "HTTP JPEG encode failed stage=%s %dx%d format=%d",
		               failure, frame->width, frame->height, frame->format);
		fprintf(stderr, "%s\n", detail);
	}
	av_packet_free(&packet);
	return ok;
}

static const char* header_value(const char* request, const char* name)
{
	const size_t name_length = strlen(name);
	const char* line = request;
	while (*line)
	{
		const char* end = strstr(line, CRLF);
		if (!end || end == line)
			break;
		if ((size_t)(end - line) > name_length + 1 &&
		    strncasecmp(line, name, name_length) == 0 && line[name_length] == ':')
		{
			const char* value = line + name_length + 1;
			while (*value == ' ' || *value == '	')
				value++;
			return value;
		}
		line = end + 2;
	}
	return NULL;
}

static const char* json_field(const char* json, const char* key)
{
	char pattern[64];
	if (snprintf(pattern, sizeof(pattern), "'%s'", key) >= (int)sizeof(pattern))
		return NULL;
	const char* found = strstr(json ? json : "", pattern);
	if (!found && json)
	{
		char quoted[64];
		const int quoted_length = snprintf(quoted, sizeof(quoted), "'%s'", key);
		if (quoted_length < 3 || (size_t)quoted_length >= sizeof(quoted))
			return NULL;
		quoted[0] = '"';
		quoted[quoted_length - 1] = '"';
		found = strstr(json, quoted);
		if (found)
			memcpy(pattern, quoted, (size_t)quoted_length + 1);
	}
	if (!found)
		return NULL;
	found = strchr(found + strlen(pattern), ':');
	if (!found)
		return NULL;
	found++;
	while (*found == ' ' || *found == '	')
		found++;
	return found;
}

static bool json_bool(const char* json, const char* key, bool fallback)
{
	const char* found = json_field(json, key);
	if (!found)
		return fallback;
	if (strncmp(found, "true", 4) == 0)
		return true;
	if (strncmp(found, "false", 5) == 0)
		return false;
	return fallback;
}

static long json_long(const char* json, const char* key, long fallback, bool* present)
{
	const char* found = json_field(json, key);
	if (!found)
	{
		if (present)
			*present = false;
		return fallback;
	}
	char* end = NULL;
	const long value = strtol(found, &end, 10);
	if (end == found)
	{
		if (present)
			*present = false;
		return fallback;
	}
	if (present)
		*present = true;
	return value;
}

static bool send_router_message(HttpApi* api, const InputRouterMessage* message)
{
	return message->type == 0 ||
	       (api->config.send_control &&
	        api->config.send_control(api->config.context, message->type, message->payload,
	                                 message->length));
}

static void note_input_busy(HttpApi* api)
{
	api->config.input_busy = api->router.pointer_buttons != 0 ||
	                         api->router.keyboard_modifiers != 0 ||
	                         api->router.control_space_down;
}

static bool handle_key(HttpApi* api, const char* body, int fd)
{
	bool present = false;
	const long code = json_long(body, "code", -1, &present);
	if (!present || code < 0 || code > 255)
		return send_error(fd, 400, "code");
	const bool extended = json_bool(body, "extended", false);
	const bool release = json_bool(body, "release", false) || json_bool(body, "up", false);
	InputRouterMessage message;
	pthread_mutex_lock(&api->lock);
	input_router_keyboard(&api->router, (uint8_t)code, extended, release, &message);
	note_input_busy(api);
	pthread_mutex_unlock(&api->lock);
	return send_ok(fd, send_router_message(api, &message));
}

static bool decode_json_string(const char* json, const char* key, char* out, size_t out_size)
{
	const char* found = json_field(json, key);
	if (!found || (*found != '"' && *found != 39))
		return false;
	const char quote = *found++;
	size_t used = 0;
	while (*found && *found != quote)
	{
		unsigned char value = (unsigned char)*found++;
		if (value == 92)
		{
			if (*found == 'n')
				value = 10;
			else if (*found == 't')
				value = 9;
			else if (*found == 'u')
				return false;
			else if (*found)
				value = (unsigned char)*found;
			else
				return false;
			found++;
		}
		if (used + 1 >= out_size)
			return false;
		out[used++] = (char)value;
	}
	out[used] = 0;
	return *found == quote;
}

static bool handle_text(HttpApi* api, const char* body, int fd)
{
	char decoded[1024];
	if (!decode_json_string(body, "text", decoded, sizeof(decoded)))
		return send_error(fd, 400, "text");
	const uint8_t* bytes = (const uint8_t*)decoded;
	const size_t length = strlen(decoded);
	size_t index = 0;
	bool ok = true;
	while (index < length && ok)
	{
		uint32_t codepoint = 0;
		size_t consumed = 1;
		if (bytes[index] < 0x80U)
			codepoint = bytes[index];
		else if ((bytes[index] & 0xe0U) == 0xc0U && index + 1 < length)
		{
			codepoint = ((uint32_t)(bytes[index] & 0x1fU) << 6) | (bytes[index + 1] & 0x3fU);
			consumed = 2;
		}
		else if ((bytes[index] & 0xf0U) == 0xe0U && index + 2 < length)
		{
			codepoint = ((uint32_t)(bytes[index] & 0x0fU) << 12) |
			            ((uint32_t)(bytes[index + 1] & 0x3fU) << 6) | (bytes[index + 2] & 0x3fU);
			consumed = 3;
		}
		else
			return send_error(fd, 400, "utf-8");
		if (codepoint == 0 || codepoint > 0xffffU)
			return send_error(fd, 400, "bmp-only");
		InputRouterMessage message;
		pthread_mutex_lock(&api->lock);
		ok = input_router_unicode(&api->router, (uint16_t)codepoint, false, &message);
		pthread_mutex_unlock(&api->lock);
		ok = ok && send_router_message(api, &message);
		index += consumed;
	}
	return send_ok(fd, ok);
}

static void map_pointer(uint16_t frame_width, uint16_t frame_height, uint16_t desktop_width,
                        uint16_t desktop_height, long x, long y, uint16_t* mapped_x,
                        uint16_t* mapped_y)
{
	const uint16_t source_width = frame_width ? frame_width : desktop_width;
	const uint16_t source_height = frame_height ? frame_height : desktop_height;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (source_width > 1 && x >= source_width)
		x = source_width - 1;
	if (source_height > 1 && y >= source_height)
		y = source_height - 1;
	if (source_width <= 1 || desktop_width <= 1)
		*mapped_x = 0;
	else
		*mapped_x = (uint16_t)((x * (desktop_width - 1L)) / (source_width - 1L));
	if (source_height <= 1 || desktop_height <= 1)
		*mapped_y = 0;
	else
		*mapped_y = (uint16_t)((y * (desktop_height - 1L)) / (source_height - 1L));
}

static bool handle_mouse(HttpApi* api, const char* body, int fd)
{
	bool x_present = false;
	bool y_present = false;
	const long x = json_long(body, "x", 0, &x_present);
	const long y = json_long(body, "y", 0, &y_present);
	if (!x_present || !y_present || x < 0 || y < 0 || x > UINT16_MAX || y > UINT16_MAX)
		return send_error(fd, 400, "x,y");
	bool buttons_present = false;
	const long buttons = json_long(body, "buttons", 0, &buttons_present);
	const bool down = json_bool(body, "down", true);
	uint16_t flags = 0;
	if (buttons_present)
	{
		if (buttons & 1)
			flags |= 0x1000;
		if (buttons & 2)
			flags |= 0x2000;
		if (buttons & 4)
			flags |= 0x4000;
		if (down && flags != 0)
			flags |= 0x8000;
	}
	uint16_t mapped_x = 0;
	uint16_t mapped_y = 0;
	uint16_t desktop_width = 0;
	uint16_t desktop_height = 0;
	InputRouterMessage message;
	pthread_mutex_lock(&api->lock);
	map_pointer(api->frame_width, api->frame_height, api->router.width, api->router.height, x, y,
	            &mapped_x, &mapped_y);
	desktop_width = api->router.width;
	desktop_height = api->router.height;
	input_router_absolute_pointer(&api->router, mapped_x, mapped_y, flags, false, &message);
	note_input_busy(api);
	pthread_mutex_unlock(&api->lock);
	const bool sent = send_router_message(api, &message);
	char json[192];
	(void)snprintf(json, sizeof(json),
	               "{'ok':%s,'x':%u,'y':%u,'source_x':%ld,'source_y':%ld,'width':%u,'height':%u}",
	               sent ? "true" : "false", mapped_x, mapped_y, x, y, desktop_width,
	               desktop_height);
	return send_text(fd, sent ? 200 : 503, "application/json", json);
}

static bool handle_wheel(HttpApi* api, const char* body, int fd)
{
	const long dy = json_long(body, "dy", 0, NULL);
	const long dx = json_long(body, "dx", 0, NULL);
	if (dy == 0 && dx == 0)
		return send_error(fd, 400, "dx,dy");
	const long raw = dy != 0 ? dy : dx;
	const long steps = raw > 120 ? 120 : raw < -120 ? -120 : raw;
	const uint16_t magnitude = (uint16_t)(steps < 0 ? -steps : steps);
	uint16_t flags = (uint16_t)((dy != 0 ? 0x0200 : 0x0400) | (magnitude & 0x01ffU));
	if (steps < 0)
		flags |= 0x0100;
	InputRouterMessage message;
	pthread_mutex_lock(&api->lock);
	input_router_absolute_pointer(&api->router, 0, 0, flags, true, &message);
	pthread_mutex_unlock(&api->lock);
	return send_ok(fd, send_router_message(api, &message));
}

static bool handle_release(HttpApi* api, int fd)
{
	InputRouterMessage message;
	pthread_mutex_lock(&api->lock);
	input_router_release_all(&api->router, &message);
	note_input_busy(api);
	pthread_mutex_unlock(&api->lock);
	return send_ok(fd, send_router_message(api, &message));
}

static bool copy_jpeg(HttpApi* api, uint8_t** jpeg, size_t* length, uint32_t* id)
{
	pthread_mutex_lock(&api->lock);
	*length = api->jpeg_length;
	*id = api->frame_id;
	*jpeg = NULL;
	if (api->jpeg && api->jpeg_length)
	{
		*jpeg = malloc(api->jpeg_length);
		if (*jpeg)
			memcpy(*jpeg, api->jpeg, api->jpeg_length);
	}
	pthread_mutex_unlock(&api->lock);
	return *jpeg != NULL;
}

static bool handle_snapshot(HttpApi* api, int fd)
{
	uint8_t* jpeg = NULL;
	size_t length = 0;
	uint32_t id = 0;
	if (!copy_jpeg(api, &jpeg, &length, &id))
		return send_error(fd, 503, "no-frame");
	char extra[96];
	(void)snprintf(extra, sizeof(extra), "X-Frame-Id: %u%s", id, CRLF);
	const bool sent = send_response(fd, 200, "OK", "image/jpeg", jpeg, length, extra);
	free(jpeg);
	return sent;
}

static bool handle_status(HttpApi* api, int fd)
{
	pthread_mutex_lock(&api->lock);
	const uint32_t id = api->frame_id;
	const uint16_t width = api->frame_width ? api->frame_width : api->config.width;
	const uint16_t height = api->frame_height ? api->frame_height : api->config.height;
	const bool has_frame = api->jpeg_length > 0;
	pthread_mutex_unlock(&api->lock);
	char json[256];
	(void)snprintf(json, sizeof(json),
	               "{'ok':true,'frame_id':%u,'width':%u,'height':%u,'has_frame':%s,"
	               "'desktop_width':%u,'desktop_height':%u}",
	               id, width, height, has_frame ? "true" : "false",
	               api->config.width, api->config.height);
	return send_text(fd, 200, "application/json", json);
}

static const char VIEWER_PAGE[] =
	"<!doctype html><meta charset=utf-8><title>NanoKVM</title>"
	"<style>html,body{margin:0;background:#111;color:#eee;font:14px sans-serif}"
	"#bar{padding:8px}img{max-width:100%;display:block;cursor:crosshair;background:#000}</style>"
	"<div id=bar>Click the image to move and click. Typed keys go to the host.</div>"
	"<img id=screen alt=screen><script>"
	"const img=document.getElementById('screen');let last=0;"
	"function refresh(){img.src='/snapshot.jpg?t='+Date.now();}"
	"img.onload=()=>setTimeout(refresh,80);img.onerror=()=>setTimeout(refresh,400);refresh();"
	"function pos(ev){const r=img.getBoundingClientRect();"
	"return {x:Math.max(0,Math.round((ev.clientX-r.left)*img.naturalWidth/r.width)),"
	"y:Math.max(0,Math.round((ev.clientY-r.top)*img.naturalHeight/r.height));}}"
	"function post(path,body){return fetch(path,{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)});}"
	"img.addEventListener('contextmenu',ev=>ev.preventDefault());"
	"img.addEventListener('mousemove',ev=>{const now=Date.now();if(now-last<30)return;last=now;post('/api/mouse',pos(ev));});"
	"img.addEventListener('mousedown',ev=>{const b=pos(ev);b.buttons=ev.button===2?2:ev.button===1?4:1;b.down=true;post('/api/mouse',b);});"
	"img.addEventListener('mouseup',ev=>{const b=pos(ev);b.buttons=ev.button===2?2:ev.button===1?4:1;b.down=false;post('/api/mouse',b);});"
	"img.addEventListener('wheel',ev=>{ev.preventDefault();post('/api/wheel',{dy:Math.sign(ev.deltaY)*120,dx:Math.sign(ev.deltaX)*120});},{passive:false});"
	"const map={Escape:1,Digit1:2,Digit2:3,Digit3:4,Digit4:5,Digit5:6,Digit6:7,Digit7:8,Digit8:9,Digit9:10,Digit0:11,Minus:12,Equal:13,Backspace:14,Tab:15,KeyQ:16,KeyW:17,KeyE:18,KeyR:19,KeyT:20,KeyY:21,KeyU:22,KeyI:23,KeyO:24,KeyP:25,BracketLeft:26,BracketRight:27,Enter:28,ControlLeft:29,KeyA:30,KeyS:31,KeyD:32,KeyF:33,KeyG:34,KeyH:35,KeyJ:36,KeyK:37,KeyL:38,Semicolon:39,Quote:40,Backquote:41,ShiftLeft:42,Backslash:43,KeyZ:44,KeyX:45,KeyC:46,KeyV:47,KeyB:48,KeyN:49,KeyM:50,Comma:51,Period:52,Slash:53,ShiftRight:54,AltLeft:56,Space:57,CapsLock:58,F1:59,F2:60,F3:61,F4:62,F5:63,F6:64,F7:65,F8:66,F9:67,F10:68};"
	"const ext={ControlRight:29,AltRight:56,MetaLeft:91,MetaRight:92,Insert:82,Delete:83,Home:71,End:79,PageUp:73,PageDown:81,ArrowUp:72,ArrowLeft:75,ArrowDown:80,ArrowRight:77};"
	"function keyBody(ev,release){const extended=Object.prototype.hasOwnProperty.call(ext,ev.code);const code=extended?ext[ev.code]:map[ev.code];return code?{code,extended,release}:null;}"
	"window.addEventListener('keydown',ev=>{const b=keyBody(ev,false);if(!b||ev.repeat)return;ev.preventDefault();post('/api/key',b);});"
	"window.addEventListener('keyup',ev=>{const b=keyBody(ev,true);if(!b)return;ev.preventDefault();post('/api/key',b);});"
	"window.addEventListener('blur',()=>post('/api/release',{}));"
	"</script>";

static bool handle_request(HttpApi* api, int fd, const char* request, const char* body)
{
	char method[8] = { 0 };
	char path[256] = { 0 };
	if (sscanf(request, "%7s %255s", method, path) != 2)
		return send_error(fd, 400, "request");
	char* query = strchr(path, '?');
	if (query)
		*query = 0;
	if (strcmp(method, "OPTIONS") == 0)
		return send_response(fd, 204, "No Content", "text/plain", "", 0, NULL);
	if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0))
		return send_response(fd, 200, "OK", "text/html; charset=utf-8", VIEWER_PAGE,
		                     sizeof(VIEWER_PAGE) - 1, NULL);
	if (strcmp(method, "GET") == 0 && strcmp(path, "/snapshot.jpg") == 0)
		return handle_snapshot(api, fd);
	if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0)
		return handle_status(api, fd);
	if (strcmp(method, "POST") != 0)
		return send_error(fd, 404, "not-found");
	if (strcmp(path, "/api/key") == 0)
		return handle_key(api, body, fd);
	if (strcmp(path, "/api/text") == 0)
		return handle_text(api, body, fd);
	if (strcmp(path, "/api/mouse") == 0)
		return handle_mouse(api, body, fd);
	if (strcmp(path, "/api/wheel") == 0)
		return handle_wheel(api, body, fd);
	if (strcmp(path, "/api/release") == 0)
		return handle_release(api, fd);
	return send_error(fd, 404, "not-found");
}

static void serve_client(HttpApi* api, int fd)
{
	char request[HTTP_API_MAX_HEADER + 1];
	size_t used = 0;
	while (used < HTTP_API_MAX_HEADER)
	{
		const ssize_t got = recv(fd, request + used, HTTP_API_MAX_HEADER - used, 0);
		if (got < 0)
		{
			if (errno == EINTR)
				continue;
			return;
		}
		if (got == 0)
			return;
		used += (size_t)got;
		request[used] = 0;
		if (strstr(request, CRLF2))
			break;
	}
	char* header_end = strstr(request, CRLF2);
	if (!header_end)
		return;
	*header_end = 0;
	const char* inline_body = header_end + 4;
	const size_t have = used - (size_t)(inline_body - request);
	const char* length_header = header_value(request, "Content-Length");
	const size_t content_length = length_header ? (size_t)strtoul(length_header, NULL, 10) : 0;
	if (content_length > HTTP_API_MAX_BODY)
	{
		(void)send_error(fd, 413, "body");
		return;
	}
	char* body = calloc(1, content_length + 1);
	if (!body)
		return;
	const size_t initial = have < content_length ? have : content_length;
	if (initial)
		memcpy(body, inline_body, initial);
	size_t filled = initial;
	while (filled < content_length)
	{
		const ssize_t got = recv(fd, body + filled, content_length - filled, 0);
		if (got < 0)
		{
			if (errno == EINTR)
				continue;
			free(body);
			return;
		}
		if (got == 0)
		{
			free(body);
			return;
		}
		filled += (size_t)got;
	}
	(void)handle_request(api, fd, request, body);
	free(body);
}

static void* http_thread(void* argument)
{
	HttpApi* api = argument;
	while (api->running)
	{
		struct sockaddr_in peer;
		socklen_t peer_length = sizeof(peer);
		const int fd = accept(api->listener, (struct sockaddr*)&peer, &peer_length);
		if (fd < 0)
		{
			if (!api->running || errno == EBADF || errno == EINVAL)
				break;
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			break;
		}
		struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
		(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
		serve_client(api, fd);
		(void)close(fd);
	}
	return NULL;
}

HttpApi* http_api_create(const HttpApiConfig* config)
{
	if (!config || !config->bind_address || config->port == 0 || !config->width || !config->height)
		return NULL;
	HttpApi* api = calloc(1, sizeof(*api));
	if (!api)
		return NULL;
	api->config = *config;
	api->listener = -1;
	api->jpeg_codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
	if (api->config.jpeg_quality < 2 || api->config.jpeg_quality > 31)
		api->config.jpeg_quality = 8;
	if (pthread_mutex_init(&api->lock, NULL) != 0 || !api->jpeg_codec)
	{
		free(api);
		return NULL;
	}
	api->lock_ready = true;
	input_router_init(&api->router, config->width, config->height, config->swap_alt_command);
	return api;
}

void http_api_destroy(HttpApi* api)
{
	if (!api)
		return;
	http_api_stop(api);
	free(api->jpeg);
	sws_freeContext(api->jpeg_scaler);
	av_frame_free(&api->jpeg_yuv);
	avcodec_free_context(&api->jpeg_encoder);
	if (api->lock_ready)
		pthread_mutex_destroy(&api->lock);
	free(api);
}

bool http_api_start(HttpApi* api)
{
	if (!api || api->started)
		return false;
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return false;
	const int enabled = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
	struct sockaddr_in address = { 0 };
	address.sin_family = AF_INET;
	address.sin_port = htons(api->config.port);
	if (inet_pton(AF_INET, api->config.bind_address, &address.sin_addr) != 1 ||
	    bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0 || listen(fd, 16) != 0)
	{
		(void)close(fd);
		return false;
	}
	api->listener = fd;
	api->running = true;
	if (pthread_create(&api->thread, NULL, http_thread, api) != 0)
	{
		api->running = false;
		(void)close(fd);
		api->listener = -1;
		return false;
	}
	api->started = true;
	if (api->config.set_stream)
		(void)api->config.set_stream(api->config.context, true);
	return true;
}

bool http_api_input_idle(const HttpApi* api)
{
	return !api || !api->config.input_busy;
}

void http_api_set_desktop(HttpApi* api, uint16_t width, uint16_t height)
{
	if (!api || width == 0 || height == 0)
		return;
	pthread_mutex_lock(&api->lock);
	api->config.width = width;
	api->config.height = height;
	api->router.width = width;
	api->router.height = height;
	pthread_mutex_unlock(&api->lock);
}

void http_api_stop(HttpApi* api)
{
	if (!api || !api->started)
		return;
	api->running = false;
	if (api->listener >= 0)
	{
		(void)shutdown(api->listener, SHUT_RDWR);
		(void)close(api->listener);
		api->listener = -1;
	}
	(void)pthread_join(api->thread, NULL);
	api->started = false;
	InputRouterMessage message;
	input_router_release_all(&api->router, &message);
	(void)send_router_message(api, &message);
}

