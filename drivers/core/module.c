#include "drivers/core/module.h"

#include "drivers/core/device.h"

#include "core/arch.h"
#include "core/boot.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "memory/layout.h"
#include "memory/vmm.h"

/* --- ELF64 relocatable objects ----------------------------------------------------- */

#define ET_REL          1
#define EM_X86_64       62
#define SHT_PROGBITS    1
#define SHT_SYMTAB      2
#define SHT_RELA        4
#define SHT_NOBITS      8
#define SHT_REL         9
#define SHF_WRITE       0x1
#define SHF_ALLOC       0x2
#define SHF_EXECINSTR   0x4
#define SHN_UNDEF       0
#define SHN_LORESERVE   0xFF00
#define SHN_ABS         0xFFF1
#define R_X86_64_NONE   0
#define R_X86_64_64     1
#define R_X86_64_PC32   2
#define R_X86_64_PLT32  4
#define R_X86_64_32     10
#define R_X86_64_32S    11
#define R_X86_64_PC64   24

#define MAX_SECTIONS    256

typedef struct {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf_header_t;

typedef struct {
    uint32_t name, type;
    uint64_t flags, addr, offset, size;
    uint32_t link, info;
    uint64_t addralign, entsize;
} elf_section_t;

typedef struct {
    uint32_t name;
    uint8_t  info, other;
    uint16_t shndx;
    uint64_t value, size;
} elf_symbol_t;

typedef struct {
    uint64_t offset;
    uint64_t info;
    int64_t  addend;
} elf_rela_t;

enum { GROUP_TEXT, GROUP_RODATA, GROUP_DATA, GROUP_COUNT };

typedef struct {
    const uint8_t       *file;
    size_t               size;
    const elf_header_t  *header;
    const elf_section_t *sections;
    const char          *section_names;
    uint64_t             address[MAX_SECTIONS]; /* where each SHF_ALLOC section was placed */
    uint64_t             group_start[GROUP_COUNT];
    uint64_t             group_size[GROUP_COUNT];
    module_t            *module;
} loader_t;

/* --- Registry ---------------------------------------------------------------------- */

extern const kernel_symbol_t __kexports_start[], __kexports_end[];
extern const module_info_t __builtin_modules_start[], __builtin_modules_end[];

static list_t modules = { { &modules.head, &modules.head } };
static module_t *current;
static uint64_t next_address = MODULE_REGION;

module_t *module_current(void)
{
    return current;
}

module_t *module_find(const char *name)
{
    list_for_each(node, &modules) {
        module_t *m = container_of(node, module_t, node);
        if (strcmp(m->name, name) == 0)
            return m;
    }
    return NULL;
}

const void *module_resolve(const char *name, module_t **provider)
{
    *provider = NULL;
    for (const kernel_symbol_t *s = __kexports_start; s < __kexports_end; s++) {
        if (strcmp(s->name, name) == 0)
            return s->address;
    }
    list_for_each(node, &modules) {
        module_t *m = container_of(node, module_t, node);
        if (m->state != MODULE_RUNNING)
            continue;
        for (size_t i = 0; i < m->export_count; i++) {
            if (strcmp(m->exports[i].name, name) == 0) {
                *provider = m;
                return m->exports[i].address;
            }
        }
    }
    return NULL;
}

static status_t add_dependency(module_t *m, module_t *provider)
{
    if (!provider || provider == m)
        return STATUS_SUCCESS;
    for (uint32_t i = 0; i < m->dep_count; i++) {
        if (m->deps[i] == provider)
            return STATUS_SUCCESS;
    }
    if (m->dep_count == MODULE_MAX_DEPS)
        return STATUS_LIMIT_EXCEEDED;
    m->deps[m->dep_count++] = provider;
    return STATUS_SUCCESS;
}

/* Check identity, versions and declared dependencies. */
static status_t check_info(module_t *m, const module_info_t *info)
{
    if (info->magic != MODULE_MAGIC || !info->name)
        return STATUS_INVALID_ARGUMENT;
    if (strlen(info->name) >= MODULE_NAME_MAX)
        return STATUS_INVALID_ARGUMENT;
    memcpy(m->name, info->name, strlen(info->name) + 1);

    if (info->api_version != DRIVER_API_VERSION) {
        klog_error("module %s: built for driver API %u, kernel provides %u", m->name, info->api_version,
                   DRIVER_API_VERSION);
        return STATUS_NOT_SUPPORTED;
    }
    if (info->min_kernel_version > KERNEL_VERSION_CODE) {
        klog_error("module %s: needs kernel %u.%u.%u, this is " KERNEL_VERSION_STRING, m->name,
                   info->min_kernel_version >> 16, (info->min_kernel_version >> 8) & 0xFF,
                   info->min_kernel_version & 0xFF);
        return STATUS_NOT_SUPPORTED;
    }
    if (module_find(m->name)) {
        klog_error("module %s: already loaded", m->name);
        return STATUS_BUSY;
    }
    for (const char *const *dep = info->dependencies; dep && *dep; dep++) {
        module_t *provider = module_find(*dep);
        if (!provider || provider->state != MODULE_RUNNING) {
            klog_error("module %s: missing dependency '%s'", m->name, *dep);
            return STATUS_NOT_FOUND;
        }
        status_t status = add_dependency(m, provider);
        if (STATUS_IS_ERROR(status))
            return status;
    }
    m->info = info;
    return STATUS_SUCCESS;
}

/* Run init; on success the module counts as a user of its dependencies. */
static status_t start(module_t *m)
{
    status_t status = STATUS_SUCCESS;

    current = m;
    if (m->info->init)
        status = m->info->init();
    current = NULL;

    if (STATUS_IS_ERROR(status)) {
        klog_error("module %s: init failed: %s", m->name, status_name(status));
        return status;
    }
    for (uint32_t i = 0; i < m->dep_count; i++)
        m->deps[i]->users++;
    m->state = MODULE_RUNNING;
    return STATUS_SUCCESS;
}

/* --- Built-in modules ----------------------------------------------------------------- */

status_t module_init_builtin(void)
{
    size_t count = (size_t)(__builtin_modules_end - __builtin_modules_start);
    module_t *pending[32];
    size_t pending_count = 0;

    for (size_t i = 0; i < count && pending_count < 32; i++) {
        module_t *m = kcalloc(1, sizeof(*m));
        if (!m)
            return STATUS_OUT_OF_MEMORY;
        m->builtin = true;
        m->info = &__builtin_modules_start[i];
        memcpy(m->name, m->info->name, strlen(m->info->name) + 1);
        pending[pending_count++] = m;
    }

    /* Initialize in dependency order: repeat until no module can start. */
    bool progress = true;
    while (pending_count && progress) {
        progress = false;
        for (size_t i = 0; i < pending_count; i++) {
            module_t *m = pending[i];
            const module_info_t *info = m->info;
            bool ready = true;
            for (const char *const *dep = info->dependencies; dep && *dep; dep++) {
                module_t *provider = module_find(*dep);
                ready &= provider && provider->state == MODULE_RUNNING;
            }
            if (!ready)
                continue;

            m->info = NULL;
            if (check_info(m, info) == STATUS_SUCCESS) {
                list_push_back(&modules, &m->node);
                if (STATUS_IS_ERROR(start(m))) {
                    list_remove(&m->node);
                    kfree(m);
                }
            } else {
                kfree(m);
            }
            pending[i] = pending[--pending_count];
            progress = true;
            break;
        }
    }
    for (size_t i = 0; i < pending_count; i++) {
        klog_error("module %s: dependencies never became available", pending[i]->name);
        kfree(pending[i]);
    }
    klog_info("modules: %zu built-in", count);
    return STATUS_SUCCESS;
}

/* --- Loader ------------------------------------------------------------------------------ */

static const elf_section_t *section(const loader_t *l, unsigned index)
{
    return &l->sections[index];
}

static const char *section_name(const loader_t *l, const elf_section_t *s)
{
    const elf_section_t *names = section(l, l->header->shstrndx);
    return s->name < names->size ? l->section_names + s->name : "";
}

static bool in_file(const loader_t *l, uint64_t offset, uint64_t length)
{
    return offset <= l->size && length <= l->size - offset;
}

static status_t validate(loader_t *l)
{
    const elf_header_t *eh = l->header;

    if (l->size < sizeof(*eh) || memcmp(eh->ident, "\x7F" "ELF", 4) != 0 || eh->ident[4] != 2 ||
        eh->ident[5] != 1 || eh->type != ET_REL || eh->machine != EM_X86_64 ||
        eh->shentsize != sizeof(elf_section_t) || eh->shnum == 0 || eh->shnum > MAX_SECTIONS ||
        eh->shstrndx >= eh->shnum || !in_file(l, eh->shoff, (uint64_t)eh->shnum * sizeof(elf_section_t)))
        return STATUS_INVALID_ARGUMENT;

    l->sections = (const elf_section_t *)(l->file + eh->shoff);
    for (unsigned i = 0; i < eh->shnum; i++) {
        const elf_section_t *s = section(l, i);
        if (s->type != SHT_NOBITS && !in_file(l, s->offset, s->size))
            return STATUS_INVALID_ARGUMENT;
    }
    l->section_names = (const char *)(l->file + section(l, eh->shstrndx)->offset);
    return STATUS_SUCCESS;
}

static int group_of(const elf_section_t *s)
{
    if (s->flags & SHF_EXECINSTR)
        return GROUP_TEXT;
    return (s->flags & SHF_WRITE) ? GROUP_DATA : GROUP_RODATA;
}

/* Place every SHF_ALLOC section, grouped by rights, and copy its contents. */
static status_t layout_and_copy(loader_t *l)
{
    uint64_t offset[MAX_SECTIONS] = { 0 };

    for (unsigned i = 0; i < l->header->shnum; i++) {
        const elf_section_t *s = section(l, i);
        if (!(s->flags & SHF_ALLOC) || s->size == 0)
            continue;
        uint64_t align = s->addralign ? s->addralign : 1;
        if (align & (align - 1) || align > PAGE_SIZE)
            return STATUS_INVALID_ARGUMENT;
        int g = group_of(s);
        offset[i] = align_up(l->group_size[g], align);
        l->group_size[g] = offset[i] + s->size;
    }

    uint64_t total = 0;
    for (int g = 0; g < GROUP_COUNT; g++) {
        l->group_start[g] = total;
        total += align_up(l->group_size[g], PAGE_SIZE);
    }
    if (total == 0)
        return STATUS_INVALID_ARGUMENT;

    uint64_t flags = arch_interrupts_save();
    uint64_t base = next_address;
    if (base + total > MODULE_REGION + MODULE_REGION_SIZE) {
        arch_interrupts_restore(flags);
        return STATUS_OUT_OF_MEMORY;
    }
    next_address += total + PAGE_SIZE; /* unmapped gap between modules */
    arch_interrupts_restore(flags);

    status_t status = vmm_alloc(vmm_kernel_space(), base, total, VM_WRITE | VM_GLOBAL);
    if (STATUS_IS_ERROR(status))
        return status;
    l->module->base = base;
    l->module->size = total;

    for (unsigned i = 0; i < l->header->shnum; i++) {
        const elf_section_t *s = section(l, i);
        if (!(s->flags & SHF_ALLOC) || s->size == 0)
            continue;
        l->address[i] = base + l->group_start[group_of(s)] + offset[i];
        if (s->type != SHT_NOBITS)
            memcpy((void *)(uintptr_t)l->address[i], l->file + s->offset, s->size);
    }
    return STATUS_SUCCESS;
}

static status_t symbol_value(loader_t *l, const elf_section_t *symtab, uint32_t index, uint64_t *value)
{
    const elf_symbol_t *symbols = (const elf_symbol_t *)(l->file + symtab->offset);
    const elf_section_t *strtab = section(l, symtab->link);

    if (index >= symtab->size / sizeof(elf_symbol_t))
        return STATUS_INVALID_ARGUMENT;
    const elf_symbol_t *sym = &symbols[index];

    if (sym->shndx == SHN_UNDEF) {
        if (sym->name >= strtab->size)
            return STATUS_INVALID_ARGUMENT;
        const char *name = (const char *)(l->file + strtab->offset + sym->name);
        module_t *provider;
        const void *address = module_resolve(name, &provider);
        if (!address) {
            klog_error("module: unresolved symbol '%s'", name);
            return STATUS_NOT_FOUND;
        }
        *value = (uint64_t)(uintptr_t)address;
        return add_dependency(l->module, provider);
    }
    if (sym->shndx == SHN_ABS) {
        *value = sym->value;
        return STATUS_SUCCESS;
    }
    if (sym->shndx >= SHN_LORESERVE || sym->shndx >= l->header->shnum || !l->address[sym->shndx])
        return STATUS_INVALID_ARGUMENT; /* COMMON or a section that is not loaded */
    *value = l->address[sym->shndx] + sym->value;
    return STATUS_SUCCESS;
}

static status_t apply(uint32_t type, uint64_t place, uint64_t value)
{
    int64_t relative = (int64_t)(value - place);

    switch (type) {
    case R_X86_64_NONE:
        return STATUS_SUCCESS;
    case R_X86_64_64:
        *(uint64_t *)(uintptr_t)place = value;
        return STATUS_SUCCESS;
    case R_X86_64_PC64:
        *(uint64_t *)(uintptr_t)place = (uint64_t)relative;
        return STATUS_SUCCESS;
    case R_X86_64_PC32:
    case R_X86_64_PLT32:
        if (relative != (int32_t)relative)
            return STATUS_INVALID_ARGUMENT;
        *(int32_t *)(uintptr_t)place = (int32_t)relative;
        return STATUS_SUCCESS;
    case R_X86_64_32:
        if (value != (uint32_t)value)
            return STATUS_INVALID_ARGUMENT;
        *(uint32_t *)(uintptr_t)place = (uint32_t)value;
        return STATUS_SUCCESS;
    case R_X86_64_32S:
        if ((int64_t)value != (int32_t)value)
            return STATUS_INVALID_ARGUMENT;
        *(int32_t *)(uintptr_t)place = (int32_t)value;
        return STATUS_SUCCESS;
    }
    klog_error("module: unsupported relocation type %u", type);
    return STATUS_NOT_SUPPORTED;
}

static uint64_t relocation_width(uint32_t type)
{
    return (type == R_X86_64_64 || type == R_X86_64_PC64) ? 8 : 4;
}

static status_t relocate(loader_t *l)
{
    for (unsigned i = 0; i < l->header->shnum; i++) {
        const elf_section_t *rela = section(l, i);
        if (rela->type == SHT_REL)
            return STATUS_NOT_SUPPORTED;
        if (rela->type != SHT_RELA || rela->info >= l->header->shnum)
            continue;
        const elf_section_t *target = section(l, rela->info);
        if (!(target->flags & SHF_ALLOC) || !l->address[rela->info])
            continue; /* debug information */
        if (rela->link >= l->header->shnum || section(l, rela->link)->type != SHT_SYMTAB ||
            rela->entsize != sizeof(elf_rela_t))
            return STATUS_INVALID_ARGUMENT;

        const elf_section_t *symtab = section(l, rela->link);
        const elf_rela_t *entries = (const elf_rela_t *)(l->file + rela->offset);
        for (uint64_t j = 0; j < rela->size / sizeof(elf_rela_t); j++) {
            const elf_rela_t *r = &entries[j];
            uint32_t type = (uint32_t)r->info;
            uint64_t value;

            if (r->offset > target->size || relocation_width(type) > target->size - r->offset)
                return STATUS_INVALID_ARGUMENT;
            status_t status = symbol_value(l, symtab, (uint32_t)(r->info >> 32), &value);
            if (!STATUS_IS_ERROR(status))
                status = apply(type, l->address[rela->info] + r->offset, value + (uint64_t)r->addend);
            if (STATUS_IS_ERROR(status))
                return status;
        }
    }
    return STATUS_SUCCESS;
}

static const void *find_section(loader_t *l, const char *name, uint64_t *size)
{
    for (unsigned i = 0; i < l->header->shnum; i++) {
        const elf_section_t *s = section(l, i);
        if (l->address[i] && strcmp(section_name(l, s), name) == 0) {
            *size = s->size;
            return (const void *)(uintptr_t)l->address[i];
        }
    }
    return NULL;
}

/* Final rights: text R-X, rodata R--, data RW-. */
static status_t protect(loader_t *l)
{
    static const uint32_t rights[GROUP_COUNT] = { VM_EXEC | VM_GLOBAL, VM_GLOBAL, VM_WRITE | VM_GLOBAL };

    for (int g = 0; g < GROUP_COUNT; g++) {
        if (!l->group_size[g])
            continue;
        status_t status = vmm_protect(vmm_kernel_space(), l->module->base + l->group_start[g],
                                      align_up(l->group_size[g], PAGE_SIZE), rights[g]);
        if (STATUS_IS_ERROR(status))
            return status;
    }
    return STATUS_SUCCESS;
}

static status_t load(loader_t *l)
{
    uint64_t size;
    status_t status = validate(l);

    if (!STATUS_IS_ERROR(status))
        status = layout_and_copy(l);
    if (!STATUS_IS_ERROR(status))
        status = relocate(l);
    if (STATUS_IS_ERROR(status))
        return status;

    const module_info_t *info = find_section(l, ".jelly_module", &size);
    if (!info || size != sizeof(module_info_t)) {
        klog_error("module: no module information (.jelly_module)");
        return STATUS_INVALID_ARGUMENT;
    }
    l->module->exports = find_section(l, ".kexports", &size);
    l->module->export_count = l->module->exports ? size / sizeof(kernel_symbol_t) : 0;

    status = check_info(l->module, info);
    if (!STATUS_IS_ERROR(status))
        status = protect(l);
    return status;
}

status_t module_load(const void *image, size_t size, module_t **out)
{
    module_t *m = kcalloc(1, sizeof(*m));
    loader_t *l = kcalloc(1, sizeof(*l));
    status_t status = STATUS_OUT_OF_MEMORY;

    if (m && l) {
        l->file = image;
        l->size = size;
        l->header = image;
        l->module = m;
        status = load(l);
    }
    kfree(l);

    if (!STATUS_IS_ERROR(status)) {
        list_push_back(&modules, &m->node);
        status = start(m);
        if (STATUS_IS_ERROR(status) && driver_count_for_module(m)) {
            klog_error("module %s: failed init left drivers registered, keeping it mapped", m->name);
            return status;
        }
        if (STATUS_IS_ERROR(status))
            list_remove(&m->node);
    }

    if (STATUS_IS_ERROR(status)) {
        if (m && m->size)
            vmm_free(vmm_kernel_space(), m->base, m->size);
        kfree(m);
        return status;
    }

    klog_info("module %s v%u loaded at %p (%lu KiB, %zu exports, %u dependencies)", m->name, m->info->version,
              (void *)m->base, m->size / 1024, m->export_count, m->dep_count);
    if (out)
        *out = m;
    return STATUS_SUCCESS;
}

status_t module_unload(const char *name)
{
    module_t *m = module_find(name);

    if (!m)
        return STATUS_NOT_FOUND;
    if (m->builtin || !m->info->exit)
        return STATUS_NOT_SUPPORTED;
    if (m->users) {
        klog_warn("module %s: still used by %u module(s)", m->name, m->users);
        return STATUS_BUSY;
    }

    current = m;
    m->info->exit();
    current = NULL;

    if (driver_count_for_module(m)) {
        klog_error("module %s: exit left drivers registered, not unloading", m->name);
        return STATUS_BUSY;
    }

    for (uint32_t i = 0; i < m->dep_count; i++)
        m->deps[i]->users--;
    list_remove(&m->node);
    vmm_free(vmm_kernel_space(), m->base, m->size);
    klog_info("module %s unloaded", m->name);
    kfree(m);
    return STATUS_SUCCESS;
}

/* --- Boot modules ----------------------------------------------------------------------- */

static bool is_module_file(const char *name)
{
    size_t n = strlen(name);
    return n > 3 && strcmp(name + n - 3, ".ko") == 0;
}

status_t module_load_boot_module(const char *file_name)
{
    size_t count;
    const boot_module_t *list = boot_modules(&count);

    for (size_t i = 0; i < count; i++) {
        if (strcmp(list[i].name, file_name) == 0)
            return module_load(phys_to_virt(list[i].phys_base), list[i].size, NULL);
    }
    return STATUS_NOT_FOUND;
}

unsigned module_load_boot_modules(void)
{
    size_t count;
    const boot_module_t *list = boot_modules(&count);
    unsigned loaded = 0;

    for (size_t i = 0; i < count; i++) {
        if (!is_module_file(list[i].name))
            continue;
        status_t status = module_load(phys_to_virt(list[i].phys_base), list[i].size, NULL);
        if (STATUS_IS_ERROR(status))
            klog_error("module file %s rejected: %s", list[i].name, status_name(status));
        else
            loaded++;
    }
    return loaded;
}
