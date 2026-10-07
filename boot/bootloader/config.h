/*
 * JellyOS Boot Manager - boot configuration (README section 4).
 *
 * Loaded from CONFIG_PATH on the boot volume. A missing or broken file never
 * prevents booting: invalid lines are skipped with a warning, and built-in
 * defaults are used when no usable entry remains.
 */

#ifndef BOOT_CONFIG_H
#define BOOT_CONFIG_H

#include "boot.h"

#define CONFIG_PATH          L"\\boot\\boot.cfg"

#define CONFIG_MAX_ENTRIES   16
#define CONFIG_MAX_MODULES   8
#define CONFIG_NAME_MAX      64
#define CONFIG_PATH_MAX      256
#define CONFIG_CMDLINE_MAX   1024

#define CONFIG_NO_ENTRY      ((UINTN)-1)

typedef enum {
    MENU_AUTO,   /* countdown prompt, menu on key press or after a failure */
    MENU_ALWAYS, /* always show the menu, countdown until a key is pressed */
    MENU_HIDDEN, /* boot immediately unless a failure requires the menu */
} menu_mode_t;

typedef struct {
    char  name[CONFIG_NAME_MAX];
    char  kernel[CONFIG_PATH_MAX];
    char  initrd[CONFIG_PATH_MAX];                      /* empty: none */
    char  modules[CONFIG_MAX_MODULES][CONFIG_PATH_MAX]; /* early modules */
    UINTN module_count;
    char  cmdline[CONFIG_CMDLINE_MAX];
} boot_entry_t;

typedef struct {
    UINTN        timeout;                         /* seconds */
    menu_mode_t  menu;
    UINTN        max_attempts;                    /* failed boots before rollback */
    char         resolution[16];                  /* "max", "keep" or "WIDTHxHEIGHT" */
    char         fallback_kernel[CONFIG_PATH_MAX]; /* previous known-good kernel, empty: none */
    UINTN        default_index;
    UINTN        recovery_index;                  /* CONFIG_NO_ENTRY if none */
    bool         from_file;                       /* false: built-in defaults */
    boot_entry_t entries[CONFIG_MAX_ENTRIES];
    UINTN        entry_count;
} boot_config_t;

/* Always produces a usable configuration. */
void config_load(EFI_HANDLE image, boot_config_t *config);

#endif
