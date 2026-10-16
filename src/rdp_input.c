#include "gateway_internal.h"
static bool client_send_input_message(Client* client, const InputRouterMessage* message)
{
	return message->type == 0 ||
	       server_send_control(client->server, message->type, message->payload, message->length);
}

/* Keep the RDP client's cursor independent from the captured host video. The
 * host cursor will eventually appear in a video frame, but that frame can be
 * delayed by mobile decoder/backpressure. RDP has a small standalone pointer
 * position update for exactly this case. */
static void client_send_pointer_position(Client* client, UINT16 x, UINT16 y)
{
	rdpUpdate* update = client->context.update;
	if (!update || !update->pointer || !update->pointer->PointerPosition)
		return;
	/* A slow bitmap write must not delay the local cursor. If the output
	 * thread currently owns the socket, skip this echo; the next move retries. */
	if (client->output_lock_ready && !TryEnterCriticalSection(&client->output_lock))
		return;
	if (!client->pointer_position_logged && update->pointer->PointerSystem)
	{
		POINTER_SYSTEM_UPDATE pointer_system = { .type = SYSPTR_DEFAULT };
		(void)update->pointer->PointerSystem(&client->context, &pointer_system);
	}
	POINTER_POSITION_UPDATE position = {
		.xPos = hid_clamp_absolute(x, client->render_width),
		.yPos = hid_clamp_absolute(y, client->render_height),
	};
	const bool wrote = update->pointer->PointerPosition(&client->context, &position);
	client_output_unlock(client);
	if (!wrote)
		return;
	EnterCriticalSection(&client->lock);
	client->stats.pointer_position_updates++;
	LeaveCriticalSection(&client->lock);
	if (!client->pointer_position_logged)
	{
		gateway_log("INFO", "RDP pointer position fast path 활성화");
		client->pointer_position_logged = true;
	}
}

static bool input_trace_enabled(void)
{
	const char* value = getenv("NANOKVM_INPUT_TRACE");
	return value && (strcmp(value, "1") == 0 || strcmp(value, "shift") == 0);
}

static bool input_trace_scancode(uint8_t code, bool extended)
{
	/* Keep the opt-in trace limited to the Shift+a/Shift+1 diagnostic. */
	return !extended && (code == 0x02 || code == 0x1e || code == 0x2a || code == 0x36);
}

BOOL on_keyboard(rdpInput* input, UINT16 flags, UINT8 code)
{
	Client* client = (Client*)input->context;
	const bool raw_extended = (flags & KBD_FLAGS_EXTENDED) != 0;
	const bool release = (flags & KBD_FLAGS_RELEASE) != 0;
	InputRouterMessage message;
	uint8_t modifiers_before = 0;
	uint8_t modifiers_after = 0;
	bool control_space_down = false;
	EnterCriticalSection(&client->lock);
	modifiers_before = client->input_router.keyboard_modifiers;
	input_router_keyboard(&client->input_router, code, raw_extended, release, &message);
	modifiers_after = client->input_router.keyboard_modifiers;
	control_space_down = client->input_router.control_space_down;
	LeaveCriticalSection(&client->lock);
	if (modifiers_after != 0 || code == 0x3a)
	{
		gateway_log("INFO",
		            "RDP modifier diagnostic raw=0x%02X extended=%u release=%u mapped=0x%02X extended=%u modifiers=0x%02X",
		            code, (unsigned)raw_extended, (unsigned)release,
		            message.payload[0], (unsigned)message.payload[1],
		            modifiers_after);
	}
	if (code == 0x39 && !raw_extended &&
	    ((modifiers_after & 0x11U) || control_space_down))
	{
		gateway_log("INFO", "RDP Control+Space diagnostic release=%u modifiers=0x%02X",
		            (unsigned)release, modifiers_after);
	}
	const bool input_trace = input_trace_enabled() && input_trace_scancode(code, raw_extended);
	uint64_t input_trace_epoch = 0;
	if (input_trace)
		input_trace_epoch = agent_transport_current_epoch(&client->server->transport);
	const bool sent = client_send_input_message(client, &message);
	if (input_trace)
	{
		gateway_log("INFO",
		            "INPUT TRACE raw=0x%02X extended=%u release=%u modifiers_before=0x%02X modifiers_after=0x%02X payload=0x%02X/%u/%u epoch=%llu sent=%u",
		            code, (unsigned)raw_extended, (unsigned)release, modifiers_before,
		            modifiers_after, message.payload[0],
		            (unsigned)message.payload[1], (unsigned)message.payload[2],
		            (unsigned long long)input_trace_epoch, (unsigned)sent);
	}
	if (sent && !client->keyboard_input_logged)
	{
		gateway_log("INFO", "RDP keyboard scancode → NanoKVM agent HID 전달 확인");
		client->keyboard_input_logged = true;
	}
	return sent;
}

