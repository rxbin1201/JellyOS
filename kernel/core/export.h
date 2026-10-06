/*
 * Kernel symbols available to loadable modules.
 *
 * Only symbols marked with EXPORT_SYMBOL can be resolved by the module
 * loader. The export table is the kernel's stable driver API boundary
 * (DRIVER_API_VERSION in drivers/core/module.h); modules can export
 * symbols for other modules the same way.
 */

#ifndef CORE_EXPORT_H
#define CORE_EXPORT_H

typedef struct {
    const char *name;
    const void *address;
} kernel_symbol_t;

#define EXPORT_SYMBOL(symbol)                                                          \
    static const kernel_symbol_t __export_##symbol                                     \
        __attribute__((used, section(".kexports"), aligned(8))) = { #symbol, (const void *)&symbol }

#endif
