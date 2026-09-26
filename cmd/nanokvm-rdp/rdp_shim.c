#include "rdp_shim.h"

#include "bitmap_diff.h"
#include "frame_flow.h"

#include <freerdp/channels/drdynvc.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/codec/interleaved.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/codec/progressive.h>
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
#include <winpr/synch.h>
#include <winpr/sysinfo.h>
#include <winpr/thread.h>
#include <winpr/wtsapi.h>
#include <winpr/winsock.h>

#include <signal.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/socket.h>
#endif

#ifndef WINPR_C_ARRAY_INIT
#define WINPR_C_ARRAY_INIT \
	{ \
		0 \
	}
#endif

#define SHIM_MAX_EVENT_HANDLES 32U
#define SHIM_CLASSIC_TILE_WIDTH 64U
#define SHIM_CLASSIC_TILE_HEIGHT 64U
#define SHIM_CLASSIC_TILE_MAX_ENCODED (SHIM_CLASSIC_TILE_WIDTH * SHIM_CLASSIC_TILE_HEIGHT * 4U)
#define SHIM_CLASSIC_MAX_UPDATE_SIZE (32U * 1024U)
#define SHIM_VIDEO_QUEUE_CAPACITY (2U)
#define SHIM_H264_QUEUE_CAPACITY (4U)
#define SHIM_GFX_TIMEOUT_MS 5000U

enum
{
	SHIM_INPUT_KEY = 5,
	SHIM_INPUT_POINTER_ABS = 6,
	SHIM_INPUT_POINTER_REL = 7,
	SHIM_INPUT_WHEEL = 8,
	SHIM_INPUT_SYNCHRONIZE = 16,
};

typedef struct NanokvmRdpShim NanokvmRdpShim;
typedef struct NanokvmRdpClient NanokvmRdpClient;

struct NanokvmRdpShim
{
	char* bind_address;
	uint16_t port;
	char* certificate;
	char* private_key;
	uint16_t width;
	uint16_t height;
	bool direct_gfx;
	NanokvmRdpCallbacks callbacks;
	freerdp_listener* listener;
	atomic_bool running;
	atomic_bool stop_requested;
	CRITICAL_SECTION lock;
	CRITICAL_SECTION frame_lock;
	HANDLE frame_event;
	HANDLE peer_thread;
	NanokvmRdpClient* active;
	bool worker_active;
	int peer_sockfd;
	uint8_t* h264_queue[SHIM_H264_QUEUE_CAPACITY];
	size_t h264_queue_lengths[SHIM_H264_QUEUE_CAPACITY];
	uint8_t h264_queue_head;
	uint8_t h264_queue_count;
	bool h264_awaiting_idr;
	uint8_t* pending_bgra;
	size_t pending_bgra_length;
};

struct NanokvmRdpClient
{
	rdpContext context;
	NanokvmRdpShim* shim;
	freerdp_peer* peer;
	HANDLE vcm;
	RdpgfxServerContext* gfx;
	RFX_CONTEXT* rfx;
	NSC_CONTEXT* nsc;
	PROGRESSIVE_CONTEXT* progressive;
	BITMAP_INTERLEAVED_CONTEXT* interleaved;
	wStream* bitmap_stream;
	CRITICAL_SECTION lock;
	bool stopping;
	bool owns_active;
	bool direct_active;
	bool bitmap_active;
	bool gfx_ready;
	bool gfx_opened;
	BYTE last_dvc_state;
	bool gfx_progressive;
	uint64_t gfx_wait_started_at;
	uint32_t next_frame_id;
	FrameFlow frame_flow;
	uint16_t render_width;
	uint16_t render_height;
	uint8_t* previous_bitmap;
	size_t previous_bitmap_length;
	bool previous_bitmap_valid;
	uint8_t* classic_encoded;
	bool bitmap_uses_rfx;
	uint32_t bitmap_frames;
};

static uint64_t shim_now(void)
{
	return (uint64_t)GetTickCount64();
}

static void interrupt_peer_socket(int sockfd)
{
	if (sockfd < 0)
		return;
#ifdef _WIN32
	(void)shutdown((SOCKET)sockfd, SD_BOTH);
#else
	(void)shutdown(sockfd, SHUT_RDWR);
#endif
}

static void shim_event(NanokvmRdpShim* shim, uint32_t event, uint32_t value, uint32_t value2)
{
	if (shim && shim->callbacks.event)
		shim->callbacks.event(shim->callbacks.owner, event, value, value2);
}

static bool shim_input(NanokvmRdpClient* client, uint8_t type, const uint8_t* payload,
	                   uint16_t length)
{
	if (!client || !client->shim || !client->shim->callbacks.input)
		return false;
	return client->shim->callbacks.input(client->shim->callbacks.owner, type, (uint8_t*)payload, length) != 0;
}

static void client_stop(NanokvmRdpClient* client)
{
	if (!client)
		return;
	EnterCriticalSection(&client->lock);
	client->stopping = true;
	LeaveCriticalSection(&client->lock);
}

static bool client_should_stop(NanokvmRdpClient* client)
{
	bool stopping = true;
	if (!client)
		return true;
	EnterCriticalSection(&client->lock);
	stopping = client->stopping;
	LeaveCriticalSection(&client->lock);
	return stopping;
}

static bool copy_buffer(uint8_t** target, size_t* target_length, const uint8_t* source, size_t length)
{
	uint8_t* next = NULL;
	if (length == 0 || !source)
		return false;
	next = malloc(length);
	if (!next)
		return false;
	memcpy(next, source, length);
	free(*target);
	*target = next;
	*target_length = length;
	return true;
}

static bool client_can_send(NanokvmRdpClient* client)
{
	bool ready = false;
	EnterCriticalSection(&client->lock);
	ready = client->gfx_ready && !client->stopping;
	LeaveCriticalSection(&client->lock);
	return ready;
}

