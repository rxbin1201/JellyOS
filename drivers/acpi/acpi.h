/*
 * ACPI static tables (RSDP, RSDT/XSDT, MADT, MCFG).
 *
 * Only table discovery and parsing: interrupt controllers for the IOAPIC
 * and ECAM ranges for PCI. AML (DSDT/SSDT, e.g. legacy PCI interrupt
 * routing via _PRT) needs an interpreter and comes later.
 */

#ifndef DRIVERS_ACPI_ACPI_H
#define DRIVERS_ACPI_ACPI_H

#include <jelly/status.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} acpi_header_t;

#define ACPI_MAX_IOAPICS   8
#define ACPI_MAX_OVERRIDES 24

/* Interrupt source override polarity/trigger (MPS INTI flags) */
#define ACPI_POLARITY_MASK   0x3
#define ACPI_POLARITY_LOW    0x3
#define ACPI_TRIGGER_MASK    0xC
#define ACPI_TRIGGER_LEVEL   0xC

typedef struct {
    uint8_t  id;
    uint64_t address;
    uint32_t gsi_base;
} acpi_ioapic_t;

typedef struct {
    uint8_t  source_irq; /* ISA IRQ */
    uint32_t gsi;
    uint16_t flags;      /* ACPI_POLARITY_*, ACPI_TRIGGER_* */
} acpi_override_t;

typedef struct {
    uint64_t        lapic_address;
    bool            legacy_pics;   /* dual 8259 present (PCAT_COMPAT) */
    uint32_t        cpu_count;
    uint32_t        ioapic_count;
    acpi_ioapic_t   ioapics[ACPI_MAX_IOAPICS];
    uint32_t        override_count;
    acpi_override_t overrides[ACPI_MAX_OVERRIDES];
} acpi_madt_info_t;

typedef struct {
    uint64_t base;      /* ECAM base for bus 0 of this segment */
    uint16_t segment;
    uint8_t  start_bus;
    uint8_t  end_bus;
} acpi_mcfg_entry_t;

/* Locate and validate all tables (checksums). */
status_t acpi_init(uint64_t rsdp_phys);

size_t   acpi_table_count(void);
const acpi_header_t *acpi_table_at(size_t index);

/* The index-th table with this signature, or NULL. */
const acpi_header_t *acpi_find_table(const char *signature, size_t index);

status_t acpi_parse_madt(acpi_madt_info_t *info);

/* Fill up to max ECAM ranges; returns the number found. */
size_t   acpi_parse_mcfg(acpi_mcfg_entry_t *entries, size_t max);

#endif
