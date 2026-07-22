#include "ffmpeg_decoder.h"
#include "foldvnc_client.h"
#include "h264.h"
#include "hid.h"

#include <freerdp/channels/channels.h>
#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/codec/color.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/codec/rfx.h>
#include <freerdp/freerdp.h>
#include <freerdp/input.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/settings.h>
#include <freerdp/update.h>

#include <winpr/crt.h>
#include <winpr/ssl.h>
#include <winpr/sysinfo.h>
#include <winpr/synch.h>
#include <winpr/thread.h>
#include <winpr/stream.h>
#include <winpr/wtsapi.h>
#include <winpr/winsock.h>

#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "nanokvm-rdp"
#define DEFAULT_WIDTH 1920U
#define DEFAULT_HEIGHT 1080U
#define DEFAULT_BITRATE 3000U
#define MAX_EVENT_HANDLES 32U

enum kvm_frame_kind
{
	KVM_FRAME_MJPEG = 0,
	KVM_FRAME_SPS = 1,
	KVM_FRAME_PPS = 2,
	KVM_FRAME_IDR = 3,
	KVM_FRAME_P = 4,
};

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
	const char* bind_address;
	uint16_t port;
	const char* certificate;
	const char* private_key;
	const char* keyboard;
	const char* mouse;
	const char* touch;
	uint16_t width;
	uint16_t height;
	uint16_t bitrate;
	bool direct_gfx;
} ServerConfig;

typedef struct Server Server;
typedef struct Client Client;

struct Server
{
	ServerConfig config;
	KvmApi kvm;
	CRITICAL_SECTION lock;
	Client* active;
};

struct Client
{
	rdpContext context;
	Server* server;
	freerdp_peer* peer;
	HANDLE vcm;
	RdpgfxServerContext* gfx;
	HANDLE video_thread;
	RFX_CONTEXT* rfx;
	NSC_CONTEXT* nsc;
	wStream* bitmap_stream;
	CRITICAL_SECTION lock;
	HidState hid;
	bool stopping;
	bool gfx_ready;
	bool gfx_opened;
	bool inflight;
	bool need_idr;
	uint64_t gfx_wait_started_at;
	uint64_t gfx_opened_at;
	uint32_t inflight_frame_id;
	uint32_t next_frame_id;
	uint64_t sent_at;
	uint8_t* sps;
	size_t sps_length;
	uint8_t* pps;
	size_t pps_length;
	bool bitmap_uses_rfx;
	uint32_t bitmap_frames;
	uint64_t bitmap_last_sent_at;
	bool keyboard_input_logged;
	bool pointer_input_logged;
	bool wheel_input_logged;
};

static volatile sig_atomic_t stop_requested = 0;

static void log_message(const char* level, const char* message)
{
	(void)fprintf(stderr, "%s: %s: %s\n", TAG, level, message);
}

