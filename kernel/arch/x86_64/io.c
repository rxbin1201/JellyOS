/*
 * Port I/O for generic code (README section 22: I/O port resources).
 */

#include "core/arch.h"
#include "core/export.h"

uint8_t arch_io_read8(uint16_t port)
{
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint16_t arch_io_read16(uint16_t port)
{
    uint16_t value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint32_t arch_io_read32(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void arch_io_write8(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

void arch_io_write16(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

void arch_io_write32(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

EXPORT_SYMBOL(arch_io_read8);
EXPORT_SYMBOL(arch_io_read16);
EXPORT_SYMBOL(arch_io_read32);
EXPORT_SYMBOL(arch_io_write8);
EXPORT_SYMBOL(arch_io_write16);
EXPORT_SYMBOL(arch_io_write32);
