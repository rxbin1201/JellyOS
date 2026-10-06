/*
 * JellyOS Boot Manager - boot state tracking and rollback decisions
 * (README sections 5 and 6, docs/architecture/boot-state.md).
 */

#ifndef BOOT_BOOT_TRACKER_H
#define BOOT_BOOT_TRACKER_H

#include "boot.h"
#include "config.h"

#include <jelly/boot_state.h>

typedef struct {
    boot_state_record_t record;             /* persisted in the UEFI variable */
    uint32_t            previous_state;     /* state found at startup */
    bool                previous_failed;    /* tracked boot never reported success */
    bool                recovery_requested; /* system asked for recovery */
    bool                available;          /* variable storage works */
} boot_tracker_t;

typedef struct {
    UINTN    entry;     /* index into config->entries */
    uint32_t mode;      /* BOOT_MODE_* */
    bool     show_menu; /* user attention required */
    CHAR16   reason[160];
} boot_decision_t;

/* Read the record, evaluate the previous boot and store BOOT_STARTED. */
void boot_tracker_init(boot_tracker_t *tracker);

/* Choose entry and mode from failure counters and recovery requests. */
void boot_tracker_decide(const boot_tracker_t *tracker, const boot_config_t *config,
                         boot_decision_t *decision);

/* Record which kind of boot is about to happen (stored with the next state). */
void boot_tracker_begin(boot_tracker_t *tracker, uint32_t mode, bool kernel_reports_success);

/* Store a new state. Disables tracking for this boot if the variable cannot be written. */
void boot_tracker_set(boot_tracker_t *tracker, boot_state_t state);

const CHAR16 *boot_state_name(uint32_t state);
const CHAR16 *boot_mode_name(uint32_t mode);

#endif
