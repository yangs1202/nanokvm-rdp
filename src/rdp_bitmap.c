#include "gateway_internal.h"
bool bitmap_stream_rfx_supported(const rdpSettings* settings)
{
	const uint32_t supported =
	    freerdp_settings_get_uint32(settings, FreeRDP_SurfaceCommandsSupported);
	return freerdp_settings_get_bool(settings, FreeRDP_RemoteFxCodec) &&
	       freerdp_settings_get_uint32(settings, FreeRDP_RemoteFxCodecId) != 0 &&
	       (supported & SURFCMDS_STREAM_SURFACE_BITS) != 0;
}

bool bitmap_stream_nsc_supported(const rdpSettings* settings)
{
	const uint32_t supported =
	    freerdp_settings_get_uint32(settings, FreeRDP_SurfaceCommandsSupported);
	return freerdp_settings_get_bool(settings, FreeRDP_NSCodec) &&
	       freerdp_settings_get_uint32(settings, FreeRDP_NSCodecId) != 0 &&
	       (supported & SURFCMDS_SET_SURFACE_BITS) != 0;
}

static void classic_tile_copy(uint8_t* destination, const uint8_t* source, uint16_t width,
	                          uint16_t left, uint16_t top, uint16_t columns, uint16_t rows)
{
	const size_t row_length = (size_t)columns * 4U;
	for (uint16_t row = 0; row < rows; row++)
	{
		const size_t offset = ((size_t)(top + row) * width + left) * 4U;
		memcpy(destination + offset, source + offset, row_length);
	}
}

/* FreeRDP's interleaved 16-bit compressor aborts when a noisy tile makes its
 * internal pixel counter exceed int16 range. Pack RGB565 directly instead. */
static void classic_pack_rgb16(uint8_t* destination, const uint8_t* bgra, uint16_t width,
                               uint16_t left, uint16_t top, uint16_t columns, uint16_t rows)
{
	size_t written = 0;
	for (uint16_t row = 0; row < rows; row++)
	{
		const size_t offset = ((size_t)(top + row) * width + left) * 4U;
		for (uint16_t column = 0; column < columns; column++)
		{
			const uint8_t* pixel = bgra + offset + (size_t)column * 4U;
			const uint16_t color = (uint16_t)(((pixel[2] & 0xF8U) << 8) |
			                                  ((pixel[1] & 0xFCU) << 3) | (pixel[0] >> 3));
			destination[written++] = (uint8_t)color;
			destination[written++] = (uint8_t)(color >> 8);
		}
	}
}

/* BitmapUpdate and SurfaceBits share FreeRDP's output buffer with pointer
 * PDUs. Serialize those writes, but keep the peer thread free to read input. */
static void client_output_lock(Client* client)
{
	if (client->output_lock_ready)
		EnterCriticalSection(&client->output_lock);
}

void client_output_unlock(Client* client)
{
	if (client->output_lock_ready)
		LeaveCriticalSection(&client->output_lock);
}

static bool client_output_write_ready(Client* client)
{
	/* FreeRDP aborts inside IsWriteBlocked once Disconnect clears frontBio.
	 * A closing or disconnected peer is not writable; do not probe it. */
	if (!client->peer || !client->peer->connected || client_should_stop(client) ||
	    !client->peer->IsWriteBlocked || !client->peer->DrainOutputBuffer)
		return false;
	if (!client->peer->IsWriteBlocked(client->peer))
		return true;
	/* A blocked socket means the client has not consumed the previous update.
	 * Drain what is already writable, then skip this frame. Forcing another
	 * BitmapUpdate onto a full socket is what closes the session. */
	(void)client->peer->DrainOutputBuffer(client->peer);
	return client->peer->connected && !client_should_stop(client) &&
	       client->peer->IsWriteBlocked && !client->peer->IsWriteBlocked(client->peer);
}

