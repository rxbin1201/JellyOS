/*
 * Must be rejected: built against a driver API version the kernel does not provide.
 */

#include "drivers/core/module.h"

static status_t start(void)
{
    return STATUS_SUCCESS;
}

static const module_info_t __jelly_module_info __attribute__((used, section(".jelly_module"), aligned(8))) = {
    .magic = MODULE_MAGIC,
    .api_version = DRIVER_API_VERSION + 99,
    .name = "bad_api",
    .version = 1,
    .init = start,
};
