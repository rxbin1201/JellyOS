/*
 * Must be rejected: depends on a module that does not exist.
 */

#include "drivers/core/module.h"

static status_t start(void)
{
    return STATUS_SUCCESS;
}

static const char *const dependencies[] = { "does_not_exist", NULL };

MODULE(.name = "bad_dependency", .version = 1, .dependencies = dependencies, .init = start);