static bool send_classic_bitmap_frame(Client* client, const uint8_t* bgra, size_t length)
{
	const uint16_t width = client->render_width;
	const uint16_t height = client->render_height;
	const size_t expected_length = (size_t)width * height * 4U;
	const rdpSettings* settings = client->context.settings;
	const uint16_t bits_per_pixel = 16;
	BITMAP_DATA rectangles[CLASSIC_BITMAP_BATCH] = WINPR_C_ARRAY_INIT;
	BITMAP_UPDATE bitmap = WINPR_C_ARRAY_INIT;
	const uint32_t negotiated_update_size =
	    freerdp_settings_get_uint32(settings, FreeRDP_MultifragMaxRequestSize);
	const uint32_t max_update_size =
	    negotiated_update_size < CLASSIC_MAX_UPDATE_SIZE ? negotiated_update_size : CLASSIC_MAX_UPDATE_SIZE;
	uint32_t update_size = 1024U;
	uint16_t rectangle_count = 0;
	uint32_t frame_tiles = 0;
	uint64_t frame_bytes = 0;
	const uint64_t slice_started_at = monotonic_milliseconds();
	bool yielded = false;

	if (!client->context.update || !client->context.update->BitmapUpdate ||
	    settings == NULL || !client->classic_encoded ||
	    length != expected_length || client_should_stop(client))
		return false;
	if (!client_output_write_ready(client))
	{
		client->classic_frame_open = true;
		return true;
	}
	if (!client->previous_bitmap)
	{
		client->previous_bitmap = malloc(expected_length);
		client->previous_bitmap_length = client->previous_bitmap ? expected_length : 0;
	}
	if (!client->previous_bitmap || client->previous_bitmap_length != expected_length)
		return false;

	for (uint16_t top = client->classic_resume_top; top < height; top += CLASSIC_TILE_HEIGHT)
	{
		const uint16_t rows = MIN(CLASSIC_TILE_HEIGHT, (uint16_t)(height - top));
		const uint16_t first_left = top == client->classic_resume_top ? client->classic_resume_left : 0;
		for (uint16_t left = first_left; left < width; left += CLASSIC_TILE_WIDTH)
		{
			const uint16_t columns = MIN(CLASSIC_TILE_WIDTH, (uint16_t)(width - left));
			if (frame_tiles >= 1 &&
			    monotonic_milliseconds() - slice_started_at >= CLASSIC_SLICE_BUDGET_MS)
			{
				client->classic_resume_left = left;
				client->classic_resume_top = top;
				client->classic_frame_open = true;
				yielded = true;
				break;
			}
			if (client->previous_bitmap_valid &&
			    !bitmap_tile_changed(client->previous_bitmap, bgra, width, left, top, columns, rows))
				continue;
			const uint32_t encoded_length = (uint32_t)columns * rows * 2U;
			uint8_t* encoded_data =
			    client->classic_encoded + (size_t)rectangle_count * CLASSIC_TILE_MAX_ENCODED;
			if (encoded_length == 0 || encoded_length > CLASSIC_TILE_MAX_ENCODED)
				return false;
			if (rectangle_count > 0 &&
			    update_size + encoded_length + 16U >= max_update_size)
			{
				bitmap.number = rectangle_count;
				bitmap.rectangles = rectangles;
				bitmap.skipCompression = TRUE;
				client_output_lock(client);
				const bool wrote = client->context.update->BitmapUpdate(&client->context, &bitmap);
				client_output_unlock(client);
				if (!wrote)
					return false;
				rectangle_count = 0;
				update_size = 1024U;
				encoded_data = client->classic_encoded;
				if (!client_output_write_ready(client))
				{
					client->classic_resume_left = left;
					client->classic_resume_top = top;
					client->classic_frame_open = true;
					yielded = true;
					break;
				}
			}
			classic_pack_rgb16(encoded_data, bgra, width, left, top, columns, rows);
			BITMAP_DATA* rectangle = &rectangles[rectangle_count];
			rectangle->destLeft = left;
			rectangle->destTop = top;
			rectangle->destRight = left + columns - 1;
			rectangle->destBottom = top + rows - 1;
			rectangle->width = columns;
			rectangle->height = rows;
			rectangle->bitsPerPixel = bits_per_pixel;
			rectangle->bitmapLength = WINPR_ASSERTING_INT_CAST(uint16_t, encoded_length);
			rectangle->bitmapDataStream = encoded_data;
			rectangle->compressed = FALSE;
			rectangle->cbCompFirstRowSize = 0;
			rectangle->cbCompMainBodySize = encoded_length;
			rectangle->cbScanWidth = columns * (bits_per_pixel / 8U);
			rectangle->cbUncompressedSize = columns * rows * (bits_per_pixel / 8U);
			rectangle_count++;
			update_size += encoded_length + 16U;
			classic_tile_copy(client->previous_bitmap, bgra, width, left, top, columns, rows);
			frame_tiles++;
			frame_bytes += encoded_length;
			if (rectangle_count == CLASSIC_BITMAP_BATCH)
			{
				bitmap.number = rectangle_count;
				bitmap.rectangles = rectangles;
				bitmap.skipCompression = TRUE;
				client_output_lock(client);
				const bool wrote = client->context.update->BitmapUpdate(&client->context, &bitmap);
				client_output_unlock(client);
				if (!wrote)
					return false;
				rectangle_count = 0;
				update_size = 1024U;
				if (client->peer && client->peer->IsWriteBlocked &&
				    client->peer->connected && !client_should_stop(client) &&
				    client->peer->IsWriteBlocked(client->peer))
				{
					client->classic_resume_left = (uint16_t)(left + CLASSIC_TILE_WIDTH);
					client->classic_resume_top = top;
					if (client->classic_resume_left >= width)
					{
						client->classic_resume_left = 0;
						client->classic_resume_top = (uint16_t)(top + CLASSIC_TILE_HEIGHT);
					}
					client->classic_frame_open = true;
					yielded = true;
					break;
				}
			}
		}
		if (yielded) break;
	}
	if (rectangle_count > 0)
	{
		bitmap.number = rectangle_count;
		bitmap.rectangles = rectangles;
		bitmap.skipCompression = TRUE;
		client_output_lock(client);
		const bool wrote = client->context.update->BitmapUpdate(&client->context, &bitmap);
		client_output_unlock(client);
		if (!wrote)
			return false;
	}
	EnterCriticalSection(&client->lock);
	client->stats.classic_tiles_sent += frame_tiles;
	client->stats.classic_bytes_sent += frame_bytes;
	LeaveCriticalSection(&client->lock);
	if (yielded)
		return true;
	client->classic_resume_left = 0;
	client->classic_resume_top = 0;
	client->classic_frame_open = false;
	if (frame_tiles > 0) client->previous_bitmap_valid = true;
	return true;
}

