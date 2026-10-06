#include "drivers/acpi/acpi.h"

#include "core/export.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/vmm.h"

#define MAX_TABLES        64
#define RSDP_V1_LENGTH    20
#define RSDP_V2_LENGTH    36
#define MADT_ENTRIES      44

typedef struct __attribute__((packed)) {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} rsdp_t;

static const acpi_header_t *tables[MAX_TABLES];
static size_t table_count;

static bool checksum_ok(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    uint8_t sum = 0;
    for (size_t i = 0; i < length; i++)
        sum += bytes[i];
    return sum == 0;
}

/* Map a whole table: the header first to learn its length. */
static const acpi_header_t *map_table(uint64_t phys)
{
    const acpi_header_t *header = vmm_phys_to_kernel(phys, sizeof(acpi_header_t));
    if (!header || header->length < sizeof(acpi_header_t) || header->length > (16u << 20))
        return NULL;

    const acpi_header_t *table = vmm_phys_to_kernel(phys, header->length);
    if (!table || !checksum_ok(table, table->length)) {
        klog_warn("acpi: table at 0x%lx has a bad checksum, ignored", phys);
        return NULL;
    }
    return table;
}

status_t acpi_init(uint64_t rsdp_phys)
{
    if (!rsdp_phys) {
        klog_error("acpi: no RSDP from the boot manager");
        return STATUS_NOT_FOUND;
    }

    const rsdp_t *rsdp = vmm_phys_to_kernel(rsdp_phys, RSDP_V2_LENGTH);
    if (!rsdp || memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !checksum_ok(rsdp, RSDP_V1_LENGTH)) {
        klog_error("acpi: invalid RSDP at 0x%lx", rsdp_phys);
        return STATUS_INVALID_ARGUMENT;
    }

    bool xsdt = rsdp->revision >= 2 && rsdp->xsdt_address && checksum_ok(rsdp, RSDP_V2_LENGTH);
    const acpi_header_t *root = map_table(xsdt ? rsdp->xsdt_address : rsdp->rsdt_address);
    if (!root) {
        klog_error("acpi: invalid %s", xsdt ? "XSDT" : "RSDT");
        return STATUS_INVALID_ARGUMENT;
    }

    size_t entry_size = xsdt ? 8 : 4;
    size_t entries = (root->length - sizeof(acpi_header_t)) / entry_size;
    const uint8_t *list = (const uint8_t *)(root + 1);

    for (size_t i = 0; i < entries && table_count < MAX_TABLES; i++) {
        uint64_t phys = 0;
        memcpy(&phys, list + i * entry_size, entry_size);
        const acpi_header_t *table = map_table(phys);
        if (table)
            tables[table_count++] = table;
    }

    char names[MAX_TABLES * 5 + 1];
    size_t n = 0;
    for (size_t i = 0; i < table_count; i++) {
        memcpy(names + n, tables[i]->signature, 4);
        names[n + 4] = ' ';
        n += 5;
    }
    names[n ? n - 1 : 0] = '\0';
    klog_info("acpi: revision %u, %s with %zu tables: %s", rsdp->revision, xsdt ? "XSDT" : "RSDT", table_count,
              names);
    return STATUS_SUCCESS;
}

size_t acpi_table_count(void)
{
    return table_count;
}

const acpi_header_t *acpi_table_at(size_t index)
{
    return index < table_count ? tables[index] : NULL;
}

const acpi_header_t *acpi_find_table(const char *signature, size_t index)
{
    for (size_t i = 0; i < table_count; i++) {
        if (memcmp(tables[i]->signature, signature, 4) == 0 && index-- == 0)
            return tables[i];
    }
    return NULL;
}

status_t acpi_parse_madt(acpi_madt_info_t *info)
{
    const acpi_header_t *madt = acpi_find_table("APIC", 0);
    if (!madt)
        return STATUS_NOT_FOUND;

    const uint8_t *bytes = (const uint8_t *)madt;
    uint32_t lapic, flags;
    memcpy(&lapic, bytes + 36, 4);
    memcpy(&flags, bytes + 40, 4);

    memset(info, 0, sizeof(*info));
    info->lapic_address = lapic;
    info->legacy_pics = flags & 1;

    for (uint32_t offset = MADT_ENTRIES; offset + 2 <= madt->length;) {
        uint8_t type = bytes[offset], length = bytes[offset + 1];
        const uint8_t *e = bytes + offset;
        if (length < 2 || offset + length > madt->length)
            break;

        switch (type) {
        case 0: /* processor local APIC */
        case 9: /* processor local x2APIC */ {
            uint32_t lapic_flags;
            memcpy(&lapic_flags, e + (type == 0 ? 4 : 8), 4);
            if (lapic_flags & 3) /* enabled or online capable */
                info->cpu_count++;
            break;
        }
        case 1: /* I/O APIC */
            if (info->ioapic_count < ACPI_MAX_IOAPICS) {
                acpi_ioapic_t *io = &info->ioapics[info->ioapic_count++];
                uint32_t address;
                io->id = e[2];
                memcpy(&address, e + 4, 4);
                memcpy(&io->gsi_base, e + 8, 4);
                io->address = address;
            }
            break;
        case 2: /* interrupt source override */
            if (info->override_count < ACPI_MAX_OVERRIDES) {
                acpi_override_t *o = &info->overrides[info->override_count++];
                o->source_irq = e[3];
                memcpy(&o->gsi, e + 4, 4);
                memcpy(&o->flags, e + 8, 2);
            }
            break;
        case 5: /* local APIC address override */
            memcpy(&info->lapic_address, e + 4, 8);
            break;
        }
        offset += length;
    }
    return STATUS_SUCCESS;
}

size_t acpi_parse_mcfg(acpi_mcfg_entry_t *entries, size_t max)
{
    const acpi_header_t *mcfg = acpi_find_table("MCFG", 0);
    if (!mcfg)
        return 0;

    const uint8_t *bytes = (const uint8_t *)mcfg;
    size_t count = 0;
    for (uint32_t offset = 44; offset + 16 <= mcfg->length && count < max; offset += 16) {
        acpi_mcfg_entry_t *e = &entries[count++];
        memcpy(&e->base, bytes + offset, 8);
        memcpy(&e->segment, bytes + offset + 8, 2);
        e->start_bus = bytes[offset + 10];
        e->end_bus = bytes[offset + 11];
    }
    return count;
}

EXPORT_SYMBOL(acpi_find_table);
