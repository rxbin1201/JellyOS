/*
 * Keyboard layouts: physical keys (JELLY_KEY_*) to characters.
 */

#ifndef DISPLAYD_KEYMAP_H
#define DISPLAYD_KEYMAP_H

#include <stdbool.h>
#include <stdint.h>

/* Select "us" or "de"; returns false for unknown names (the layout stays). */
bool     keymap_select(const char *name);
const char *keymap_name(void);
/* The character for a key with WM_MOD_* modifiers, 0 if it produces none. */
uint32_t keymap_translate(uint32_t key, uint32_t modifiers);

#endif