static bool send_progressive_frame(Client* client, const uint8_t* bgra, size_t length)
{
	const uint16_t width = client->render_width;
	const uint16_t height = client->render_height;
	const size_t expected_length = (size_t)width * height * 4U;
	REGION16 region = WINPR_C_ARRAY_INIT;
	RDPGFX_SURFACE_COMMAND command = WINPR_C_ARRAY_INIT;
	RDPGFX_START_FRAME_PDU start = WINPR_C_ARRAY_INIT;
	RDPGFX_END_FRAME_PDU end = WINPR_C_ARRAY_INIT;

	if (!client->progressive || !client->gfx || !client->gfx->SurfaceFrameCommand ||
	    length != expected_length || client_should_stop(client))
		return false;
	region16_init(&region);
	const uint16_t tile_columns = (uint16_t)((width + 63U) / 64U);
	const uint16_t tile_rows = (uint16_t)((height + 63U) / 64U);
	if (!client->previous_bitmap)
	{
		client->previous_bitmap = malloc(expected_length);
		client->previous_bitmap_length = client->previous_bitmap ? expected_length : 0;
	}
	const uint8_t* previous = client->previous_bitmap_valid &&
	                          client->previous_bitmap_length == expected_length
	                              ? client->previous_bitmap
	                              : NULL;
	size_t changed_tiles = 0;
	for (uint16_t tile_y = 0; tile_y < tile_rows; tile_y++)
	{
		for (uint16_t tile_x = 0; tile_x < tile_columns; tile_x++)
		{
			const uint16_t left = (uint16_t)(tile_x * 64U);
			const uint16_t top = (uint16_t)(tile_y * 64U);
			const uint16_t columns = (uint16_t)(left + 64U > width ? (uint16_t)(width - left) : 64U);
			const uint16_t rows = (uint16_t)(top + 64U > height ? (uint16_t)(height - top) : 64U);
			if (previous && !bitmap_tile_changed(previous, bgra, width, left, top, columns, rows))
				continue;
			RECTANGLE_16 rect = { .left = left, .top = top,
				.right = (UINT16)(left + columns), .bottom = (UINT16)(top + rows) };
			if (!region16_union_rect(&region, &region, &rect))
			{
				region16_uninit(&region);
				return false;
			}
			changed_tiles++;
		}
	}
	if (changed_tiles == 0)
	{
		region16_uninit(&region);
		return true;
	}
	const int encoded = progressive_compress(
	    client->progressive, bgra, WINPR_ASSERTING_INT_CAST(uint32_t, length),
	    PIXEL_FORMAT_BGRX32, width, height, (uint32_t)width * 4U, &region,
	    &command.data, &command.length);
	region16_uninit(&region);
	if (encoded < 0)
		return false;
	if (encoded == 0)
		return true;

	EnterCriticalSection(&client->lock);
	start.frameId = client->next_frame_id++;
	start.timestamp = (UINT32)monotonic_milliseconds();
	end.frameId = start.frameId;
	LeaveCriticalSection(&client->lock);
	command.surfaceId = 1;
	command.codecId = RDPGFX_CODECID_CAPROGRESSIVE;
	command.format = PIXEL_FORMAT_BGRX32;
	command.left = 0;
	command.top = 0;
	command.right = width;
	command.bottom = height;
	command.width = width;
	command.height = height;
	/* Register before submission: the channel receiver may ACK concurrently. */
	EnterCriticalSection(&client->lock);
	const bool tracked = frame_flow_sent(&client->frame_flow, start.frameId,
	                                     monotonic_milliseconds());
	LeaveCriticalSection(&client->lock);
	if (!tracked) return false;
	const UINT error = client->gfx->SurfaceFrameCommand(client->gfx, &command, &start, &end);
	command.data = NULL;
	if (error != CHANNEL_RC_OK)
	{
		gateway_log("ERROR", "RDPGFX Progressive frame 전송 실패");
		return false;
	}
	if (client->previous_bitmap && client->previous_bitmap_length == expected_length)
	{
		memcpy(client->previous_bitmap, bgra, expected_length);
		client->previous_bitmap_valid = true;
	}
	return true;
}

