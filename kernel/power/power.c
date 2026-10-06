/*
 * Shutdown and reboot.
 *
 * Power off needs the S5 sleep type from the DSDT. Without an AML
 * interpreter, the "_S5_" package is located by its byte pattern, the usual
 * approach until AML support exists. The values are written to PM1a/PM1b
 * control from the FADT together with SLP_EN.
 */

#include "power/power.h"

#include "core/arch.h"
#include "core/log.h"
#include "core/string.h"
#include "drivers/acpi/acpi.h"
#include "fs/vfs/vfs.h"
#include "memory/vmm.h"

#define FADT_DSDT          40
#define FADT_PM1A_CONTROL  64
#define FADT_PM1B_CONTROL  68
#define FADT_FLAGS         112
#define FADT_RESET_REG     116
#define FADT_RESET_VALUE   128
#define FADT_X_DSDT        140
#define FADT_RESET_SUPPORT (1u << 10)
#define SLP_EN             (1u << 13)
#define AML_PACKAGE_OP     0x12
#define AML_BYTE_PREFIX    0x0A
#define GAS_SYSTEM_IO      1

static uint32_t read32(const uint8_t *table, uint32_t offset)
{
    uint32_t value;
    memcpy(&value, table + offset, 4);
    return value;
}

/* One integer element of an AML package: BytePrefix n, or the small constants Zero/One. */
static const uint8_t *aml_integer(const uint8_t *p, uint8_t *value)
{
    if (*p == AML_BYTE_PREFIX) {
        *value = p[1];
        return p + 2;
    }
    *value = *p;
    return p + 1;
}

static status_t find_s5(const uint8_t *fadt, uint8_t *slp_a, uint8_t *slp_b)
{
    const acpi_header_t *f = (const acpi_header_t *)fadt;
    uint64_t dsdt_phys = read32(fadt, FADT_DSDT);
    if (f->length >= FADT_X_DSDT + 8) {
        uint64_t x;
        memcpy(&x, fadt + FADT_X_DSDT, 8);
        if (x)
            dsdt_phys = x;
    }

    const acpi_header_t *header = vmm_phys_to_kernel(dsdt_phys, sizeof(acpi_header_t));
    if (!header)
        return STATUS_NOT_FOUND;
    const uint8_t *dsdt = vmm_phys_to_kernel(dsdt_phys, header->length);
    if (!dsdt)
        return STATUS_NOT_FOUND;

    for (uint32_t i = sizeof(acpi_header_t); i + 12 < header->length; i++) {
        if (memcmp(dsdt + i, "_S5_", 4) != 0 || dsdt[i + 4] != AML_PACKAGE_OP)
            continue;
        const uint8_t *p = dsdt + i + 5;
        p += 1 + (*p >> 6); /* PkgLength: lead byte plus 0-3 follow bytes */
        p++;                /* NumElements */
        p = aml_integer(p, slp_a);
        aml_integer(p, slp_b);
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_FOUND;
}

status_t power_off(void)
{
    uint8_t slp_a, slp_b;
    const uint8_t *fadt = (const uint8_t *)acpi_find_table("FACP", 0);

    vfs_sync();
    if (!fadt || STATUS_IS_ERROR(find_s5(fadt, &slp_a, &slp_b))) {
        klog_error("power: no ACPI S5 information, cannot power off");
        return STATUS_NOT_SUPPORTED;
    }

    uint32_t pm1a = read32(fadt, FADT_PM1A_CONTROL), pm1b = read32(fadt, FADT_PM1B_CONTROL);
    klog_info("power: powering off");
    arch_interrupts_disable();
    arch_io_write16((uint16_t)pm1a, (uint16_t)((slp_a << 10) | SLP_EN));
    if (pm1b)
        arch_io_write16((uint16_t)pm1b, (uint16_t)((slp_b << 10) | SLP_EN));
    arch_interrupts_enable();
    klog_error("power: the machine did not power off");
    return STATUS_DEVICE_ERROR;
}

status_t power_reboot(void)
{
    const uint8_t *fadt = (const uint8_t *)acpi_find_table("FACP", 0);

    vfs_sync();
    klog_info("power: rebooting");
    arch_interrupts_disable();

    if (fadt && ((const acpi_header_t *)fadt)->length > FADT_RESET_VALUE &&
        (read32(fadt, FADT_FLAGS) & FADT_RESET_SUPPORT) && fadt[FADT_RESET_REG] == GAS_SYSTEM_IO) {
        uint64_t port;
        memcpy(&port, fadt + FADT_RESET_REG + 4, 8);
        arch_io_write8((uint16_t)port, fadt[FADT_RESET_VALUE]);
    }
    arch_io_write8(0xCF9, 0x06); /* PCI reset control: full reset */
    arch_io_write8(0x64, 0xFE);  /* keyboard controller: pulse reset line */

    arch_interrupts_enable();
    klog_error("power: the machine did not reset");
    return STATUS_DEVICE_ERROR;
}
