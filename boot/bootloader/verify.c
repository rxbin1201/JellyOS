#include "verify.h"

#include "firmware.h"
#include "log.h"

EFI_STATUS verify_image(verify_kind_t kind, const CHAR16 *path, const void *data, UINTN size)
{
    static bool warned;

    (void)kind;
    (void)data;
    (void)size;

    /* No signature policy exists yet: every image is accepted. */
    if (!warned && firmware_secure_boot_enabled()) {
        log_warn(L"Secure Boot is enabled, but JellyOS images are not signature-checked yet");
        warned = true;
    }
    log_debug(L"Verified %s (no signature policy)", path);
    return EFI_SUCCESS;
}