static bool send_bitmap_frame(Client* client, const uint8_t* bgra, size_t length)
{
	const uint16_t width = client->render_width;
	const uint16_t height = client->render_height;
	const size_t expected_length = (size_t)width * height * 4U;
	rdpSettings* settings = client->context.settings;
	rdpUpdate* update = client->context.update;
	SURFACE_BITS_COMMAND command = WINPR_C_ARRAY_INIT;
	RFX_RECT rect = { .x = 0, .y = 0, .width = width, .height = height };

	if (!settings || !update || length != expected_length || client_should_stop(client))
		return false;
	if (client->gfx_uses_progressive)
	{
		if (!send_progressive_frame(client, bgra, length))
			return false;
		goto sent;
	}
	if (!client->bitmap_uses_rfx && !client->nsc)
		goto sent_classic;
	if (!update->SurfaceBits || !client->bitmap_stream)
		return false;
	Stream_Clear(client->bitmap_stream);
	Stream_SetPosition(client->bitmap_stream, 0);
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
	if (!client_output_write_ready(client))
		return true;
	client_output_lock(client);
	const bool wrote = update->SurfaceBits(&client->context, &command);
	client_output_unlock(client);
	if (!wrote)
		return false;

sent_classic:
	if (!client->bitmap_uses_rfx && !client->nsc && !send_classic_bitmap_frame(client, bgra, length))
		return false;
sent:
	EnterCriticalSection(&client->lock);
	client->stats.bitmap_frames++;
	const uint32_t frame_count = client->stats.bitmap_frames;
	LeaveCriticalSection(&client->lock);
	if (frame_count == 1)
	{
		if (client->gfx_uses_progressive)
			gateway_log("INFO", "FoldVNC H.264 → FFmpeg BGRA → RDPGFX Progressive 첫 frame 전송 완료");
		else
			gateway_log("INFO", "FoldVNC H.264 → FFmpeg BGRA → RDP bitmap 첫 frame 전송 완료");
	}
	return true;
}