static void on_signal(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static uint64_t monotonic_milliseconds(void)
{
	return (uint64_t)GetTickCount64();
}

static bool copy_bytes(uint8_t** destination, size_t* destination_length, const uint8_t* source,
	                      size_t source_length)
{
	uint8_t* next = NULL;
	if (source_length > 0)
	{
		next = malloc(source_length);
		if (!next)
			return false;
		memcpy(next, source, source_length);
	}
	free(*destination);
	*destination = next;
	*destination_length = source_length;
	return true;
}

static bool kvm_load(KvmApi* api)
{
	if (api->handle)
		return true;
	const char* candidates[] = {
		"/root/foldvnc/dl_lib/libkvm.so",
		"/kvmapp/server/dl_lib/libkvm.so",
		"/tmp/server/dl_lib/libkvm.so",
		"libkvm.so",
	};
	for (size_t index = 0; index < ARRAYSIZE(candidates); index++)
	{
		api->handle = dlopen(candidates[index], RTLD_NOW | RTLD_LOCAL);
		if (api->handle)
			break;
	}
	if (!api->handle)
	{
		log_message("ERROR", "libkvm.so를 열 수 없습니다");
		return false;
	}

	*(void**)(&api->init) = dlsym(api->handle, "kvmv_init");
	*(void**)(&api->read_image) = dlsym(api->handle, "kvmv_read_img");
	*(void**)(&api->free_data) = dlsym(api->handle, "free_kvmv_data");
	*(void**)(&api->set_frame_detect) = dlsym(api->handle, "set_frame_detact");
	if (!api->init || !api->read_image || !api->free_data || !api->set_frame_detect)
	{
		log_message("ERROR", "libkvm.so 필수 symbol을 찾을 수 없습니다");
		return false;
	}
	api->init(0);
	api->set_frame_detect(0);
	return true;
}

static bool client_should_stop(Client* client)
{
	bool stopping = false;
	EnterCriticalSection(&client->lock);
	stopping = client->stopping;
	LeaveCriticalSection(&client->lock);
	return stopping;
}

static void client_stop(Client* client)
{
	EnterCriticalSection(&client->lock);
	client->stopping = true;
	LeaveCriticalSection(&client->lock);
}

static bool client_avc420_supported(const RDPGFX_CAPS_ADVERTISE_PDU* advertise,
	                                RDPGFX_CAPSET* selected)
{
	for (UINT32 index = 0; index < advertise->capsSetCount; index++)
	{
		const RDPGFX_CAPSET* current = &advertise->capsSets[index];
		if (current->version == RDPGFX_CAPVERSION_81 &&
		    (current->flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED) != 0)
		{
			*selected = *current;
			return true;
		}
		if (current->version >= RDPGFX_CAPVERSION_10 &&
		    (current->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED) == 0)
		{
			*selected = *current;
			return true;
		}
	}
	return false;
}

static UINT on_gfx_caps_advertise(RdpgfxServerContext* gfx,
	                              const RDPGFX_CAPS_ADVERTISE_PDU* advertise)
{
	Client* client = (Client*)gfx->custom;
	RDPGFX_CAPSET selected = WINPR_C_ARRAY_INIT;
	RDPGFX_CAPS_CONFIRM_PDU confirm = WINPR_C_ARRAY_INIT;
	RDPGFX_CREATE_SURFACE_PDU surface = WINPR_C_ARRAY_INIT;
	RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU map = WINPR_C_ARRAY_INIT;
	UINT error = CHANNEL_RC_OK;

	if (!client_avc420_supported(advertise, &selected))
	{
		log_message("ERROR", "client가 RDPGFX AVC420을 지원하지 않아 session을 종료합니다");
		client_stop(client);
		return ERROR_NOT_SUPPORTED;
	}

	confirm.capsSet = &selected;
	if (!gfx->CapsConfirm || (error = gfx->CapsConfirm(gfx, &confirm)) != CHANNEL_RC_OK)
		return error ? error : ERROR_INTERNAL_ERROR;

	surface.surfaceId = 1;
	surface.width = client->server->config.width;
	surface.height = client->server->config.height;
	surface.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;
	if (!gfx->CreateSurface || (error = gfx->CreateSurface(gfx, &surface)) != CHANNEL_RC_OK)
		return error ? error : ERROR_INTERNAL_ERROR;

	map.surfaceId = surface.surfaceId;
	map.outputOriginX = 0;
	map.outputOriginY = 0;
	map.reserved = 0;
	if (!gfx->MapSurfaceToOutput || (error = gfx->MapSurfaceToOutput(gfx, &map)) != CHANNEL_RC_OK)
		return error ? error : ERROR_INTERNAL_ERROR;

	EnterCriticalSection(&client->lock);
	client->gfx_ready = true;
	client->need_idr = true;
	LeaveCriticalSection(&client->lock);
	log_message("INFO", "RDPGFX AVC420 capability 확인 및 surface 초기화 완료");
	return CHANNEL_RC_OK;
}

static UINT on_gfx_frame_ack(RdpgfxServerContext* gfx,
	                         const RDPGFX_FRAME_ACKNOWLEDGE_PDU* acknowledge)
{
	Client* client = (Client*)gfx->custom;
	EnterCriticalSection(&client->lock);
	if (client->inflight && acknowledge->frameId >= client->inflight_frame_id)
		client->inflight = false;
	if (acknowledge->queueDepth == SUSPEND_FRAME_ACKNOWLEDGEMENT)
		client->need_idr = true;
	LeaveCriticalSection(&client->lock);
	return CHANNEL_RC_OK;
}

static bool client_can_send(Client* client)
{
	bool can_send = false;
	EnterCriticalSection(&client->lock);
	if (client->inflight && monotonic_milliseconds() - client->sent_at > 1000U)
	{
		client->inflight = false;
		client->need_idr = true;
		log_message("WARN", "RDPGFX frame acknowledgement timeout; IDR 동기화를 재시도합니다");
	}
	can_send = client->gfx_ready && !client->inflight && !client->stopping;
	LeaveCriticalSection(&client->lock);
	return can_send;
}

static bool make_idr_payload(Client* client, const uint8_t* data, size_t length, uint8_t** owned,
	                           const uint8_t** payload, size_t* payload_length)
{
	const bool has_sps = h264_contains_nal_type(data, length, 7);
	const bool has_pps = h264_contains_nal_type(data, length, 8);
	const size_t sps_size = has_sps ? 0 : h264_annexb_size(client->sps, client->sps_length);
	const size_t pps_size = has_pps ? 0 : h264_annexb_size(client->pps, client->pps_length);
	if ((sps_size != 0 && !client->sps) || (pps_size != 0 && !client->pps))
		return false;
	if (sps_size == 0 && pps_size == 0)
	{
		*payload = data;
		*payload_length = length;
		return true;
	}
	uint8_t* combined = malloc(sps_size + pps_size + length);
	if (!combined)
		return false;
	size_t offset = 0;
	if (sps_size != 0)
		offset += h264_copy_annexb(combined + offset, client->sps, client->sps_length);
	if (pps_size != 0)
		offset += h264_copy_annexb(combined + offset, client->pps, client->pps_length);
	memcpy(combined + offset, data, length);
	*owned = combined;
	*payload = combined;
	*payload_length = offset + length;
	return true;
}

static bool send_avc420_frame(Client* client, const uint8_t* data, size_t length)
{
	RECTANGLE_16 rect = { .left = 0, .top = 0, .right = client->server->config.width,
		.bottom = client->server->config.height };
	RDPGFX_H264_QUANT_QUALITY quality = { .qpVal = 0, .qualityVal = 100, .qp = 0, .r = 0, .p = 0 };
	RDPGFX_AVC420_BITMAP_STREAM avc = WINPR_C_ARRAY_INIT;
	RDPGFX_SURFACE_COMMAND command = WINPR_C_ARRAY_INIT;
	RDPGFX_START_FRAME_PDU start = WINPR_C_ARRAY_INIT;
	RDPGFX_END_FRAME_PDU end = WINPR_C_ARRAY_INIT;
	UINT error = CHANNEL_RC_OK;

	EnterCriticalSection(&client->lock);
	if (!client->gfx_ready || client->inflight || client->stopping)
	{
		LeaveCriticalSection(&client->lock);
		return true;
	}
	start.frameId = client->next_frame_id++;
	start.timestamp = (UINT32)monotonic_milliseconds();
	end.frameId = start.frameId;
	client->inflight = true;
	client->inflight_frame_id = start.frameId;
	client->sent_at = monotonic_milliseconds();
	LeaveCriticalSection(&client->lock);

	avc.meta.numRegionRects = 1;
	avc.meta.regionRects = &rect;
	avc.meta.quantQualityVals = &quality;
	avc.length = (UINT32)length;
	avc.data = (BYTE*)data;

	command.surfaceId = 1;
	command.codecId = RDPGFX_CODECID_AVC420;
	command.format = PIXEL_FORMAT_BGRX32;
	command.left = 0;
	command.top = 0;
	command.right = client->server->config.width;
	command.bottom = client->server->config.height;
	command.width = client->server->config.width;
	command.height = client->server->config.height;
	command.extra = &avc;

	if (!client->gfx || !client->gfx->SurfaceFrameCommand)
		error = ERROR_INVALID_HANDLE;
	else
		error = client->gfx->SurfaceFrameCommand(client->gfx, &command, &start, &end);
	if (error != CHANNEL_RC_OK)
	{
		EnterCriticalSection(&client->lock);
		client->inflight = false;
		client->need_idr = true;
		LeaveCriticalSection(&client->lock);
		log_message("ERROR", "RDPGFX AVC420 frame 전송 실패");
		return false;
	}
	return true;
}

static DWORD WINAPI video_thread(LPVOID argument)
{
	Client* client = (Client*)argument;
	Server* server = client->server;
	if (!kvm_load(&server->kvm))
	{
		client_stop(client);
		return 0;
	}

	while (!client_should_stop(client))
	{
		if (!client_can_send(client))
		{
			Sleep(5);
			continue;
		}

		uint8_t* data = NULL;
		uint32_t length = 0;
		const int kind = server->kvm.read_image(server->config.width, server->config.height, 1,
		                                        server->config.bitrate, &data, &length);
		if (kind < 0 || !data || length == 0)
		{
			if (data)
				(void)server->kvm.free_data(&data);
			Sleep(2);
			continue;
		}

		if (kind == KVM_FRAME_SPS)
			(void)copy_bytes(&client->sps, &client->sps_length, data, length);
		else if (kind == KVM_FRAME_PPS)
			(void)copy_bytes(&client->pps, &client->pps_length, data, length);

		bool should_send = kind == KVM_FRAME_IDR || kind == KVM_FRAME_P;
		bool need_idr = true;
		EnterCriticalSection(&client->lock);
		need_idr = client->need_idr;
		LeaveCriticalSection(&client->lock);
		if (kind == KVM_FRAME_IDR)
		{
			EnterCriticalSection(&client->lock);
			client->need_idr = false;
			LeaveCriticalSection(&client->lock);
		}
		if (kind == KVM_FRAME_P && need_idr)
			should_send = false;

		if (should_send)
		{
			uint8_t* owned = NULL;
			const uint8_t* payload = data;
			size_t payload_length = length;
			bool payload_ok = true;
			if (kind == KVM_FRAME_IDR)
				payload_ok = make_idr_payload(client, data, length, &owned, &payload, &payload_length);
			if (!payload_ok || !send_avc420_frame(client, payload, payload_length))
				client_stop(client);
			free(owned);
		}
		(void)server->kvm.free_data(&data);
	}
	return 0;
}

static bool bitmap_stream_rfx_supported(const rdpSettings* settings)
{
	const uint32_t supported =
	    freerdp_settings_get_uint32(settings, FreeRDP_SurfaceCommandsSupported);
	return freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec) &&
	       freerdp_settings_get_uint32(settings, FreeRDP_RemoteFxCodecId) != 0 &&
	       (supported & SURFCMDS_STREAM_SURFACE_BITS) != 0;
}

