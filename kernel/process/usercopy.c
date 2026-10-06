#include "process/usercopy.h"

#include "process/process.h"

#include "core/arch.h"
#include "core/string.h"
#include "memory/layout.h"

bool user_range_ok(uint64_t address, size_t size, bool write)
{
    process_t *p = process_current();

    if (!p || size == 0 || address < USER_SPACE_START || address >= USER_SPACE_END ||
        size > USER_SPACE_END - address)
        return false;

    uint32_t required = VM_USER | (write ? VM_WRITE : 0);
    for (uint64_t page = align_down(address, PAGE_SIZE); page < address + size; page += PAGE_SIZE) {
        uint64_t phys;
        uint32_t flags;
        if (!vmm_query(&p->space, page, &phys, &flags) || (flags & required) != required)
            return false;
    }
    return true;
}

status_t copy_from_user(void *dest, uint64_t user_src, size_t size)
{
    if (size == 0)
        return STATUS_SUCCESS;
    if (!user_range_ok(user_src, size, false))
        return STATUS_INVALID_ARGUMENT;

    arch_user_access_begin();
    memcpy(dest, (const void *)(uintptr_t)user_src, size);
    arch_user_access_end();
    return STATUS_SUCCESS;
}

status_t copy_to_user(uint64_t user_dest, const void *src, size_t size)
{
    if (size == 0)
        return STATUS_SUCCESS;
    if (!user_range_ok(user_dest, size, true))
        return STATUS_INVALID_ARGUMENT;

    arch_user_access_begin();
    memcpy((void *)(uintptr_t)user_dest, src, size);
    arch_user_access_end();
    return STATUS_SUCCESS;
}