static bool client_supports_avc420(const RDPGFX_CAPSET* cap)
{
	if (cap->version == RDPGFX_CAPVERSION_81)
		return (cap->flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED) != 0;
	return cap->version >= RDPGFX_CAPVERSION_10 &&
	       (cap->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED) == 0;
}

static bool client_has_avc444(const RDPGFX_CAPS_ADVERTISE_PDU* advertise)
{
	for (UINT32 i = 0; i < advertise->capsSetCount; i++)
	{
		const RDPGFX_CAPSET* cap = &advertise->capsSets[i];
		if (cap->version >= RDPGFX_CAPVERSION_10 &&
		    (cap->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED) == 0 &&
		    (cap->flags & RDPGFX_CAPS_FLAG_AVC_THINCLIENT) != 0)
			return true;
	}
	return false;
}

static bool client_select_cap(const RDPGFX_CAPS_ADVERTISE_PDU* advertise,
	                          RDPGFX_CAPSET* selected, bool* use_avc420)
{
	static const UINT32 versions[] = { RDPGFX_CAPVERSION_107, RDPGFX_CAPVERSION_106,
		RDPGFX_CAPVERSION_106_ERR, RDPGFX_CAPVERSION_105, RDPGFX_CAPVERSION_104,
		RDPGFX_CAPVERSION_103, RDPGFX_CAPVERSION_102, RDPGFX_CAPVERSION_101,
		RDPGFX_CAPVERSION_10, RDPGFX_CAPVERSION_81, RDPGFX_CAPVERSION_8 };
	const bool allow_avc420 = client_has_avc444(advertise);
	for (size_t pass = 0; pass < 2; pass++)
	{
		for (size_t v = 0; v < ARRAYSIZE(versions); v++)
		{
			for (UINT32 i = 0; i < advertise->capsSetCount; i++)
			{
				const RDPGFX_CAPSET* cap = &advertise->capsSets[i];
				if (cap->version != versions[v])
					continue;
				const bool avc420 = allow_avc420 && client_supports_avc420(cap);
				if ((pass == 0) != avc420)
					continue;
				*selected = *cap;
				*use_avc420 = avc420;
				return true;
			}
		}
	}
	return false;
}

static bool bitmap_stream_rfx_supported(const rdpSettings* settings)
{
	const uint32_t supported = freerdp_settings_get_uint32(settings, FreeRDP_SurfaceCommandsSupported);
	return freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec) &&
	       freerdp_settings_get_uint32(settings, FreeRDP_RemoteFxCodecId) != 0 &&
	       (supported & SURFCMDS_STREAM_SURFACE_BITS) != 0;
}

static bool bitmap_stream_nsc_supported(const rdpSettings* settings)
{
	const uint32_t supported = freerdp_settings_get_uint32(settings, FreeRDP_SurfaceCommandsSupported);
	return freerdp_settings_get_bool(settings, FreeRDP_NSCodec) &&
	       freerdp_settings_get_uint32(settings, FreeRDP_NSCodecId) != 0 &&
	       (supported & SURFCMDS_SET_SURFACE_BITS) != 0;
}

static bool send_classic_bitmap(NanokvmRdpClient* client, const uint8_t* bgra, size_t length)
{
	const uint16_t width = client->render_width;
	const uint16_t height = client->render_height;
	const size_t expected = (size_t)width * height * 4U;
	BITMAP_DATA rectangles[1] = WINPR_C_ARRAY_INIT;
	BITMAP_UPDATE bitmap = WINPR_C_ARRAY_INIT;
	uint16_t rectangle_count = 0;
	if (!client->context.update || !client->context.update->BitmapUpdate || !client->interleaved ||
	    !client->classic_encoded || length != expected)
		return false;
	if (!client->previous_bitmap)
	{
		client->previous_bitmap = malloc(expected);
		client->previous_bitmap_length = client->previous_bitmap ? expected : 0;
	}
	if (!client->previous_bitmap)
		return false;
	for (uint16_t top = 0; top < height; top += SHIM_CLASSIC_TILE_HEIGHT)
	{
		const uint16_t rows = (uint16_t)((top + SHIM_CLASSIC_TILE_HEIGHT > height) ? height - top : SHIM_CLASSIC_TILE_HEIGHT);
		for (uint16_t left = 0; left < width; left += SHIM_CLASSIC_TILE_WIDTH)
		{
			const uint16_t columns = (uint16_t)((left + SHIM_CLASSIC_TILE_WIDTH > width) ? width - left : SHIM_CLASSIC_TILE_WIDTH);
			const size_t source_offset = ((size_t)top * width + left) * 4U;
			if (client->previous_bitmap_valid &&
			    !bitmap_tile_changed(client->previous_bitmap, bgra, width, left, top, columns, rows))
				continue;
			uint8_t* encoded = client->classic_encoded;
			uint32_t encoded_length = SHIM_CLASSIC_TILE_MAX_ENCODED;
			if ((columns % 4U) != 0 ||
			    !interleaved_compress(client->interleaved, encoded, &encoded_length, columns, rows,
			                          bgra, PIXEL_FORMAT_BGRX32, (uint32_t)width * 4U, left, top,
			                          NULL, 16))
				return false;
			if (encoded_length == 0 || encoded_length > SHIM_CLASSIC_TILE_MAX_ENCODED)
				return false;
			BITMAP_DATA* rectangle = &rectangles[rectangle_count];
			rectangle->destLeft = left;
			rectangle->destTop = top;
			rectangle->destRight = left + columns - 1U;
			rectangle->destBottom = top + rows - 1U;
			rectangle->width = columns;
			rectangle->height = rows;
			rectangle->bitsPerPixel = 16;
			rectangle->bitmapLength = (uint16_t)encoded_length;
			rectangle->bitmapDataStream = encoded;
			rectangle->compressed = TRUE;
			rectangle->cbCompFirstRowSize = 0;
			rectangle->cbCompMainBodySize = encoded_length;
			rectangle->cbScanWidth = columns * 2U;
			rectangle->cbUncompressedSize = columns * rows * 2U;
			rectangle_count++;
			for (uint16_t row = 0; row < rows; row++)
				memcpy(client->previous_bitmap + source_offset + (size_t)row * width * 4U,
				       bgra + source_offset + (size_t)row * width * 4U,
				       (size_t)columns * 4U);
			if (rectangle_count == 1)
			{
				bitmap.number = rectangle_count;
				bitmap.rectangles = rectangles;
				if (!client->context.update->BitmapUpdate(&client->context, &bitmap))
					return false;
				rectangle_count = 0;
			}
		}
	}
	client->previous_bitmap_valid = true;
	(void)client->bitmap_frames++;
	return true;
}