static bool bitmap_stream_nsc_supported(const rdpSettings* settings)
{
	const uint32_t supported =
	    freerdp_settings_get_uint32(settings, FreeRDP_SurfaceCommandsSupported);
	return freerdp_settings_get_bool(settings, FreeRDP_NSCodec) &&
	       freerdp_settings_get_uint32(settings, FreeRDP_NSCodecId) != 0 &&
	       (supported & SURFCMDS_SET_SURFACE_BITS) != 0;
}

static bool send_classic_bitmap_frame(Client* client, const uint8_t* bgra, size_t length)
{
	const uint16_t width = client->server->config.width;
	const uint16_t height = client->server->config.height;
	const size_t expected_length = (size_t)width * height * 4U;
	const rdpSettings* settings = client->context.settings;
	const uint32_t color_depth =
	    settings ? freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth) : 0;
	const uint16_t bits_per_pixel = color_depth == 24 ? 16 : (uint16_t)color_depth;
	const size_t bytes_per_pixel = bits_per_pixel / 8U;
	const size_t row_bytes = (size_t)width * bytes_per_pixel;
	const uint16_t rows_per_rectangle =
	    row_bytes ? WINPR_ASSERTING_INT_CAST(uint16_t, UINT16_MAX / row_bytes) : 0;
	uint8_t* reversed = NULL;

	if (!client->context.update || !client->context.update->BitmapUpdate ||
	    settings == NULL ||
	    (bits_per_pixel != 16 && bits_per_pixel != 32) ||
	    length != expected_length || rows_per_rectangle == 0 || client_should_stop(client))
		return false;

	reversed = malloc(row_bytes * rows_per_rectangle);
	if (!reversed)
		return false;

	for (uint16_t top = 0; top < height;)
	{
		const uint16_t rows = MIN(rows_per_rectangle, (uint16_t)(height - top));
		BITMAP_DATA rectangle = WINPR_C_ARRAY_INIT;
		BITMAP_UPDATE bitmap = WINPR_C_ARRAY_INIT;

		for (uint16_t row = 0; row < rows; row++)
		{
			const size_t source_row = (size_t)(top + rows - row - 1);
			const uint8_t* source = bgra + (source_row * (size_t)width * 4U);
			uint8_t* destination = reversed + ((size_t)row * row_bytes);
			if (bits_per_pixel == 32)
				memcpy(destination, source, row_bytes);
			else
			{
				for (uint16_t column = 0; column < width; column++)
				{
					const uint8_t* pixel = source + ((size_t)column * 4U);
					const uint16_t rgb565 = (uint16_t)(((uint16_t)(pixel[2] >> 3) << 11U) |
					                                  ((uint16_t)(pixel[1] >> 2) << 5U) |
					                                  (uint16_t)(pixel[0] >> 3));
					memcpy(destination + ((size_t)column * 2U), &rgb565, sizeof(rgb565));
				}
			}
		}

		rectangle.destLeft = 0;
		rectangle.destTop = top;
		rectangle.destRight = width - 1;
		rectangle.destBottom = top + rows - 1;
		rectangle.width = width;
		rectangle.height = rows;
		rectangle.bitsPerPixel = bits_per_pixel;
		rectangle.bitmapLength = WINPR_ASSERTING_INT_CAST(uint16_t, row_bytes * rows);
		rectangle.bitmapDataStream = reversed;
		rectangle.compressed = FALSE;
		bitmap.number = 1;
		bitmap.rectangles = &rectangle;
		bitmap.skipCompression = FALSE;

		if (!client->context.update->BitmapUpdate(&client->context, &bitmap))
		{
			free(reversed);
			return false;
		}
		top += rows;
	}

	free(reversed);
	return true;
}