/* The decoder owns its frame; retain the reference, not an 8 MB BGRA copy. */
static bool on_decoded_frame(void* context, const AVFrame* frame)
{
	Client* client = context;
	if (client_should_stop(client)) return false;
	AVFrame* retained = av_frame_clone(frame);
	if (!retained) return false;
	EnterCriticalSection(&client->lock);
	/* A classic slice still owns converter.frame. Keep the newest decoded
	 * frame queued, but do not replace the pixels that slice is sending. */
	AVFrame* stale = client->classic_frame_open ? client->pending_next_decoded
	                                            : client->pending_decoded;
	if (client->classic_frame_open)
		client->pending_next_decoded = retained;
	else
		client->pending_decoded = retained;
	client->stats.decoded_frames++;
	client->stats.bitmap_queued_frames++;
	if (stale)
	{
		client->stats.bitmap_queue_drops++;
		client->stats.bitmap_stale_drops++;
	}
	client->stats.last_decode_latency_ms = monotonic_milliseconds() - client->last_rtp_received_at;
	LeaveCriticalSection(&client->lock);
	av_frame_free(&stale);
	if (client->output_wakeup) (void)SetEvent(client->output_wakeup);
	return true;
}

static bool client_flush_pending_bitmap(Client* client)
{
	if (!client_output_write_ready(client))
		return true;
	const uint64_t now = monotonic_milliseconds();
	EnterCriticalSection(&client->lock);
	client->stats.bitmap_flushes++;
	unsigned interval = client->gfx_uses_progressive
	                        ? frame_flow_interval(&client->frame_flow) : BITMAP_FRAME_INTERVAL_MS;
	/* A classic frame is sliced. Pace only between completed frames; an open
	 * slice must resume immediately so input is not delayed by the old 250 ms cap. */
	const bool classic_resume = !client->gfx_uses_progressive && client->classic_frame_open;
	if ((client->gfx_uses_progressive && frame_flow_blocked(&client->frame_flow)) ||
	    (!classic_resume && client->bitmap_last_send_started_at &&
	     now - client->bitmap_last_send_started_at < interval))
	{
		LeaveCriticalSection(&client->lock);
		return true;
	}
	AVFrame* frame = client->pending_decoded;
	if (!frame && !classic_resume)
	{
		client->stats.bitmap_empty_flushes++;
		LeaveCriticalSection(&client->lock);
		return true;
	}
	/* Keep replacing the pending raw frame while blocked. Even when capture
	 * stops, the final frame survives until pacing/ACK/transport allows it. */
	if (frame) client->pending_decoded = NULL;
	if (!classic_resume) client->bitmap_last_send_started_at = now;
	LeaveCriticalSection(&client->lock);

	bool sent = false;
	if (classic_resume)
	{
		sent = send_classic_bitmap_frame(client, client->converter.frame,
		                                 client->converter.frame_size);
	}
	else
	{
		const bool converted = ffmpeg_converter_convert(&client->converter, frame,
		                                                client->render_width, client->render_height);
		av_frame_free(&frame);
		sent = converted && send_bitmap_frame(client, client->converter.frame,
		                                     client->converter.frame_size);
		EnterCriticalSection(&client->lock);
		if (converted) client->stats.converted_frames++;
		LeaveCriticalSection(&client->lock);
	}
	if (sent && !client->classic_frame_open)
	{
		EnterCriticalSection(&client->lock);
		AVFrame* next = client->pending_next_decoded;
		client->pending_next_decoded = NULL;
		if (next)
		{
			if (client->pending_decoded)
			{
				client->stats.bitmap_queue_drops++;
				client->stats.bitmap_stale_drops++;
				av_frame_free(&client->pending_decoded);
			}
			client->pending_decoded = next;
			next = NULL;
		}
		LeaveCriticalSection(&client->lock);
		av_frame_free(&next);
	}
	const uint64_t completed_at = monotonic_milliseconds();
	if (!client->classic_frame_open && completed_at - now >= 100U)
	{
		gateway_log("WARN", "RDP slow frame send elapsed_ms=%llu ok=%u",
		            (unsigned long long)(completed_at - now), (unsigned)sent);
	}
	EnterCriticalSection(&client->lock);
	if (sent && !client->classic_frame_open)
		client->stats.last_rdp_send_ms = completed_at - client->bitmap_last_send_started_at;
	if (client->gfx_uses_progressive)
		frame_flow_send_cost(&client->frame_flow, completed_at - now, completed_at);
	LeaveCriticalSection(&client->lock);
	if (!sent)
	{
		gateway_log("ERROR", "RDP bitmap 변환 또는 전송 실패");
		client_stop(client);
	}
	return sent;
}