static bool send_progressive(NanokvmRdpClient* client, const uint8_t* bgra, size_t length);

static bool send_bitmap(NanokvmRdpClient* client, const uint8_t* bgra, size_t length)
{
	const uint16_t width = client->render_width;
	const uint16_t height = client->render_height;
	const size_t expected = (size_t)width * height * 4U;
	rdpSettings* settings = client->context.settings;
	if (!settings || !client->context.update || length != expected)
		return false;
	if (client->gfx_progressive)
	{
		return send_progressive(client, bgra, length);
	}
	if (client->bitmap_uses_rfx || client->nsc)
	{
		SURFACE_BITS_COMMAND command = WINPR_C_ARRAY_INIT;
		RFX_RECT rect = { .x = 0, .y = 0, .width = width, .height = height };
		if (!client->context.update->SurfaceBits || !client->bitmap_stream)
			return false;
		Stream_Clear(client->bitmap_stream);
		Stream_SetPosition(client->bitmap_stream, 0);
		if (client->bitmap_uses_rfx)
		{
			rfx_context_set_pixel_format(client->rfx, PIXEL_FORMAT_BGRX32);
			if (!rfx_compose_message(client->rfx, client->bitmap_stream, &rect, 1, bgra, width,
			                         height, (uint32_t)width * 4U))
				return false;
			command.cmdType = CMDTYPE_STREAM_SURFACE_BITS;
			command.bmp.codecID = (uint16_t)freerdp_settings_get_uint32(settings, FreeRDP_RemoteFxCodecId);
		}
		else
		{
			if (!nsc_context_set_parameters(client->nsc, NSC_COLOR_FORMAT, PIXEL_FORMAT_BGRX32) ||
			    !nsc_compose_message(client->nsc, client->bitmap_stream, bgra, width, height,
			                         (uint32_t)width * 4U))
				return false;
			command.cmdType = CMDTYPE_SET_SURFACE_BITS;
			command.bmp.codecID = (uint16_t)freerdp_settings_get_uint32(settings, FreeRDP_NSCodecId);
		}
		command.destLeft = 0;
		command.destTop = 0;
		command.destRight = width;
		command.destBottom = height;
		command.bmp.bpp = 32;
		command.bmp.width = width;
		command.bmp.height = height;
		command.bmp.bitmapDataLength = (uint32_t)Stream_GetPosition(client->bitmap_stream);
		command.bmp.bitmapData = Stream_Buffer(client->bitmap_stream);
		if (!client->context.update->SurfaceBits(&client->context, &command))
			return false;
		client->bitmap_frames++;
		return true;
	}
	return send_classic_bitmap(client, bgra, length);
}

static bool send_avc420(NanokvmRdpClient* client, const uint8_t* data, size_t length)
{
	RECTANGLE_16 rect = { .left = 0, .top = 0, .right = client->shim->width, .bottom = client->shim->height };
	RDPGFX_H264_QUANT_QUALITY quality = { .qpVal = 0, .qualityVal = 100, .qp = 0, .r = 0, .p = 0 };
	RDPGFX_AVC420_BITMAP_STREAM avc = WINPR_C_ARRAY_INIT;
	RDPGFX_SURFACE_COMMAND command = WINPR_C_ARRAY_INIT;
	RDPGFX_START_FRAME_PDU start = WINPR_C_ARRAY_INIT;
	RDPGFX_END_FRAME_PDU end = WINPR_C_ARRAY_INIT;
	if (!client->gfx || !client->gfx->SurfaceFrameCommand || !client_can_send(client))
		return false;
	EnterCriticalSection(&client->lock);
	start.frameId = client->next_frame_id++;
	end.frameId = start.frameId;
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
	command.right = client->shim->width;
	command.bottom = client->shim->height;
	command.width = client->shim->width;
	command.height = client->shim->height;
	command.extra = &avc;
	if (client->gfx->SurfaceFrameCommand(client->gfx, &command, &start, &end) != CHANNEL_RC_OK)
		return false;
	return true;
}