static bool send_bitmap_frame(Client* client, const uint8_t* bgra, size_t length)
{
	const uint16_t width = client->server->config.width;
	const uint16_t height = client->server->config.height;
	const size_t expected_length = (size_t)width * height * 4U;
	rdpSettings* settings = client->context.settings;
	rdpUpdate* update = client->context.update;
	SURFACE_BITS_COMMAND command = WINPR_C_ARRAY_INIT;
	RFX_RECT rect = { .x = 0, .y = 0, .width = width, .height = height };

	if (!settings || !update || length != expected_length || client_should_stop(client))
		return false;
	if (!client->bitmap_uses_rfx && !client->nsc)
		goto sent_classic;
	if (!update->SurfaceBits || !client->bitmap_stream)
		return false;
	Stream_Clear(client->bitmap_stream);
	Stream_ResetPosition(client->bitmap_stream);
	if (client->bitmap_uses_rfx)
	{
		if (!client->rfx || !bitmap_stream_rfx_supported(settings))
			return false;
		rfx_context_set_pixel_format(client->rfx, PIXEL_FORMAT_BGRX32);
		if (!rfx_compose_message(client->rfx, client->bitmap_stream, &rect, 1, bgra, width, height,
		                         (uint32_t)width * 4U))
			return false;
		command.cmdType = CMDTYPE_STREAM_SURFACE_BITS;
		command.bmp.codecID =
		    WINPR_ASSERTING_INT_CAST(uint16_t,
		                             freerdp_settings_get_uint32(settings, FreeRDP_RemoteFxCodecId));
	}
	else
	{
		if (!client->nsc || !bitmap_stream_nsc_supported(settings))
			return false;
		if (!nsc_context_set_parameters(client->nsc, NSC_COLOR_FORMAT, PIXEL_FORMAT_BGRX32) ||
		    !nsc_compose_message(client->nsc, client->bitmap_stream, bgra, width, height,
		                         (uint32_t)width * 4U))
			return false;
		command.cmdType = CMDTYPE_SET_SURFACE_BITS;
		command.bmp.codecID =
		    WINPR_ASSERTING_INT_CAST(uint16_t,
		                             freerdp_settings_get_uint32(settings, FreeRDP_NSCodecId));
	}
	command.destLeft = 0;
	command.destTop = 0;
	command.destRight = width;
	command.destBottom = height;
	command.bmp.bpp = 32;
	command.bmp.flags = 0;
	command.bmp.width = width;
	command.bmp.height = height;
	command.bmp.bitmapDataLength =
	    WINPR_ASSERTING_INT_CAST(uint32_t, Stream_GetPosition(client->bitmap_stream));
	command.bmp.bitmapData = Stream_Buffer(client->bitmap_stream);
	if (!update->SurfaceBits(&client->context, &command))
		return false;

sent_classic:
	if (!client->bitmap_uses_rfx && !client->nsc && !send_classic_bitmap_frame(client, bgra, length))
		return false;
	client->bitmap_frames++;
	if (client->bitmap_frames == 1)
		log_message("INFO", "FoldVNC H.264 → FFmpeg BGRA → RDP bitmap 첫 frame 전송 완료");
	return true;
}

