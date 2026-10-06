/*
 * JellyOS Boot Manager - boot menu and countdown prompt.
 */

#ifndef BOOT_MENU_H
#define BOOT_MENU_H

#include "boot.h"
#include "boot_tracker.h"
#include "config.h"

typedef enum {
    ACTION_BOOT,
    ACTION_REBOOT,
    ACTION_EXIT,
} menu_action_kind_t;

typedef struct {
    menu_action_kind_t kind;
    UINTN              entry; /* ACTION_BOOT: index into config->entries */
    uint32_t           mode;  /* ACTION_BOOT: BOOT_MODE_* */
    bool               chosen_by_user;
} menu_action_t;

/*
 * Countdown for menu=auto ("Booting X in N s, press any key").
 * Returns true if a key was pressed and the menu should be shown.
 */
bool menu_prompt(const boot_config_t *config, const boot_decision_t *decision);

/*
 * Interactive menu. countdown > 0 boots the preselected item automatically
 * unless a key is pressed; countdown == 0 waits for the user.
 */
menu_action_t menu_run(EFI_HANDLE image, const boot_config_t *config, const boot_tracker_t *tracker,
                       const boot_decision_t *decision, UINTN countdown);

#endif