static bool send_progressive(NanokvmRdpClient* client, const uint8_t* bgra, size_t length)
{
	const uint16_t width = client->render_width;
	const uint16_t height = client->render_height;
	const size_t expected = (size_t)width * height * 4U;
	REGION16 region = WINPR_C_ARRAY_INIT;
	RDPGFX_SURFACE_COMMAND command = WINPR_C_ARRAY_INIT;
	RDPGFX_START_FRAME_PDU start = WINPR_C_ARRAY_INIT;
	RDPGFX_END_FRAME_PDU end = WINPR_C_ARRAY_INIT;
	if (!client->progressive || !client->gfx || !client->gfx->SurfaceFrameCommand || length != expected)
		return false;
	EnterCriticalSection(&client->lock);
	const bool blocked = frame_flow_blocked(&client->frame_flow);
	LeaveCriticalSection(&client->lock);
	if (blocked)
		return true;
	region16_init(&region);
	const uint16_t tile_columns = (uint16_t)((width + 63U) / 64U);
	const uint16_t tile_rows = (uint16_t)((height + 63U) / 64U);
	const uint8_t* previous = client->previous_bitmap_valid ? client->previous_bitmap : NULL;
	for (uint16_t tile_y = 0; tile_y < tile_rows; tile_y++)
	{
		for (uint16_t tile_x = 0; tile_x < tile_columns; tile_x++)
		{
			const uint16_t left = (uint16_t)(tile_x * 64U);
			const uint16_t top = (uint16_t)(tile_y * 64U);
			const uint16_t columns = (uint16_t)(left + 64U > width ? width - left : 64U);
			const uint16_t rows = (uint16_t)(top + 64U > height ? height - top : 64U);
			if (previous && !bitmap_tile_changed(previous, bgra, width, left, top, columns, rows))
				continue;
			RECTANGLE_16 rect = { .left = left, .top = top,
				.right = (UINT16)(left + columns), .bottom = (UINT16)(top + rows) };
			if (!region16_union_rect(&region, &region, &rect))
			{
				region16_uninit(&region);
				return false;
			}
		}
	}
	if (region16_n_rects(&region) == 0)
	{
		region16_uninit(&region);
		return true;
	}
	const int encoded = progressive_compress(client->progressive, bgra, (uint32_t)length,
	                                        PIXEL_FORMAT_BGRX32, width, height, (uint32_t)width * 4U,
	                                        &region, &command.data, &command.length);
	region16_uninit(&region);
	if (encoded < 0)
	{
		(void)fprintf(stderr, "nanokvm-rdp-shim: progressive encode failed\n");
		return false;
	}
	if (encoded == 0)
		return encoded == 0;
	EnterCriticalSection(&client->lock);
	start.frameId = client->next_frame_id++;
	end.frameId = start.frameId;
	const bool tracked = frame_flow_sent(&client->frame_flow, start.frameId, shim_now());
	LeaveCriticalSection(&client->lock);
	if (!tracked)
	{
		command.data = NULL;
		return true;
	}
	command.surfaceId = 1;
	command.codecId = RDPGFX_CODECID_CAPROGRESSIVE;
	command.format = PIXEL_FORMAT_BGRX32;
	command.left = 0;
	command.top = 0;
	command.right = width;
	command.bottom = height;
	command.width = width;
	command.height = height;
	const UINT error = client->gfx->SurfaceFrameCommand(client->gfx, &command, &start, &end);
	command.data = NULL;
	if (error != CHANNEL_RC_OK)
	{
		(void)fprintf(stderr, "nanokvm-rdp-shim: progressive submit failed error=%u\n", error);
		return false;
	}
	if (!client->previous_bitmap)
	{
		client->previous_bitmap = malloc(expected);
		client->previous_bitmap_length = client->previous_bitmap ? expected : 0;
	}
	if (!client->previous_bitmap)
		return false;
	memcpy(client->previous_bitmap, bgra, expected);
	client->previous_bitmap_valid = true;
	return true;
}

static UINT on_frame_ack(RdpgfxServerContext* gfx,
                         const RDPGFX_FRAME_ACKNOWLEDGE_PDU* acknowledge)
{
	NanokvmRdpClient* client = (NanokvmRdpClient*)gfx->custom;
	uint64_t elapsed = 0;
	EnterCriticalSection(&client->lock);
	(void)frame_flow_ack(&client->frame_flow, acknowledge->frameId, acknowledge->queueDepth,
	                     shim_now(), &elapsed);
	LeaveCriticalSection(&client->lock);
	shim_event(client->shim, NANOKVM_RDP_EVENT_FRAME_ACK, acknowledge->frameId,
	           acknowledge->queueDepth);
	return CHANNEL_RC_OK;
}

static UINT on_caps_advertise(RdpgfxServerContext* gfx,
                              const RDPGFX_CAPS_ADVERTISE_PDU* advertise)
{
	NanokvmRdpClient* client = (NanokvmRdpClient*)gfx->custom;
	RDPGFX_CAPSET selected = WINPR_C_ARRAY_INIT;
	RDPGFX_CAPS_CONFIRM_PDU confirm = WINPR_C_ARRAY_INIT;
	RDPGFX_CREATE_SURFACE_PDU surface = WINPR_C_ARRAY_INIT;
	RDPGFX_MAP_SURFACE_TO_OUTPUT_PDU map = WINPR_C_ARRAY_INIT;
	bool avc420 = false;
	if (!client_select_cap(advertise, &selected, &avc420))
		return CHANNEL_RC_UNSUPPORTED_VERSION;
	confirm.capsSet = &selected;
	if (!gfx->CapsConfirm || gfx->CapsConfirm(gfx, &confirm) != CHANNEL_RC_OK)
		return ERROR_INTERNAL_ERROR;
	surface.surfaceId = 1;
	surface.width = client->shim->width;
	surface.height = client->shim->height;
	surface.pixelFormat = GFX_PIXEL_FORMAT_XRGB_8888;
	if (!gfx->CreateSurface || gfx->CreateSurface(gfx, &surface) != CHANNEL_RC_OK)
		return ERROR_INTERNAL_ERROR;
	map.surfaceId = surface.surfaceId;
	if (!gfx->MapSurfaceToOutput || gfx->MapSurfaceToOutput(gfx, &map) != CHANNEL_RC_OK)
		return ERROR_INTERNAL_ERROR;
	EnterCriticalSection(&client->lock);
	client->gfx_ready = true;
	client->direct_active = avc420;
	client->bitmap_active = !avc420;
	client->gfx_progressive = !avc420;
	LeaveCriticalSection(&client->lock);
	if (!avc420)
	{
		client->progressive = progressive_context_new_ex(
		    TRUE, freerdp_settings_get_uint32(client->context.settings, FreeRDP_ThreadingFlags));
		if (!client->progressive || !progressive_context_reset(client->progressive))
			return ERROR_INTERNAL_ERROR;
	}
	shim_event(client->shim, avc420 ? NANOKVM_RDP_EVENT_VIDEO_AVC420 : NANOKVM_RDP_EVENT_VIDEO_BITMAP,
	           0, 0);
	return CHANNEL_RC_OK;
}

