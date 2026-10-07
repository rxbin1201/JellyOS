/*
 * Input manager (README section 37).
 *
 *     keyboard, mouse, tablet drivers  ─▶  input manager  ─▶  standardized events
 *
 * Drivers report jelly_input_event_t (key codes from <jelly/input.h>); the
 * manager stamps them and copies them into every open input queue.
 * Applications never see the hardware: the display server reads the
 * queue and routes the events to windows.
 */

#ifndef INPUT_INPUT_H
#define INPUT_INPUT_H

#include "core/object.h"

#include <jelly/syscall.h>

/* Register an input device; returns its number for the events' `device` field. */
uint32_t input_register_device(const char *name);

/* Report an event (time and device filled in here). Safe in interrupt context. */
void     input_report(uint32_t device, uint32_t type, uint32_t code, int32_t value, int32_t dx, int32_t dy,
                      int32_t x, int32_t y, uint32_t flags);

/* A new event queue (OBJECT_INPUT, waitable while events are pending). */
status_t input_open(object_t **queue);
/* Take up to `count` events; returns how many. */
size_t   input_read(object_t *queue, jelly_input_event_t *events, size_t count);

uint32_t input_device_count(void);

#endif
