/*
 * JellyOS Boot Protocol - persistent boot state.
 *
 * Stored in the UEFI variable BOOT_STATE_VARIABLE_NAME under
 * BOOT_STATE_VENDOR_GUID with attributes NV | BS | RT.
 * See docs/architecture/boot-state.md.
 *
 * Kernel / userspace contract:
 *   - Mark a healthy boot:  read the record, set state = BOOT_SUCCESS, write it back.
 *   - Request recovery:     read the record, set state = RECOVERY_REQUESTED, write it back.
 *   All other fields are owned by the boot manager and must be preserved.
 */

#ifndef JELLY_BOOT_STATE_H
#define JELLY_BOOT_STATE_H

#include <stdint.h>

#define BOOT_STATE_VARIABLE_NAME "JellyOSBootState"

/* c64f307e-e85f-4506-aec0-cbdccf291161 */
#define BOOT_STATE_VENDOR_GUID \
    { 0xc64f307e, 0xe85f, 0x4506, { 0xae, 0xc0, 0xcb, 0xdc, 0xcf, 0x29, 0x11, 0x61 } }

#define BOOT_STATE_RECORD_VERSION 1

typedef enum {
    BOOT_STATE_NONE           = 0, /* no record yet */
    BOOT_STARTED              = 1, /* boot manager running */
    BOOT_CONFIGURATION_LOADED = 2, /* configuration parsed (or defaults applied) */
    KERNEL_LOADED             = 3, /* kernel and modules in memory */
    KERNEL_STARTED            = 4, /* control passed to the kernel */
    BOOT_SUCCESS              = 5, /* written by the system once it is healthy */
    BOOT_FAILED               = 6, /* previous tracked boot never reported success */
    RECOVERY_REQUESTED        = 7, /* written by the system: boot recovery next time */
} boot_state_t;

#define BOOT_STATE_FLAG_TRACKED (1u << 0) /* booted kernel reports success */

typedef struct {
    uint32_t version;           /* BOOT_STATE_RECORD_VERSION */
    uint32_t size;              /* sizeof(boot_state_record_t) */
    uint32_t state;             /* boot_state_t of the current or last boot */
    uint32_t mode;              /* BOOT_MODE_* of that boot */
    uint32_t flags;             /* BOOT_STATE_FLAG_* of that boot */
    uint32_t last_result;       /* BOOT_SUCCESS, BOOT_FAILED or BOOT_STATE_NONE */
    uint32_t normal_failures;   /* consecutive failed boots of the current kernel */
    uint32_t fallback_failures; /* consecutive failed boots of the previous kernel */
    uint32_t boot_count;        /* total boots seen by the boot manager */
    uint32_t reserved;
} boot_state_record_t;

#endif