static BOOL on_keyboard(rdpInput* input, UINT16 flags, UINT8 code)
{
	NanokvmRdpClient* client = (NanokvmRdpClient*)input->context;
	const uint8_t payload[3] = { code, (uint8_t)((flags & KBD_FLAGS_EXTENDED) != 0),
		(uint8_t)((flags & KBD_FLAGS_RELEASE) != 0) };
	return shim_input(client, SHIM_INPUT_KEY, payload, sizeof(payload)) ? TRUE : FALSE;
}

static BOOL on_unicode(rdpInput* input, UINT16 flags, UINT16 code)
{
	NanokvmRdpClient* client = (NanokvmRdpClient*)input->context;
	uint8_t payload[3] = { (uint8_t)(code >> 8U), (uint8_t)code, 0 };
	if ((flags & KBD_FLAGS_RELEASE) != 0)
		return TRUE;
	return shim_input(client, 14, payload, 2) ? TRUE : FALSE;
}

static BOOL on_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	NanokvmRdpClient* client = (NanokvmRdpClient*)input->context;
	uint8_t payload[12] = { 0 };
	payload[0] = (uint8_t)(x >> 8U); payload[1] = (uint8_t)x;
	payload[2] = (uint8_t)(y >> 8U); payload[3] = (uint8_t)y;
	payload[4] = (uint8_t)(client->shim->width >> 8U); payload[5] = (uint8_t)client->shim->width;
	payload[6] = (uint8_t)(client->shim->height >> 8U); payload[7] = (uint8_t)client->shim->height;
	payload[8] = (uint8_t)(flags >> 8U); payload[9] = (uint8_t)flags;
	return shim_input(client, SHIM_INPUT_POINTER_ABS, payload, 10) ? TRUE : FALSE;
}

static BOOL on_extended_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	return on_mouse(input, flags & (PTR_FLAGS_DOWN | PTR_XFLAGS_BUTTON1 | PTR_XFLAGS_BUTTON2), x, y);
}

static BOOL on_relative_mouse(rdpInput* input, UINT16 flags, INT16 dx, INT16 dy)
{
	NanokvmRdpClient* client = (NanokvmRdpClient*)input->context;
	uint8_t payload[6] = { (uint8_t)(dx >> 8U), (uint8_t)dx, (uint8_t)(dy >> 8U), (uint8_t)dy,
		(uint8_t)(flags >> 8U), (uint8_t)flags };
	return shim_input(client, SHIM_INPUT_POINTER_REL, payload, sizeof(payload)) ? TRUE : FALSE;
}

static BOOL on_synchronize(rdpInput* input, UINT32 toggles)
{
	NanokvmRdpClient* client = (NanokvmRdpClient*)input->context;
	(void)toggles;
	return shim_input(client, SHIM_INPUT_SYNCHRONIZE, NULL, 0) ? TRUE : FALSE;
}

static BOOL client_context_new(freerdp_peer* peer, rdpContext* context)
{
	NanokvmRdpClient* client = (NanokvmRdpClient*)context;
	NanokvmRdpShim* shim = (NanokvmRdpShim*)peer->ContextExtra;
	if (!shim || !InitializeCriticalSectionAndSpinCount(&client->lock, 4000))
		return FALSE;
	client->shim = shim;
	client->peer = peer;
	client->render_width = shim->width;
	client->render_height = shim->height;
	if (shim->direct_gfx)
	{
		client->vcm = WTSOpenServerA((LPSTR)context);
		if (!client->vcm || client->vcm == INVALID_HANDLE_VALUE)
		{
			DeleteCriticalSection(&client->lock);
			return FALSE;
		}
	}
	return TRUE;
}

static void free_client_buffers(NanokvmRdpClient* client)
{
	free(client->previous_bitmap);
	free(client->classic_encoded);
	if (client->bitmap_stream) Stream_Free(client->bitmap_stream, TRUE);
	if (client->interleaved) bitmap_interleaved_context_free(client->interleaved);
	if (client->progressive) progressive_context_free(client->progressive);
	if (client->nsc) nsc_context_free(client->nsc);
	if (client->rfx) rfx_context_free(client->rfx);
	if (client->gfx) rdpgfx_server_context_free(client->gfx);
	if (client->vcm && client->vcm != INVALID_HANDLE_VALUE) WTSCloseServer(client->vcm);
}

static void client_context_free(freerdp_peer* peer, rdpContext* context)
{
	NanokvmRdpClient* client = (NanokvmRdpClient*)context;
	NanokvmRdpShim* shim = client->shim;
	client_stop(client);
	if (client->owns_active)
	{
		EnterCriticalSection(&shim->lock);
		if (shim->active == client) shim->active = NULL;
		LeaveCriticalSection(&shim->lock);
	}
	free_client_buffers(client);
	shim_event(shim, NANOKVM_RDP_EVENT_SESSION_STOPPED, 0, 0);
	DeleteCriticalSection(&client->lock);
	(void)peer;
}

static bool client_prepare_bitmap(NanokvmRdpClient* client)
{
	const rdpSettings* settings = client->context.settings;
	client->bitmap_active = true;
	client->bitmap_uses_rfx = bitmap_stream_rfx_supported(settings);
	if (client->bitmap_uses_rfx)
	{
		client->rfx = rfx_context_new_ex(TRUE, freerdp_settings_get_uint32(settings, FreeRDP_ThreadingFlags));
		if (!client->rfx || !rfx_context_reset(client->rfx, client->render_width, client->render_height)) return false;
	}
	else if (bitmap_stream_nsc_supported(settings))
	{
		client->nsc = nsc_context_new();
		if (!client->nsc) return false;
	}
	else
	{
		client->classic_encoded = calloc(1, SHIM_CLASSIC_TILE_MAX_ENCODED);
		client->interleaved = bitmap_interleaved_context_new(TRUE);
		if (!client->classic_encoded || !client->interleaved) return false;
	}
	if (client->bitmap_uses_rfx || client->nsc)
	{
		client->bitmap_stream = Stream_New(NULL, 65536);
		if (!client->bitmap_stream) return false;
	}
	shim_event(client->shim, NANOKVM_RDP_EVENT_VIDEO_BITMAP, 0, 0);
	return true;
}