DWORD WINAPI output_thread(LPVOID argument)
{
	Client* client = (Client*)argument;
	while (!client_should_stop(client))
	{
		if (client->shutdown_event &&
		    WaitForSingleObject(client->shutdown_event, 0) == WAIT_OBJECT_0)
			break;
		if (!client->bitmap_fallback_active)
		{
			if (client->output_wakeup)
				(void)WaitForSingleObject(client->output_wakeup, 50);
			continue;
		}
		EnterCriticalSection(&client->lock);
		const bool pending = client->pending_decoded != NULL ||
		                     client->pending_next_decoded != NULL || client->classic_frame_open;
		const bool waiting_for_ack = client->gfx_uses_progressive &&
		                             frame_flow_blocked(&client->frame_flow);
		const uint64_t last_send = client->bitmap_last_send_started_at;
		const uint64_t interval = client->gfx_uses_progressive
		                              ? frame_flow_interval(&client->frame_flow)
		                              : BITMAP_FRAME_INTERVAL_MS;
		LeaveCriticalSection(&client->lock);
		DWORD timeout = 50;
		if (pending && !waiting_for_ack)
		{
			const uint64_t now = monotonic_milliseconds();
			if (client->classic_frame_open || last_send == 0 || now - last_send >= interval)
			{
				if (!client_flush_pending_bitmap(client))
					break;
				timeout = 0;
			}
			else
				timeout = WINPR_ASSERTING_INT_CAST(DWORD, interval - (now - last_send));
		}
		if (timeout == 0 && client->peer && client->peer->IsWriteBlocked &&
		    client->peer->connected && !client_should_stop(client) &&
		    client->peer->IsWriteBlocked(client->peer))
			timeout = 20;
		if (timeout == 0)
			continue;
		HANDLE handles[2] = WINPR_C_ARRAY_INIT;
		DWORD count = 0;
		if (client->output_wakeup)
			handles[count++] = client->output_wakeup;
		if (client->shutdown_event)
			handles[count++] = client->shutdown_event;
		if (count == 0)
			Sleep(timeout);
		else
			(void)WaitForMultipleObjects(count, handles, FALSE, timeout);
	}
	return 0;
}