static bool on_decoded_bitmap_frame(void* context, const uint8_t* bgra, size_t length)
{
	Client* client = context;
	const uint64_t now = monotonic_milliseconds();
	if (client->bitmap_frames > 0 && now - client->bitmap_last_sent_at < 1000U)
		return true;
	if (send_bitmap_frame(client, bgra, length))
	{
		client->bitmap_last_sent_at = now;
		return true;
	}
	log_message("ERROR", "RDP bitmap frame 전송 실패");
	client_stop(client);
	return false;
}

static DWORD WINAPI bitmap_video_thread(LPVOID argument)
{
	Client* client = (Client*)argument;
	FoldVncClient foldvnc = { .fd = -1 };
	FfmpegDecoder decoder = { .pid = -1, .input = -1, .output = -1 };
	const uint16_t capture_width = DEFAULT_WIDTH;
	const uint16_t capture_height = DEFAULT_HEIGHT;
	const uint16_t width = client->server->config.width;
	const uint16_t height = client->server->config.height;

	if (!foldvnc_client_connect(&foldvnc, "127.0.0.1", 7890, capture_width, capture_height, 60) ||
	    !ffmpeg_decoder_start(&decoder, width, height, on_decoded_bitmap_frame, client))
	{
		log_message("ERROR", "FoldVNC H.264 backend 또는 FFmpeg decoder를 시작할 수 없습니다");
		client_stop(client);
		goto out;
	}
	log_message("INFO", "FoldVNC H.264 backend와 FFmpeg BGRA decoder 시작 완료");
	while (!client_should_stop(client))
	{
		uint8_t* data = NULL;
		size_t length = 0;
		bool keyframe = false;
		if (!foldvnc_client_read_video(&foldvnc, &data, &length, &keyframe) || !data || length == 0)
		{
			log_message("ERROR", "FoldVNC H.264 frame 수신 실패");
			client_stop(client);
			break;
		}
		const bool pushed = ffmpeg_decoder_push(&decoder, data, length);
		free(data);
		if (!pushed)
		{
			log_message("ERROR", "FFmpeg H.264 decoder 입력 실패");
			client_stop(client);
			break;
		}
		if (keyframe)
			log_message("INFO", "FoldVNC H.264 keyframe 동기화 완료");
	}

out:
	ffmpeg_decoder_stop(&decoder);
	foldvnc_client_close(&foldvnc);
	return 0;
}

static BOOL on_keyboard(rdpInput* input, UINT16 flags, UINT8 code)
{
	Client* client = (Client*)input->context;
	const bool sent = hid_scancode(&client->hid, code, (flags & KBD_FLAGS_EXTENDED) != 0,
	                               (flags & KBD_FLAGS_RELEASE) != 0);
	if (sent && !client->keyboard_input_logged)
	{
		log_message("INFO", "RDP keyboard scancode → USB HID keyboard report 전달 확인");
		client->keyboard_input_logged = true;
	}
	return sent;
}

static BOOL on_unicode_keyboard(rdpInput* input, UINT16 flags, UINT16 code)
{
	(void)input;
	(void)flags;
	(void)code;
	return TRUE;
}

static BOOL on_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	Client* client = (Client*)input->context;
	const rdpSettings* settings = input->context->settings;
	const uint32_t width = freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
	const uint32_t height = freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
	const bool position_ok = hid_absolute(&client->hid, x, y, width, height, flags);
	const bool wheel_ok = hid_wheel(&client->hid, flags);
	if (position_ok && !client->pointer_input_logged)
	{
		log_message("INFO", "RDP absolute pointer → USB HID touch report 전달 확인");
		client->pointer_input_logged = true;
	}
	if (wheel_ok && (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)) != 0 &&
	    !client->wheel_input_logged)
	{
		log_message("INFO", "RDP wheel → USB HID mouse report 전달 확인");
		client->wheel_input_logged = true;
	}
	return position_ok && wheel_ok;
}

