/*
 * x86_64 CPU primitives: CPUID, MSRs, control registers, interrupt flag.
 */

#ifndef ARCH_X86_64_CPU_H
#define ARCH_X86_64_CPU_H

#include <stdbool.h>
#include <stdint.h>

#define MSR_APIC_BASE 0x1B
#define MSR_EFER      0xC0000080

#define CR0_MP        (1ULL << 1)
#define CR0_EM        (1ULL << 2)
#define CR0_TS        (1ULL << 3)
#define CR0_WP        (1ULL << 16)
#define CR4_OSFXSR    (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define CR4_UMIP      (1ULL << 11)
#define CR4_SMEP      (1ULL << 20)
#define CR4_SMAP      (1ULL << 21)
#define RFLAGS_IF     (1ULL << 9)
#define EFER_NXE      (1ULL << 11)

typedef struct {
    char     vendor[13];
    char     brand[49];
    bool     apic;
    bool     x2apic;
    bool     nx;
    bool     tsc;
    bool     invariant_tsc;
    bool     page_1g;
    bool     smep;
    bool     smap;
    bool     umip;
} cpu_features_t;

extern cpu_features_t cpu_features;

void cpu_init(void);

static inline void cpu_cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
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

#define DEFINE_CR_READ(n)                                     \
    static inline uint64_t cpu_read_cr##n(void)               \
    {                                                         \
        uint64_t v;                                           \
        __asm__ volatile("mov %%cr" #n ", %0" : "=r"(v));     \
        return v;                                             \
    }
DEFINE_CR_READ(0)
DEFINE_CR_READ(2)
DEFINE_CR_READ(3)
DEFINE_CR_READ(4)
#undef DEFINE_CR_READ

static inline void cpu_write_cr0(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

static inline void cpu_write_cr4(uint64_t v)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

static inline uint64_t cpu_read_rflags(void)
{
    uint64_t v;
    __asm__ volatile("pushfq; pop %0" : "=r"(v));
    return v;
}

static inline void cpu_invlpg(uint64_t address)
{
    __asm__ volatile("invlpg (%0)" : : "r"(address) : "memory");
}

#endif