static BOOL peer_post_connect(freerdp_peer* peer)
{
	NanokvmRdpClient* client = (NanokvmRdpClient*)peer->context;
	NanokvmRdpShim* shim = client->shim;
	if (!client->context.update || !client->context.update->DesktopResize)
		return FALSE;
	if (freerdp_settings_get_uint32(client->context.settings, FreeRDP_DesktopWidth) != shim->width ||
	    freerdp_settings_get_uint32(client->context.settings, FreeRDP_DesktopHeight) != shim->height)
	{
		if (!freerdp_settings_set_uint32(client->context.settings, FreeRDP_DesktopWidth, shim->width) ||
		    !freerdp_settings_set_uint32(client->context.settings, FreeRDP_DesktopHeight, shim->height) ||
		    !client->context.update->DesktopResize(&client->context))
			return FALSE;
	}
	EnterCriticalSection(&shim->lock);
	if (shim->active) { LeaveCriticalSection(&shim->lock); return FALSE; }
	shim->active = client;
	client->owns_active = true;
	LeaveCriticalSection(&shim->lock);
	shim_event(shim, NANOKVM_RDP_EVENT_SESSION_STARTED, 0, 0);
	if (!shim->direct_gfx)
		return client_prepare_bitmap(client);
	client->gfx = rdpgfx_server_context_new(client->vcm);
	if (!client->gfx) return FALSE;
	client->gfx->rdpcontext = &client->context;
	client->gfx->custom = client;
	client->gfx->CapsAdvertise = on_caps_advertise;
	client->gfx->FrameAcknowledge = on_frame_ack;
	if (!client->gfx->Initialize || !client->gfx->Initialize(client->gfx, TRUE)) return FALSE;
	client->direct_active = true;
	client->gfx_wait_started_at = shim_now();
	return TRUE;
}

static bool client_configure(freerdp_peer* peer, NanokvmRdpShim* shim)
{
	rdpSettings* settings = peer->context->settings;
	rdpPrivateKey* key = freerdp_key_new_from_file(shim->private_key);
	rdpCertificate* cert = freerdp_certificate_new_from_file(shim->certificate);
	if (!key || !cert) return false;
	if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1) ||
	    !freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, shim->direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_GfxH264, shim->direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_GfxProgressive, shim->direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_GfxProgressiveV2, shim->direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, shim->direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled, shim->direct_gfx) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_HasExtendedMouseEvent, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_HasHorizontalWheel, TRUE) ||
	    !freerdp_settings_set_bool(settings, FreeRDP_HasRelativeMouseEvent, TRUE) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_KeyboardLayout, 0x00000412U) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_KeyboardType, 4) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_KeyboardSubType, 0) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_KeyboardFunctionKey, 12) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, shim->width) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, shim->height) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32) ||
	    !freerdp_settings_set_uint32(settings, FreeRDP_MultifragMaxRequestSize, 0xFFFFFFU))
		return false;
	peer->PostConnect = peer_post_connect;
	peer->context->input->KeyboardEvent = on_keyboard;
	peer->context->input->UnicodeKeyboardEvent = on_unicode;
	peer->context->input->MouseEvent = on_mouse;
	peer->context->input->ExtendedMouseEvent = on_extended_mouse;
	peer->context->input->RelMouseEvent = on_relative_mouse;
	peer->context->input->SynchronizeEvent = on_synchronize;
	return peer->Initialize(peer) == TRUE;
}

static bool client_flush_frame(NanokvmRdpClient* client)
{
	NanokvmRdpShim* shim = client->shim;
	uint8_t* h264 = NULL;
	uint8_t* bgra = NULL;
	size_t h264_length = 0;
	size_t bgra_length = 0;
	EnterCriticalSection(&shim->frame_lock);
	if (client->direct_active && client->gfx_ready)
	{
		if (shim->h264_queue_count > 0)
		{
			h264 = shim->h264_queue[shim->h264_queue_head];
			h264_length = shim->h264_queue_lengths[shim->h264_queue_head];
			shim->h264_queue[shim->h264_queue_head] = NULL;
			shim->h264_queue_lengths[shim->h264_queue_head] = 0;
			shim->h264_queue_head = (uint8_t)((shim->h264_queue_head + 1U) % SHIM_H264_QUEUE_CAPACITY);
			shim->h264_queue_count--;
		}
	}
	else if (client->bitmap_active)
	{
		bgra = shim->pending_bgra;
		bgra_length = shim->pending_bgra_length;
		shim->pending_bgra = NULL; shim->pending_bgra_length = 0;
	}
	if (shim->h264_queue_count == 0 && shim->pending_bgra == NULL)
		(void)ResetEvent(shim->frame_event);
	LeaveCriticalSection(&shim->frame_lock);
	if (h264)
	{
		const bool ok = send_avc420(client, h264, h264_length);
		free(h264);
		return ok;
	}
	if (bgra)
	{
		const bool ok = send_bitmap(client, bgra, bgra_length);
		free(bgra);
		return ok;
	}
	return true;
}

static bool client_process_channels(NanokvmRdpClient* client)
{
	if (!client->gfx || !client->vcm || client->gfx_wait_started_at == 0)
		return true;
	if (!WTSVirtualChannelManagerCheckFileDescriptor(client->vcm))
		return false;
	const BYTE dvc_state = WTSVirtualChannelManagerGetDrdynvcState(client->vcm);
	if (dvc_state != client->last_dvc_state)
		client->last_dvc_state = dvc_state;
	if (!client->gfx_opened &&
	    dvc_state == DRDYNVC_STATE_READY)
	{
		if (!client->gfx->Open || !client->gfx->Open(client->gfx))
			return false;
		client->gfx_opened = true;
		client->gfx_wait_started_at = shim_now();
	}
	if (client->gfx_opened)
	{
		HANDLE event = rdpgfx_server_get_event_handle(client->gfx);
		if (event && WaitForSingleObject(event, 0) == WAIT_OBJECT_0 &&
		    rdpgfx_server_handle_messages(client->gfx) != CHANNEL_RC_OK)
			return false;
	}
	if (client->gfx_wait_started_at != 0 && !client->gfx_ready &&
	    shim_now() - client->gfx_wait_started_at > SHIM_GFX_TIMEOUT_MS)
	{
		client->direct_active = false;
		client->bitmap_active = true;
		client->gfx_progressive = false;
		client->gfx_wait_started_at = 0;
		if (!client_prepare_bitmap(client))
			return false;
	}
	return true;
}

