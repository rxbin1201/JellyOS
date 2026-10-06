/*
 * Kernel modules (README section 23).
 *
 * A module declares, in a module_info_t placed in section ".jelly_module":
 *   - the driver API version it was built against (must equal DRIVER_API_VERSION)
 *   - the minimum kernel version it needs
 *   - its own name and version
 *   - the modules it depends on
 *   - init/exit functions (which register/unregister its drivers)
 * Drivers inside a module declare their device IDs and capabilities.
 *
 * Built-in modules are linked into the kernel. Loadable modules are ELF64
 * relocatable objects (.ko) that the loader validates, relocates against
 * exported kernel and module symbols, maps with W^X rights and initializes;
 * they can be unloaded when nothing depends on them.
 * See docs/architecture/drivers.md.
 */

#ifndef DRIVERS_CORE_MODULE_H
#define DRIVERS_CORE_MODULE_H

#include "core/export.h"
#include "core/list.h"
#include "core/version.h"

#include <jelly/status.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Incremented whenever exported driver interfaces change incompatibly. */
#define DRIVER_API_VERSION 1

#define MODULE_MAGIC       0x444F4D4AU /* "JMOD" */
#define MODULE_NAME_MAX    32
#define MODULE_MAX_DEPS    8

typedef struct {
    uint32_t           magic;
    uint32_t           api_version;
    uint32_t           min_kernel_version; /* KERNEL_VERSION(major, minor, patch) */
    uint32_t           version;
    const char        *name;
    const char        *description;
    const char *const *dependencies;       /* NULL-terminated, may be NULL */
    status_t         (*init)(void);
    void             (*exit)(void);        /* NULL: the module cannot be unloaded */
} module_info_t;

/* Declare the module of this source file (one per module). */
#define MODULE(...)                                                                     \
    static const module_info_t __jelly_module_info                                      \
        __attribute__((used, section(".jelly_module"), aligned(8))) = {                 \
            .magic = MODULE_MAGIC, .api_version = DRIVER_API_VERSION, __VA_ARGS__ }

typedef enum {
    MODULE_LOADED,  /* mapped and relocated, init not yet run */
    MODULE_RUNNING,
} module_state_t;

typedef struct module {
    char                 name[MODULE_NAME_MAX];
    const module_info_t *info;
    module_state_t       state;
    bool                 builtin;

    uint64_t             base;        /* loadable modules: mapped image */
    uint64_t             size;
    const kernel_symbol_t *exports;
    size_t               export_count;

    struct module       *deps[MODULE_MAX_DEPS];
    uint32_t             dep_count;
    uint32_t             users;       /* modules depending on this one */
    list_node_t          node;
} module_t;

/* Initialize all built-in modules in dependency order. */
status_t  module_init_builtin(void);

/* Validate, relocate, map and initialize a loadable module image. */
status_t  module_load(const void *image, size_t size, module_t **module);

/* Run exit and free the module; BUSY while other modules depend on it. */
status_t  module_unload(const char *name);

module_t *module_find(const char *name);

/* Load every boot module whose name ends in ".ko". Returns the number loaded. */
unsigned  module_load_boot_modules(void);

/* Load one boot module by file name (e.g. "edu.ko"). */
status_t  module_load_boot_module(const char *file_name);

/* The module whose init/exit is running (drivers record their owner). */
module_t *module_current(void);

/* Address of an exported kernel or module symbol, or NULL. */
const void *module_resolve(const char *name, module_t **provider);

#endif
