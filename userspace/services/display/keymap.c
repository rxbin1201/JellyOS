/*
 * Keyboard layouts: US (QWERTY) and German (QWERTZ, with AltGr).
 *
 * Each layout lists, per row of the keyboard, the characters of consecutive
 * key codes as UTF-8 strings for the normal, shifted and AltGr levels. Caps
 * Lock affects letters only. With Ctrl, letters produce control characters
 * (Ctrl-C = 3, Ctrl-D = 4), as terminals expect.
 */

#include "keymap.h"

#include <string.h>

#include "graphics/core/canvas.h"
#include "graphics/window/protocol.h"

#include <jelly/input.h>

typedef struct {
    uint32_t    first_key;
    const char *normal, *shift, *altgr;
} row_t;

typedef struct {
    const char  *name;
    const row_t *rows;
} layout_t;

/* A space in an AltGr row means "nothing". */
static const row_t us_rows[] = {
    { JELLY_KEY_1, "1234567890-=", "!@#$%^&*()_+", NULL },
    { JELLY_KEY_Q, "qwertyuiop[]", "QWERTYUIOP{}", NULL },
    { JELLY_KEY_A, "asdfghjkl;'`", "ASDFGHJKL:\"~", NULL },
    { JELLY_KEY_BACKSLASH, "\\zxcvbnm,./", "|ZXCVBNM<>?", NULL },
    { JELLY_KEY_102ND, "\\", "|", NULL },
    { 0, NULL, NULL, NULL },
};

static const row_t de_rows[] = {
    { JELLY_KEY_1, "1234567890ß´", "!\"§$%&/()=?`", " ²³   {[]}\\ " },
    { JELLY_KEY_Q, "qwertzuiopü+", "QWERTZUIOPÜ*", "@ €        ~" },
    { JELLY_KEY_A, "asdfghjklöä^", "ASDFGHJKLÖÄ°", NULL },
    { JELLY_KEY_BACKSLASH, "#yxcvbnm,.-", "'YXCVBNM;:_", "       µ   " },
    { JELLY_KEY_102ND, "<", ">", "|" },
    { 0, NULL, NULL, NULL },
};

static const layout_t layouts[] = {
    { "us", us_rows },
    { "de", de_rows },
};

static const layout_t *current = &layouts[0];

bool keymap_select(const char *name)
{
    for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
        if (!strcmp(layouts[i].name, name)) {
            current = &layouts[i];
            return true;
        }
    }
    return false;
}

const char *keymap_name(void)
{
    return current->name;
}

/* The n-th code point of a UTF-8 string, 0 if it is shorter. */
static uint32_t nth(const char *text, uint32_t n)
{
    if (!text)
        return 0;
    for (uint32_t i = 0; *text; i++) {
        uint32_t c = utf8_next(&text);
        if (i == n)
            return c == ' ' ? 0 : c;
    }
    return 0;
}

static bool is_letter(uint32_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) ||
           (c >= 0xC0 && c <= 0xDE && c != 0xD7);
}

static uint32_t flip_case(uint32_t c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE))
        return c - 0x20;
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE))
        return c + 0x20;
    return c;
}

uint32_t keymap_translate(uint32_t key, uint32_t modifiers)
{
    switch (key) {
    case JELLY_KEY_ENTER:
    case JELLY_KEY_KPENTER:
        return '\n';
    case JELLY_KEY_BACKSPACE:
        return '\b';
    case JELLY_KEY_TAB:
        return '\t';
    case JELLY_KEY_ESCAPE:
        return 0x1B;
    case JELLY_KEY_SPACE:
        return ' ';
    }

    for (const row_t *row = current->rows; row->normal; row++) {
        uint32_t length = (uint32_t)text_length(row->normal, (int32_t)strlen(row->normal));
        if (key < row->first_key || key >= row->first_key + length)
            continue;
        uint32_t index = key - row->first_key;
        uint32_t base = nth(row->normal, index);

        if (modifiers & WM_MOD_ALTGR)
            return nth(row->altgr, index);
        if ((modifiers & WM_MOD_CTRL) && base >= 'a' && base <= 'z')
            return base - 'a' + 1;
        bool shifted = (modifiers & WM_MOD_SHIFT) != 0;
        uint32_t c = shifted ? nth(row->shift, index) : base;
        if ((modifiers & WM_MOD_CAPS) && is_letter(base))
            c = flip_case(c);
        return c;
    }
    return 0;
}