static DWORD WINAPI peer_thread(LPVOID argument)
{
	freerdp_peer* peer = (freerdp_peer*)argument;
	NanokvmRdpShim* shim = (NanokvmRdpShim*)peer->ContextExtra;
	if (!freerdp_peer_context_new(peer)) goto out;
	if (!client_configure(peer, shim)) goto out_context;
	NanokvmRdpClient* client = (NanokvmRdpClient*)peer->context;
	while (!client_should_stop(client) && !atomic_load(&shim->stop_requested))
	{
		HANDLE handles[SHIM_MAX_EVENT_HANDLES] = WINPR_C_ARRAY_INIT;
		DWORD count = peer->GetEventHandles(peer, handles, ARRAYSIZE(handles));
		if (count == 0 || count >= ARRAYSIZE(handles)) break;
		if (client->vcm && count < ARRAYSIZE(handles))
			handles[count++] = WTSVirtualChannelManagerGetEventHandle(client->vcm);
		if (count >= ARRAYSIZE(handles)) break;
		handles[count++] = shim->frame_event;
		const DWORD status = WaitForMultipleObjects(count, handles, FALSE, 20);
		if (status == WAIT_FAILED) break;
		if (!peer->CheckFileDescriptor(peer)) break;
		if (!client_process_channels(client) || !client_flush_frame(client)) break;
	}
	out_context:
	if (peer->context)
		client_stop((NanokvmRdpClient*)peer->context);
	out:
	if (peer->Disconnect) peer->Disconnect(peer);
	if (peer->context)
		freerdp_peer_context_free(peer);
	freerdp_peer_free(peer);
	EnterCriticalSection(&shim->lock);
	shim->worker_active = false;
	shim->peer_sockfd = -1;
	LeaveCriticalSection(&shim->lock);
	return 0;
}

static BOOL peer_accepted(freerdp_listener* listener, freerdp_peer* peer)
{
	NanokvmRdpShim* shim = (NanokvmRdpShim*)listener->info;
	EnterCriticalSection(&shim->lock);
	const bool busy = shim->active != NULL || shim->worker_active;
	if (shim->peer_thread && WaitForSingleObject(shim->peer_thread, 0) == WAIT_OBJECT_0)
	{
		CloseHandle(shim->peer_thread);
		shim->peer_thread = NULL;
	}
	LeaveCriticalSection(&shim->lock);
	if (busy) return FALSE;
	peer->ContextExtra = shim;
	peer->ContextSize = sizeof(NanokvmRdpClient);
	peer->ContextNew = client_context_new;
	peer->ContextFree = client_context_free;
	EnterCriticalSection(&shim->lock);
	shim->worker_active = true;
	shim->peer_sockfd = peer->sockfd;
	LeaveCriticalSection(&shim->lock);
	HANDLE thread = CreateThread(NULL, 0, peer_thread, peer, 0, NULL);
	if (!thread)
	{
		EnterCriticalSection(&shim->lock);
		shim->worker_active = false;
		shim->peer_sockfd = -1;
		LeaveCriticalSection(&shim->lock);
		return FALSE;
	}
	EnterCriticalSection(&shim->lock);
	shim->peer_thread = thread;
	LeaveCriticalSection(&shim->lock);
	return TRUE;
}

NanokvmRdpShim* nanokvm_rdp_shim_new(const char* bind_address, uint16_t port,
	                                 const char* certificate, const char* private_key,
	                                 uint16_t width, uint16_t height, bool direct_gfx,
	                                 const NanokvmRdpCallbacks* callbacks)
{
	NanokvmRdpShim* shim = calloc(1, sizeof(*shim));
	if (!shim || !bind_address || !certificate || !private_key || width == 0 || height == 0)
		goto fail;
	shim->bind_address = strdup(bind_address);
	shim->certificate = strdup(certificate);
	shim->private_key = strdup(private_key);
	if (!shim->bind_address || !shim->certificate || !shim->private_key ||
	    !InitializeCriticalSectionAndSpinCount(&shim->lock, 4000) ||
	    !InitializeCriticalSectionAndSpinCount(&shim->frame_lock, 4000))
		goto fail;
	shim->port = port;
	shim->width = width;
	shim->height = height;
	shim->direct_gfx = direct_gfx;
	shim->peer_sockfd = -1;
	if (callbacks) shim->callbacks = *callbacks;
	atomic_init(&shim->running, false);
	atomic_init(&shim->stop_requested, false);
	/* WinPR on macOS does not implement auto-reset events. Keep this manual
	 * reset event and reset it while holding frame_lock after consuming work. */
	shim->frame_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!shim->frame_event) goto fail;
	return shim;
fail:
	if (shim)
	{
		free(shim->bind_address); free(shim->certificate); free(shim->private_key);
		free(shim);
	}
	return NULL;
}

