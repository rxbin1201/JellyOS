/*
 * Built-in icons drawn from rectangles, rounded rectangles and lines.
 * Coordinates are in 1/16 of the icon size, so icons scale cleanly.
 */

#include "graphics/core/icons.h"

#include <string.h>

typedef struct {
    canvas_t *c;
    int32_t   x, y, size;
} pen_t;

static int32_t u(const pen_t *p, int32_t sixteenths)
{
    return sixteenths * p->size / 16;
}

static rect_t box(const pen_t *p, int32_t x, int32_t y, int32_t w, int32_t h)
{
    return rect_make(p->x + u(p, x), p->y + u(p, y), u(p, w) > 0 ? u(p, w) : 1, u(p, h) > 0 ? u(p, h) : 1);
}

static void fill(const pen_t *p, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, color_t color)
{
    canvas_fill_rounded(p->c, box(p, x, y, w, h), u(p, radius), color);
}

static void line(const pen_t *p, int32_t x0, int32_t y0, int32_t x1, int32_t y1, color_t color)
{
    int32_t thickness = p->size >= 32 ? 2 : 1;
    for (int32_t t = 0; t < thickness; t++)
        canvas_line(p->c, p->x + u(p, x0) + t, p->y + u(p, y0), p->x + u(p, x1) + t, p->y + u(p, y1), color);
}

static void document(const pen_t *p, color_t paper, color_t edge)
{
    fill(p, 3, 1, 10, 14, 1, edge);
    fill(p, 4, 2, 8, 12, 1, paper);
}

void icon_draw(canvas_t *canvas, icon_t icon, int32_t x, int32_t y, int32_t size, color_t accent)
{
    pen_t p = { canvas, x, y, size };
    color_t white = RGB(0xFF, 0xFF, 0xFF), dark = RGB(0x2A, 0x24, 0x40), grey = RGB(0x9A, 0x96, 0xAA);
    color_t violet = RGB(0x7C, 0x3A, 0xED), teal = RGB(0x2D, 0xD4, 0xBF), pink = RGB(0xEF, 0x44, 0x6C);
    color_t amber = RGB(0xF5, 0xB0, 0x41);

    switch (icon) {
    case ICON_NONE:
        break;
    case ICON_FOLDER:
        fill(&p, 1, 3, 6, 3, 1, color_mix(amber, RGB(0, 0, 0), 40));
        fill(&p, 1, 4, 14, 10, 1, amber);
        fill(&p, 1, 6, 14, 8, 1, color_mix(amber, white, 50));
        break;
    case ICON_FILE:
        document(&p, white, grey);
        break;
    case ICON_TEXT:
        document(&p, white, grey);
        for (int i = 0; i < 4; i++)
            fill(&p, 5, 4 + i * 2, i == 3 ? 4 : 6, 1, 0, accent);
        break;
    case ICON_PROGRAM:
    case ICON_DEMO:
        fill(&p, 1, 2, 14, 12, 2, accent);
        fill(&p, 2, 5, 12, 8, 1, white);
        fill(&p, 2, 3, 2, 1, 0, white);
        if (icon == ICON_DEMO) {
            fill(&p, 4, 7, 4, 2, 1, accent);
            fill(&p, 4, 10, 8, 1, 0, grey);
        }
        break;
    case ICON_TERMINAL:
        fill(&p, 1, 2, 14, 12, 2, dark);
        line(&p, 4, 6, 6, 8, teal);
        line(&p, 6, 8, 4, 10, teal);
        fill(&p, 8, 10, 4, 1, 0, teal);
        break;
    case ICON_FILES:
        fill(&p, 1, 3, 6, 3, 1, color_mix(accent, RGB(0, 0, 0), 40));
        fill(&p, 1, 4, 14, 10, 1, accent);
        fill(&p, 5, 7, 6, 5, 1, white);
        break;
    case ICON_SETTINGS:
        /* a cog: ring with teeth */
        fill(&p, 7, 1, 2, 14, 1, accent);
        fill(&p, 1, 7, 14, 2, 1, accent);
        line(&p, 3, 3, 13, 13, accent);
        line(&p, 13, 3, 3, 13, accent);
        fill(&p, 3, 3, 10, 10, 5, accent);
        fill(&p, 6, 6, 4, 4, 2, white);
        break;
    case ICON_POWER:
        fill(&p, 2, 2, 12, 12, 6, pink);
        fill(&p, 4, 4, 8, 8, 4, white);
        fill(&p, 5, 5, 6, 6, 3, pink);
        fill(&p, 7, 2, 2, 6, 1, white);
        break;
    case ICON_RESTART:
        fill(&p, 2, 2, 12, 12, 6, accent);
        fill(&p, 4, 4, 8, 8, 4, white);
        fill(&p, 5, 5, 6, 6, 3, accent);
        fill(&p, 8, 2, 4, 4, 0, white);
        break;
    case ICON_LOGOUT:
        fill(&p, 2, 2, 8, 12, 1, grey);
        fill(&p, 3, 3, 6, 10, 1, white);
        fill(&p, 7, 7, 7, 2, 0, accent);
        line(&p, 12, 5, 14, 8, accent);
        line(&p, 14, 8, 12, 11, accent);
        break;
    case ICON_NETWORK:
        fill(&p, 6, 1, 4, 4, 1, accent);
        fill(&p, 1, 11, 4, 4, 1, accent);
        fill(&p, 11, 11, 4, 4, 1, accent);
        fill(&p, 7, 5, 2, 4, 0, accent);
        fill(&p, 3, 8, 10, 1, 0, accent);
        fill(&p, 3, 8, 1, 3, 0, accent);
        fill(&p, 12, 8, 1, 3, 0, accent);
        break;
    case ICON_INFO:
        fill(&p, 1, 1, 14, 14, 7, accent);
        fill(&p, 7, 4, 2, 2, 1, white);
        fill(&p, 7, 7, 2, 5, 0, white);
        break;
    case ICON_JELLY:
        /* dome with tentacles */
        fill(&p, 2, 2, 12, 9, 6, violet);
        fill(&p, 2, 7, 12, 3, 0, violet);
        fill(&p, 4, 4, 3, 2, 1, color_mix(violet, white, 120));
        for (int i = 0; i < 4; i++)
            fill(&p, 3 + i * 3, 10, 1, 4 + (i % 2) * 2, 0, color_mix(violet, white, 80));
        break;
    }
}

icon_t icon_by_name(const char *name)
{
    static const struct {
        const char *name;
        icon_t      icon;
    } names[] = {
        { "folder", ICON_FOLDER },     { "file", ICON_FILE },         { "text", ICON_TEXT },
        { "program", ICON_PROGRAM },   { "terminal", ICON_TERMINAL }, { "files", ICON_FILES },
        { "settings", ICON_SETTINGS }, { "power", ICON_POWER },       { "network", ICON_NETWORK },
        { "info", ICON_INFO },         { "jelly", ICON_JELLY },       { "demo", ICON_DEMO },
        { "logout", ICON_LOGOUT },     { "restart", ICON_RESTART },
    };
    for (size_t i = 0; name && i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(names[i].name, name))
            return names[i].icon;
    }
    return ICON_PROGRAM;
}
