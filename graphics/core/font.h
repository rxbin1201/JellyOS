/*
 * Built-in 8x16 bitmap font (Latin-1). Shared by the kernel's framebuffer
 * console and the userspace graphics library.
 */

#ifndef GRAPHICS_CORE_FONT_H
#define GRAPHICS_CORE_FONT_H

#include <stdint.h>

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

extern const uint8_t font8x16[256][16];

#endif