int nanokvm_rdp_shim_run(NanokvmRdpShim* shim)
{
	if (!shim || atomic_exchange(&shim->running, true)) return -1;
	atomic_store(&shim->stop_requested, false);
	if (!WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi()))
	{
		(void)fprintf(stderr, "nanokvm-rdp-shim: WTS API init failed\n");
		goto fail;
	}
	if (!winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT))
	{
		(void)fprintf(stderr, "nanokvm-rdp-shim: SSL init failed\n");
		goto fail;
	}
	WSADATA wsa = WINPR_C_ARRAY_INIT;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
	{
		(void)fprintf(stderr, "nanokvm-rdp-shim: WSA init failed\n");
		goto fail;
	}
	shim->listener = freerdp_listener_new();
	if (!shim->listener || !shim->listener->Open(shim->listener, shim->bind_address, shim->port))
	{
		(void)fprintf(stderr, "nanokvm-rdp-shim: RDP listener open failed on %s:%u\n",
		              shim->bind_address, shim->port);
		goto fail_wsa;
	}
	shim->listener->info = shim;
	shim->listener->PeerAccepted = peer_accepted;
	while (!atomic_load(&shim->stop_requested))
	{
		HANDLE handles[8] = WINPR_C_ARRAY_INIT;
		const DWORD count = shim->listener->GetEventHandles(shim->listener, handles, ARRAYSIZE(handles));
		if (count == 0) break;
		if (WaitForMultipleObjects(count, handles, FALSE, 200) == WAIT_FAILED) break;
		if (!shim->listener->CheckFileDescriptor(shim->listener)) break;
	}
	atomic_store(&shim->stop_requested, true);
	EnterCriticalSection(&shim->lock);
	const int peer_sockfd = shim->peer_sockfd;
	LeaveCriticalSection(&shim->lock);
	interrupt_peer_socket(peer_sockfd);
	shim->listener->Close(shim->listener);
	freerdp_listener_free(shim->listener);
	shim->listener = NULL;
	EnterCriticalSection(&shim->lock);
	HANDLE worker = shim->peer_thread;
	shim->peer_thread = NULL;
	LeaveCriticalSection(&shim->lock);
	if (worker)
	{
		(void)WaitForSingleObject(worker, INFINITE);
		CloseHandle(worker);
	}
	WSACleanup();
	atomic_store(&shim->running, false);
	return 0;

fail_wsa:
	if (shim->listener)
	{
		freerdp_listener_free(shim->listener);
		shim->listener = NULL;
	}
	WSACleanup();
fail:
	atomic_store(&shim->running, false);
	return -1;
}

void nanokvm_rdp_shim_stop(NanokvmRdpShim* shim)
{
	if (!shim) return;
	atomic_store(&shim->stop_requested, true);
	EnterCriticalSection(&shim->lock);
	const int peer_sockfd = shim->peer_sockfd;
	LeaveCriticalSection(&shim->lock);
	interrupt_peer_socket(peer_sockfd);
	if (shim->frame_event) SetEvent(shim->frame_event);
}

void nanokvm_rdp_shim_disconnect_active(NanokvmRdpShim* shim)
{
	if (!shim) return;
	EnterCriticalSection(&shim->lock);
	const int peer_sockfd = shim->peer_sockfd;
	LeaveCriticalSection(&shim->lock);
	interrupt_peer_socket(peer_sockfd);
}

void nanokvm_rdp_shim_free(NanokvmRdpShim* shim)
{
	if (!shim) return;
	nanokvm_rdp_shim_stop(shim);
	EnterCriticalSection(&shim->lock);
	HANDLE worker = shim->peer_thread;
	shim->peer_thread = NULL;
	LeaveCriticalSection(&shim->lock);
	if (worker)
	{
		(void)WaitForSingleObject(worker, INFINITE);
		CloseHandle(worker);
	}
	for (size_t i = 0; i < SHIM_H264_QUEUE_CAPACITY; i++)
		free(shim->h264_queue[i]);
	free(shim->pending_bgra);
	if (shim->frame_event) CloseHandle(shim->frame_event);
	DeleteCriticalSection(&shim->frame_lock);
	DeleteCriticalSection(&shim->lock);
	free(shim->bind_address);
	free(shim->certificate);
	free(shim->private_key);
	free(shim);
}

bool nanokvm_rdp_shim_send_h264(NanokvmRdpShim* shim, const uint8_t* data, size_t length,
	                            bool keyframe)
{
	if (!shim || !data || length == 0) return false;
	EnterCriticalSection(&shim->frame_lock);
	if (shim->h264_awaiting_idr && !keyframe)
	{
		LeaveCriticalSection(&shim->frame_lock);
		return true;
	}
	if (shim->h264_queue_count == SHIM_H264_QUEUE_CAPACITY)
	{
		for (size_t i = 0; i < SHIM_H264_QUEUE_CAPACITY; i++)
		{
			free(shim->h264_queue[i]);
			shim->h264_queue[i] = NULL;
			shim->h264_queue_lengths[i] = 0;
		}
		shim->h264_queue_head = 0;
		shim->h264_queue_count = 0;
		shim->h264_awaiting_idr = true;
		LeaveCriticalSection(&shim->frame_lock);
		shim_event(shim, NANOKVM_RDP_EVENT_H264_QUEUE_OVERFLOW, SHIM_H264_QUEUE_CAPACITY, 0);
		return true;
	}
	const uint8_t slot = (uint8_t)((shim->h264_queue_head + shim->h264_queue_count) % SHIM_H264_QUEUE_CAPACITY);
	const bool ok = copy_buffer(&shim->h264_queue[slot], &shim->h264_queue_lengths[slot], data, length);
	if (ok && keyframe)
		shim->h264_awaiting_idr = false;
	if (ok)
		shim->h264_queue_count++;
	LeaveCriticalSection(&shim->frame_lock);
	if (ok) SetEvent(shim->frame_event);
	return ok;
}

bool nanokvm_rdp_shim_send_bgra(NanokvmRdpShim* shim, const uint8_t* data, size_t length)
{
	if (!shim || !data || length == 0) return false;
	EnterCriticalSection(&shim->frame_lock);
	const bool ok = copy_buffer(&shim->pending_bgra, &shim->pending_bgra_length, data, length);
	LeaveCriticalSection(&shim->frame_lock);
	if (ok) SetEvent(shim->frame_event);
	return ok;
}
