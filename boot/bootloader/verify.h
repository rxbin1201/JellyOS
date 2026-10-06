/*
 * JellyOS Boot Manager - image verification hook.
 *
 * Signature verification architecture (README section 3): every image the
 * boot manager hands to the kernel passes through verify_image() before it
 * is used. Signed kernels and modules (Phase 15) plug their policy in here
 * without touching the loading code.
 */

#ifndef BOOT_VERIFY_H
#define BOOT_VERIFY_H

#include "boot.h"

typedef enum {
    VERIFY_KERNEL,
    VERIFY_INITRD,
    VERIFY_MODULE,
} verify_kind_t;

/* Returns EFI_SUCCESS if the image may be used, EFI_SECURITY_VIOLATION otherwise. */
EFI_STATUS verify_image(verify_kind_t kind, const CHAR16 *path, const void *data, UINTN size);

#endif
