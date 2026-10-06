#include "core/ksyms.h"

extern char __text_end[];

const char *ksym_lookup(uint64_t address, uint64_t *offset)
{
    size_t lo = 0, hi = ksym_count;

    if (ksym_count == 0 || address < ksym_table[0].address || address >= (uint64_t)(uintptr_t)__text_end)
        return NULL;

    /* Last symbol with address <= target (table is sorted). */
    while (hi - lo > 1) {
        size_t mid = (lo + hi) / 2;
        if (ksym_table[mid].address <= address)
            lo = mid;
        else
            hi = mid;
    }
    *offset = address - ksym_table[lo].address;
    return ksym_table[lo].name;
}