BOOL on_unicode_keyboard(rdpInput* input, UINT16 flags, UINT16 code)
{
	Client* client = (Client*)input->context;
	const uint64_t received_at = monotonic_milliseconds();
	InputRouterMessage message;
	if (!input_router_unicode(&client->input_router, code, (flags & KBD_FLAGS_RELEASE) != 0, &message))
	{
		gateway_log("WARN", "지원하지 않는 RDP Unicode keyboard code unit U+%04X", code);
		return FALSE;
	}
	if (message.type == 0)
		return TRUE;
	const bool sent = client_send_input_message(client, &message);
	gateway_log("INFO", "RDP Unicode input received_ms=%llu ascii=%u bytes=%u forward_ms=%llu ok=%u",
	            (unsigned long long)received_at, (unsigned)(code <= 0x7fU), message.length,
	            (unsigned long long)(monotonic_milliseconds() - received_at), (unsigned)sent);
	if (!sent)
		gateway_log("WARN", "RDP Unicode keyboard text를 NanoKVM agent에 전달하지 못했습니다");
	return sent;
}

BOOL client_release_all_inputs(Client* client, const char* reason)
{
	InputRouterMessage message;
	EnterCriticalSection(&client->lock);
	input_router_release_all(&client->input_router, &message);
	LeaveCriticalSection(&client->lock);
	const bool sent = client_send_input_message(client, &message);
	if (sent && reason)
	{
		gateway_log("INFO", "RDP input state resync: %s → RELEASE_ALL 전송", reason);
	}
	return sent;
}

BOOL on_synchronize(rdpInput* input, UINT32 toggle_states)
{
	Client* client = (Client*)input->context;
	uint8_t previous_modifiers = 0;
	uint8_t previous_buttons = 0;
	EnterCriticalSection(&client->lock);
	previous_modifiers = client->input_router.keyboard_modifiers;
	previous_buttons = client->input_router.pointer_buttons;
	LeaveCriticalSection(&client->lock);
	gateway_log("INFO",
	            "RDP Synchronize toggles=0x%08X previous_modifiers=0x%02X previous_buttons=0x%02X",
	            toggle_states, previous_modifiers, previous_buttons);
	/* TS_SYNC_EVENT resets held keys. Subsequent down events restore held keys;
	 * do not cancel preceding input that is still waiting for USB delivery. */
	InputRouterMessage output;
	EnterCriticalSection(&client->lock);
	input_router_synchronize(&client->input_router, &output);
	LeaveCriticalSection(&client->lock);
	return client_send_input_message(client, &output);
}

