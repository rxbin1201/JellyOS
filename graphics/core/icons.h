/*
 * Built-in icons, drawn from simple shapes at any size (no image files
 * needed). They share the JellyOS palette; `accent` tints the main shape.
 */

#ifndef GRAPHICS_CORE_ICONS_H
#define GRAPHICS_CORE_ICONS_H

#include "graphics/core/canvas.h"

typedef enum {
    ICON_NONE,
    ICON_FOLDER,
    ICON_FILE,
    ICON_TEXT,
    ICON_PROGRAM,
    ICON_TERMINAL,
    ICON_FILES,
    ICON_SETTINGS,
    ICON_POWER,
    ICON_NETWORK,
    ICON_INFO,
    ICON_JELLY,      /* the JellyOS mark: a jellyfish dome */
    ICON_DEMO,
    ICON_LOGOUT,
    ICON_RESTART,
} icon_t;

void   icon_draw(canvas_t *canvas, icon_t icon, int32_t x, int32_t y, int32_t size, color_t accent);
/* Look up an icon by name ("folder", "terminal", ...); ICON_PROGRAM if unknown. */
icon_t icon_by_name(const char *name);

#endif
