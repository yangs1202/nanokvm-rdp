#ifndef NANOKVM_RDP_INPUT_ROUTER_H
#define NANOKVM_RDP_INPUT_ROUTER_H

#include <stdbool.h>
#include <stdint.h>

/* The router accepts already-decoded RDP input meaning. It deliberately has
 * no FreeRDP dependency so its v1 control payloads can be unit tested alone. */
typedef struct
{
	uint8_t type;
	uint8_t payload[12];
	uint16_t length;
} InputRouterMessage;

typedef struct
{
	uint16_t width;
	uint16_t height;
	bool swap_alt_command;
	uint8_t pointer_buttons;
	uint8_t keyboard_modifiers;
	bool control_space_down;
} InputRouter;

void input_router_init(InputRouter* router, uint16_t width, uint16_t height,
	                   bool swap_alt_command);
void input_router_keyboard(InputRouter* router, uint8_t code, bool extended,
	                       bool release, InputRouterMessage* message);
/* Returns false for unsupported UTF-16 code units. A release is accepted but
 * produces no message, preserving the v1 text-input contract. */
bool input_router_unicode(InputRouter* router, uint16_t code, bool release,
	                      InputRouterMessage* message);
void input_router_synchronize(InputRouter* router, InputRouterMessage* message);
void input_router_release_all(InputRouter* router, InputRouterMessage* message);
void input_router_absolute_pointer(InputRouter* router, uint16_t x, uint16_t y,
	                               uint16_t flags, bool wheel, InputRouterMessage* message);
void input_router_relative_pointer(InputRouter* router, int16_t x_delta, int16_t y_delta,
	                               uint16_t flags, bool wheel, InputRouterMessage* message);

#endif