static BOOL on_extended_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	return on_mouse(input, flags, x, y);
}

static BOOL on_dvc_creation_status(void* userdata, UINT32 channel_id, INT32 creation_status)
{
	Client* client = (Client*)userdata;
	(void)client;
	char message[128] = { 0 };
	(void)snprintf(message, sizeof(message), "RDP dynamic channel id=%u creation status=%d",
	               channel_id, creation_status);
	log_message(creation_status == 0 ? "INFO" : "ERROR", message);
	return TRUE;
}

static BOOL client_context_new(freerdp_peer* peer, rdpContext* context)
{
	Client* client = (Client*)context;
	Server* server = (Server*)peer->ContextExtra;
	if (!server || !InitializeCriticalSectionAndSpinCount(&client->lock, 4000))
		return FALSE;
	client->server = server;
	client->peer = peer;
	client->need_idr = true;
	hid_init(&client->hid, server->config.keyboard, server->config.mouse, server->config.touch);
	if (server->config.direct_gfx)
	{
		client->vcm = WTSOpenServerA((LPSTR)context);
		if (!client->vcm || client->vcm == INVALID_HANDLE_VALUE)
			goto fail;
		WTSVirtualChannelManagerSetDVCCreationCallback(client->vcm, on_dvc_creation_status, client);
	}

	EnterCriticalSection(&server->lock);
	if (server->active)
	{
		LeaveCriticalSection(&server->lock);
		log_message("WARN", "single-client 제한으로 새 RDP 연결을 거부합니다");
		goto fail;
	}
	server->active = client;
	LeaveCriticalSection(&server->lock);
	return TRUE;

fail:
	if (client->vcm && client->vcm != INVALID_HANDLE_VALUE)
		WTSCloseServer(client->vcm);
	DeleteCriticalSection(&client->lock);
	return FALSE;
}

static void client_context_free(freerdp_peer* peer, rdpContext* context)
{
	(void)peer;
	Client* client = (Client*)context;
	client_stop(client);
	if (client->video_thread)
	{
		(void)WaitForSingleObject(client->video_thread, 3000);
		(void)CloseHandle(client->video_thread);
	}
	if (client->gfx)
		rdpgfx_server_context_free(client->gfx);
	if (client->rfx)
		rfx_context_free(client->rfx);
	if (client->nsc)
		nsc_context_free(client->nsc);
	if (client->bitmap_stream)
		Stream_Free(client->bitmap_stream, TRUE);
	if (client->vcm && client->vcm != INVALID_HANDLE_VALUE)
		WTSCloseServer(client->vcm);
	hid_release_all(&client->hid);
	free(client->sps);
	free(client->pps);
	if (client->server)
	{
		EnterCriticalSection(&client->server->lock);
		if (client->server->active == client)
			client->server->active = NULL;
		LeaveCriticalSection(&client->server->lock);
	}
	DeleteCriticalSection(&client->lock);
	log_message("INFO", "RDP client 연결 종료 및 USB HID release 완료");
}

static bool client_prepare_gfx(Client* client)
{
	if (client->gfx)
		return true;
	client->gfx = rdpgfx_server_context_new(client->vcm);
	if (!client->gfx)
		return false;
	client->gfx->rdpcontext = &client->context;
	client->gfx->custom = client;
	client->gfx->CapsAdvertise = on_gfx_caps_advertise;
	client->gfx->FrameAcknowledge = on_gfx_frame_ack;
	if (!client->gfx->Initialize || !client->gfx->Initialize(client->gfx, FALSE))
		return false;
	client->video_thread = CreateThread(NULL, 0, video_thread, client, 0, NULL);
	if (!client->video_thread)
		return false;
	return true;
}

static bool client_prepare_bitmap(Client* client)
{
	const rdpSettings* settings = client->context.settings;
	if (!settings)
		return false;
	client->bitmap_uses_rfx = bitmap_stream_rfx_supported(settings);
	if (client->bitmap_uses_rfx)
	{
		client->rfx = rfx_context_new_ex(
		    TRUE, freerdp_settings_get_uint32(settings, FreeRDP_ThreadingFlags));
		if (!client->rfx || !rfx_context_reset(client->rfx, client->server->config.width,
		                                      client->server->config.height))
			return false;
	}
	else if (bitmap_stream_nsc_supported(settings))
	{
		client->nsc = nsc_context_new();
		if (!client->nsc)
			return false;
	}
	else
	{
		const uint32_t color_depth = freerdp_settings_get_uint32(settings, FreeRDP_ColorDepth);
		if (color_depth == 24 &&
		    !freerdp_settings_set_uint32((rdpSettings*)settings, FreeRDP_ColorDepth, 16))
			return false;
		if (color_depth != 16 && color_depth != 24 && color_depth != 32)
		{
			log_message("ERROR", "client가 지원하지 않는 classic bitmap 색 깊이를 요청했습니다");
			return false;
		}
		char message[128];
		(void)snprintf(message, sizeof(message),
		               "RemoteFX/NSCodec 없이 %u-bit classic BitmapUpdate 경로를 사용합니다",
		               color_depth == 24 ? 16 : color_depth);
		log_message("INFO", message);
	}
	if (client->bitmap_uses_rfx || client->nsc)
	{
		client->bitmap_stream = Stream_New(NULL, 65536);
		if (!client->bitmap_stream)
			return false;
	}
	client->video_thread = CreateThread(NULL, 0, bitmap_video_thread, client, 0, NULL);
	return client->video_thread != NULL;
}