DWORD WINAPI bitmap_video_thread(LPVOID argument)
{
	Client* client = (Client*)argument;
	DeviceSessionLease lease = { 0 };
	FfmpegDecoder decoder = { 0 };
	H264Buffer fallback = { 0 };
	bool backend_ready = false;
	HANDLE yield_event = client->server->http_yield_event;
	if (yield_event)
		(void)SetEvent(yield_event);

	for (unsigned attempt = 0; attempt < 10 && !client_should_stop(client); attempt++)
	{
		if (device_session_acquire(client->server->device_session,
		                           DEVICE_SESSION_CONSUMER_BITMAP, &lease) &&
		    ffmpeg_decoder_start_raw(&decoder, on_decoded_frame, client))
		{
			backend_ready = true;
			break;
		}
		ffmpeg_decoder_stop(&decoder);
		device_session_release(&lease);
		Sleep(1000);
	}
	if (yield_event)
		(void)ResetEvent(yield_event);
	if (!backend_ready)
	{
		gateway_log("ERROR", "RTP/H.264 receiver 또는 FFmpeg decoder를 시작할 수 없습니다");
		client_stop(client);
		goto out;
	}
	gateway_log("INFO", "RTP/H.264 receiver와 FFmpeg BGRA decoder 시작 완료");
	while (!client_should_stop(client))
	{
		VideoSourceAccessUnit access_unit = { 0 };
		if (!device_session_read(&lease, &access_unit))
		{
			const int error = errno;
			if (client_should_stop(client))
				break;
			if (error == EAGAIN || error == EWOULDBLOCK)
				continue;
			gateway_log("ERROR", "RTP/H.264 frame 수신 실패");
			client_stop(client);
			break;
		}
		uint8_t* data = access_unit.data;
		const size_t length = access_unit.length;
		if (!data || length == 0)
		{
			video_source_release_access_unit(&access_unit);
			continue;
		}
		const uint32_t nal_types = access_unit.nal_types;
		const bool idr = (nal_types & (1U << 5)) != 0;
		const bool p_frame = (nal_types & (1U << 1)) != 0;
		EnterCriticalSection(&client->lock);
		client->last_rtp_received_at = monotonic_milliseconds();
		client->stats.rtp_nals++;
		client->stats.rtp_access_units++;
		if (idr)
			client->stats.rtp_idr_units++;
		if (p_frame)
			client->stats.rtp_p_units++;
		LeaveCriticalSection(&client->lock);
		const uint8_t* push_data = data;
		size_t push_length = length;
		if (!h264_ensure_annexb(&fallback, data, length, &push_data, &push_length))
		{
			video_source_release_access_unit(&access_unit);
			client_stop(client);
			break;
		}
		const bool packet_loss = access_unit.packet_loss;
		/* client_stop() makes the decoded-frame callback reject the frame.
		 * That is teardown, not a decoder failure, so do not log or stop again. */
		const bool stopping = client_should_stop(client);
		const bool pushed = stopping || ffmpeg_decoder_push(&decoder, push_data, push_length);
		video_source_release_access_unit(&access_unit);
		if (!stopping && !pushed)
		{
			gateway_log("ERROR", "FFmpeg H.264 decoder 입력 실패");
			client_stop(client);
			break;
		}
		if (packet_loss)
		{
			gateway_log("WARN", "RTP frame loss 감지; NanoKVM agent에 IDR 재동기화를 요청합니다");
		}
	}

out:
	h264_buffer_free(&fallback);
	ffmpeg_decoder_stop(&decoder);
	device_session_release(&lease);
	return 0;
}
