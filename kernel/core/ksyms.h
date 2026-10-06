/*
 * Kernel symbol table for stack traces (README section 50).
 *
 * The table is generated at build time by tools/debugger/ksyms.py from a
 * first link of the kernel and linked into the final image (two-pass link).
 */

#ifndef CORE_KSYMS_H
#define CORE_KSYMS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t    address;
    const char *name;
} ksym_t;

extern const ksym_t ksym_table[];
extern const size_t ksym_count;

/* Find the function containing address. Returns NULL if unknown. */
const char *ksym_lookup(uint64_t address, uint64_t *offset);

#endif
