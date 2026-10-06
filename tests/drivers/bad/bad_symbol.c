/*
 * Must be rejected: uses a kernel function that is not exported.
 */

#include "drivers/core/module.h"

void pmm_reclaim_everything_now(void); /* not exported (and not existing) */

static status_t start(void)
{
    pmm_reclaim_everything_now();
    return STATUS_SUCCESS;
}

MODULE(.name = "bad_symbol", .version = 1, .init = start);