BOOL on_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	Client* client = (Client*)input->context;
	const bool has_wheel = (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)) != 0U;
	InputRouterMessage message;
	uint8_t pointer_buttons = 0;
	uint8_t keyboard_modifiers = 0;
	EnterCriticalSection(&client->lock);
	input_router_absolute_pointer(&client->input_router, x, y, flags, has_wheel, &message);
	pointer_buttons = client->input_router.pointer_buttons;
	keyboard_modifiers = client->input_router.keyboard_modifiers;
	LeaveCriticalSection(&client->lock);
	if (!has_wheel)
		client_send_pointer_position(client, x, y);
	if ((flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3)) != 0)
	{
		gateway_log("INFO",
		            "RDP absolute button flags=0x%04X x=%u y=%u down=%u mask=0x%02X modifiers=0x%02X",
		            flags, x, y, (unsigned)((flags & PTR_FLAGS_DOWN) != 0),
		            pointer_buttons, keyboard_modifiers);
	}
	/* 휠 비트만 있는 이벤트는 좌표가 0이다. 위치 보고와 분리해 커서가 원점으로 튀지 않게 한다. */
	const bool sent = client_send_input_message(client, &message);
	if (sent && !has_wheel && !client->pointer_input_logged)
	{
		gateway_log("INFO", "RDP absolute pointer → NanoKVM agent HID 전달 확인");
		client->pointer_input_logged = true;
	}
	if (sent && has_wheel &&
	    !client->wheel_input_logged)
	{
		gateway_log("INFO", "RDP wheel → NanoKVM agent HID 전달 확인");
		client->wheel_input_logged = true;
	}
	return sent;
}

BOOL on_extended_mouse(rdpInput* input, UINT16 flags, UINT16 x, UINT16 y)
{
	return on_mouse(input, hid_pointer_flags_from_extended(flags), x, y);
}

BOOL on_relative_mouse(rdpInput* input, UINT16 flags, INT16 x_delta, INT16 y_delta)
{
	Client* client = (Client*)input->context;
	bool vertical_wheel = (flags & PTR_FLAGS_WHEEL) != 0;
	bool horizontal_wheel = (flags & PTR_FLAGS_HWHEEL) != 0;
	/* Windows App의 가로 스크롤은 상대 마우스의 가로 델타와 세로 휠 비트를 같이 보낸다.
	 * 휠 비트만 보면 페이지가 세로로만 움직이므로 가로 델타를 AC Pan으로 바꾼다. */
	if (!horizontal_wheel && vertical_wheel && x_delta != 0 && y_delta == 0)
	{
		flags = (uint16_t)((flags & (uint16_t)~(PTR_FLAGS_WHEEL | WHEEL_ROTATION_MASK)) |
		                   PTR_FLAGS_HWHEEL | (uint16_t)(flags & WHEEL_ROTATION_MASK));
		if (x_delta < 0)
			flags |= PTR_FLAGS_WHEEL_NEGATIVE;
		else
			flags = (uint16_t)(flags & (uint16_t)~PTR_FLAGS_WHEEL_NEGATIVE);
		horizontal_wheel = true;
	}
	const bool has_wheel = (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)) != 0;
	InputRouterMessage message;
	uint8_t pointer_buttons = 0;
	uint8_t keyboard_modifiers = 0;
	EnterCriticalSection(&client->lock);
	input_router_relative_pointer(&client->input_router, x_delta, y_delta, flags, has_wheel, &message);
	pointer_buttons = client->input_router.pointer_buttons;
	keyboard_modifiers = client->input_router.keyboard_modifiers;
	LeaveCriticalSection(&client->lock);
	if ((flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3)) != 0)
	{
		gateway_log("INFO",
		            "RDP relative button flags=0x%04X dx=%d dy=%d mask=0x%02X down=%u modifiers=0x%02X",
		            flags, x_delta, y_delta, pointer_buttons,
		            (unsigned)((flags & PTR_FLAGS_DOWN) != 0), keyboard_modifiers);
	}
	const bool sent = client_send_input_message(client, &message);
	if (sent && has_wheel &&
	    !client->wheel_input_logged)
	{
		gateway_log("INFO", "RDP relative wheel → NanoKVM agent HID 전달 확인");
		client->wheel_input_logged = true;
	}
	return sent;
}
