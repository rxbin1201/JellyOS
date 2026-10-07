/*
 * What the PS/2 driver found (for diagnostics and the kernel tests).
 */

#ifndef DRIVERS_INPUT_PS2_H
#define DRIVERS_INPUT_PS2_H

#include <stdbool.h>

/* An i8042 controller answered and the keyboard interrupt is routed. */
bool     ps2_keyboard_present(void);

/* Bytes per mouse packet: 3, 4 with a wheel, 0 without a mouse. */
unsigned ps2_mouse_packet_length(void);

#endif