static BOOL peer_post_connect(freerdp_peer* peer)
{
	Client* client = (Client*)peer->context;
	if (client->server->config.direct_gfx)
	{
		if (!client_prepare_gfx(client))
			return FALSE;
		client->gfx_wait_started_at = monotonic_milliseconds();
		log_message("INFO", "RDP session activation 완료; RDPGFX dynamic channel open 대기 중");
	}
	else
	{
		if (!client_prepare_bitmap(client))
			return FALSE;
		log_message("INFO", "RDP session activation 완료; FoldVNC bitmap backend 시작");
	}
	return TRUE;
}

static bool client_process_dynamic_channels(Client* client)
{
	if (client->gfx_wait_started_at == 0 ||
	    !WTSVirtualChannelManagerIsChannelJoined(client->vcm, DRDYNVC_SVC_CHANNEL_NAME))
		return true;

	/* This only flushes the VCM's local queue; it does not read the peer transport. */
	if (!WTSVirtualChannelManagerCheckFileDescriptor(client->vcm))
		return false;

	if (!client->gfx || client->gfx_opened ||
	    WTSVirtualChannelManagerGetDrdynvcState(client->vcm) != DRDYNVC_STATE_READY)
		return true;

	if (!client->gfx->Open || !client->gfx->Open(client->gfx))
		return false;
	client->gfx_opened = true;
	client->gfx_opened_at = monotonic_milliseconds();
	log_message("INFO", "RDPGFX dynamic channel open 완료; AVC420 capability 대기 중");
	return true;
}

static bool client_check_gfx_timeout(Client* client)
{
	if (client->gfx_wait_started_at == 0 || client->gfx_ready)
		return true;
	const uint64_t started_at = client->gfx_opened ? client->gfx_opened_at : client->gfx_wait_started_at;
	if (monotonic_milliseconds() - started_at <= 5000)
		return true;
	if (client->gfx_opened)
		log_message("ERROR", "client가 5초 내 RDPGFX AVC420 capability를 보내지 않아 session을 종료합니다");
	else
		log_message("ERROR", "client가 5초 내 RDPGFX dynamic channel을 열지 않아 session을 종료합니다");
	return false;
}

static bool configure_peer(freerdp_peer* peer, Server* server)
{
	peer->ContextSize = sizeof(Client);
	peer->ContextNew = client_context_new;
	peer->ContextFree = client_context_free;
	if (!freerdp_peer_context_new(peer))
		return false;

	rdpSettings* settings = peer->context->settings;
	rdpPrivateKey* private_key = freerdp_key_new_from_file_enc(server->config.private_key, NULL);
	rdpCertificate* certificate = freerdp_certificate_new_from_file(server->config.certificate);
	if (!private_key || !certificate)
		return false;
	if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, private_key, 1) ||
	    !freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, certificate, 1) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline,
	                               server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_GfxH264, server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec,
	                               !server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_NSCodec, !server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled,
	                               server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled,
	                               server->config.direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_HasRelativeMouseEvent, TRUE) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, server->config.width) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, server->config.height) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_MultifragMaxRequestSize, 0xFFFFFFU))
		return false;

	peer->PostConnect = peer_post_connect;
	peer->context->input->KeyboardEvent = on_keyboard;
	peer->context->input->UnicodeKeyboardEvent = on_unicode_keyboard;
	peer->context->input->MouseEvent = on_mouse;
	peer->context->input->ExtendedMouseEvent = on_extended_mouse;
	return peer->Initialize(peer) == TRUE;
}

static DWORD WINAPI peer_thread(LPVOID argument)
{
	freerdp_peer* peer = (freerdp_peer*)argument;
	Server* server = (Server*)peer->ContextExtra;
	if (!configure_peer(peer, server))
		goto out;

	Client* client = (Client*)peer->context;
	log_message("INFO", "TLS RDP client 연결 수락");
	while (!client_should_stop(client))
	{
		HANDLE handles[MAX_EVENT_HANDLES] = WINPR_C_ARRAY_INIT;
		DWORD count = peer->GetEventHandles(peer, handles, ARRAYSIZE(handles));
		if (count == 0 || count >= ARRAYSIZE(handles))
			break;
		if (server->config.direct_gfx)
			handles[count++] = WTSVirtualChannelManagerGetEventHandle(client->vcm);
		const DWORD status = WaitForMultipleObjects(count, handles, FALSE, 100);
		if (status == WAIT_TIMEOUT)
		{
			if (server->config.direct_gfx &&
			    (!client_process_dynamic_channels(client) || !client_check_gfx_timeout(client)))
				break;
			continue;
		}
		if (status == WAIT_FAILED)
			break;
		if (!peer->CheckFileDescriptor(peer))
			break;
		if (server->config.direct_gfx &&
		    (!client_process_dynamic_channels(client) || !client_check_gfx_timeout(client)))
			break;
	}
out:
	if (peer->Disconnect)
		peer->Disconnect(peer);
	freerdp_peer_context_free(peer);
	freerdp_peer_free(peer);
	return 0;
}

