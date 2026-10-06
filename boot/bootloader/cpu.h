/*
 * JellyOS Boot Manager - x86_64 CPU helpers.
 */

#ifndef BOOT_CPU_H
#define BOOT_CPU_H

#include <stdbool.h>
#include <stdint.h>

#define MSR_EFER      0xC0000080
#define EFER_NXE      (1ULL << 11)
#define CR4_LA57      (1ULL << 12)
#define CPUID_EXT_NX  (1u << 20)

static inline void cpu_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static inline uint64_t cpu_read_msr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void cpu_write_msr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

static inline uint64_t cpu_read_cr4(void)
{
    uint64_t value;
    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

static inline bool cpu_supports_nx(void)
{
    uint32_t a, b, c, d;

    cpu_cpuid(0x80000000, &a, &b, &c, &d);
    if (a < 0x80000001)
        return false;
    cpu_cpuid(0x80000001, &a, &b, &c, &d);
    return d & CPUID_EXT_NX;
}

static inline bool cpu_five_level_paging(void)
{
    return cpu_read_cr4() & CR4_LA57;
}

static inline void cpu_enable_nx(void)
{
    cpu_write_msr(MSR_EFER, cpu_read_msr(MSR_EFER) | EFER_NXE);
}

#endif