static BOOL peer_accepted(freerdp_listener* listener, freerdp_peer* peer)
{
	Server* server = (Server*)listener->info;
	peer->ContextExtra = server;
	HANDLE thread = CreateThread(NULL, 0, peer_thread, peer, 0, NULL);
	if (!thread)
		return FALSE;
	(void)CloseHandle(thread);
	return TRUE;
}

static void print_usage(const char* executable)
{
	(void)fprintf(stderr,
	              "Usage: %s [-listen host:port] [-cert file] [-key file] [-width n] [-height n] "
	              "[-bitrate n] [-direct-gfx]\n",
	              executable);
}

static bool parse_listen(const char* value, const char** host, uint16_t* port)
{
	const char* separator = strrchr(value, ':');
	if (!separator || separator == value || separator[1] == '\0')
		return false;
	char* end = NULL;
	const long parsed = strtol(separator + 1, &end, 10);
	if (*end != '\0' || parsed < 1 || parsed > UINT16_MAX)
		return false;
	static char address[64];
	const size_t host_length = (size_t)(separator - value);
	if (host_length >= sizeof(address))
		return false;
	memcpy(address, value, host_length);
	address[host_length] = '\0';
	*host = address;
	*port = (uint16_t)parsed;
	return true;
}

int main(int argc, char* argv[])
{
	Server server = WINPR_C_ARRAY_INIT;
	server.config.bind_address = "0.0.0.0";
	server.config.port = 3389;
	server.config.certificate = "/root/nanokvm-rdp/cert.pem";
	server.config.private_key = "/root/nanokvm-rdp/key.pem";
	server.config.width = DEFAULT_WIDTH;
	server.config.height = DEFAULT_HEIGHT;
	server.config.bitrate = DEFAULT_BITRATE;
	for (int index = 1; index < argc; index++)
	{
		if (strcmp(argv[index], "-listen") == 0 && index + 1 < argc)
		{
			if (!parse_listen(argv[++index], &server.config.bind_address, &server.config.port))
				return 2;
		}
		else if (strcmp(argv[index], "-cert") == 0 && index + 1 < argc)
			server.config.certificate = argv[++index];
		else if (strcmp(argv[index], "-key") == 0 && index + 1 < argc)
			server.config.private_key = argv[++index];
		else if (strcmp(argv[index], "-width") == 0 && index + 1 < argc)
			server.config.width = (uint16_t)strtoul(argv[++index], NULL, 10);
		else if (strcmp(argv[index], "-height") == 0 && index + 1 < argc)
			server.config.height = (uint16_t)strtoul(argv[++index], NULL, 10);
		else if (strcmp(argv[index], "-bitrate") == 0 && index + 1 < argc)
			server.config.bitrate = (uint16_t)strtoul(argv[++index], NULL, 10);
		else if (strcmp(argv[index], "-direct-gfx") == 0)
			server.config.direct_gfx = true;
		else
		{
			print_usage(argv[0]);
			return 2;
		}
	}
	if (server.config.width == 0 || server.config.height == 0 || server.config.bitrate == 0)
		return 2;
	if (!InitializeCriticalSectionAndSpinCount(&server.lock, 4000))
		return 1;
	if (!WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi()) ||
	    !winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT))
		return 1;

	freerdp_listener* listener = freerdp_listener_new();
	if (!listener)
		return 1;
	listener->info = &server;
	listener->PeerAccepted = peer_accepted;
	WSADATA wsa = WINPR_C_ARRAY_INIT;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0 ||
	    !listener->Open(listener, server.config.bind_address, server.config.port))
	{
		freerdp_listener_free(listener);
		return 1;
	}
	(void)signal(SIGINT, on_signal);
	(void)signal(SIGTERM, on_signal);
	(void)fprintf(stderr, "%s: INFO: TLS RDP server listening on %s:%u\n", TAG,
	              server.config.bind_address, server.config.port);

	while (!stop_requested)
	{
		HANDLE handles[8] = WINPR_C_ARRAY_INIT;
		const DWORD count = listener->GetEventHandles(listener, handles, ARRAYSIZE(handles));
		if (count == 0 || WaitForMultipleObjects(count, handles, FALSE, 200) == WAIT_FAILED)
			break;
		if (!listener->CheckFileDescriptor(listener))
			break;
	}
	listener->Close(listener);
	freerdp_listener_free(listener);
	WSACleanup();
	DeleteCriticalSection(&server.lock);
	return 0;
}
