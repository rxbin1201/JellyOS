#include "gdt.h"

#include "core/log.h"

#include <stdint.h>

#define IST_STACK_SIZE 16384

typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss_t;

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} descriptor_pointer_t;

/* Flat 64-bit segments: present, ring 0/3, code (execute/read) or data (read/write). */
#define SEGMENT_KERNEL_CODE 0x00AF9A000000FFFFULL
#define SEGMENT_KERNEL_DATA 0x00CF92000000FFFFULL
#define SEGMENT_USER_DATA   0x00CFF2000000FFFFULL
#define SEGMENT_USER_CODE   0x00AFFA000000FFFFULL
#define TSS_TYPE_AVAILABLE  0x89ULL

static uint64_t gdt[7] __attribute__((aligned(16)));
static tss_t tss __attribute__((aligned(16)));
static uint8_t ist_stacks[3][IST_STACK_SIZE] __attribute__((aligned(16)));

static void set_tss_descriptor(unsigned index, const tss_t *t)
{
    uint64_t base = (uint64_t)(uintptr_t)t;
    uint64_t limit = sizeof(*t) - 1;

    gdt[index] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (TSS_TYPE_AVAILABLE << 40) |
                 (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[index + 1] = base >> 32;
}

static void load_segments(void)
{
    descriptor_pointer_t pointer = { sizeof(gdt) - 1, (uint64_t)(uintptr_t)gdt };

    __asm__ volatile(
        "lgdt %0\n"
        "pushq %1\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"                 /* reload CS */
        "1:\n"
        "mov %2, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "xor %%eax, %%eax\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %3, %%ax\n"
        "ltr %%ax\n"
        :
        : "m"(pointer), "i"(GDT_KERNEL_CODE), "i"(GDT_KERNEL_DATA), "i"(GDT_TSS)
        : "rax", "memory");
}

void gdt_init(void)
{
    gdt[0] = 0;
    gdt[GDT_KERNEL_CODE / 8] = SEGMENT_KERNEL_CODE;
    gdt[GDT_KERNEL_DATA / 8] = SEGMENT_KERNEL_DATA;
    gdt[GDT_USER_DATA / 8] = SEGMENT_USER_DATA;
    gdt[GDT_USER_CODE / 8] = SEGMENT_USER_CODE;

    for (unsigned i = 0; i < 3; i++)
        tss.ist[i] = (uint64_t)(uintptr_t)(ist_stacks[i] + IST_STACK_SIZE);
    tss.iomap_base = sizeof(tss); /* no I/O permission bitmap */
    set_tss_descriptor(GDT_TSS / 8, &tss);

    load_segments();
    klog_debug("gdt: loaded, TSS with %u IST stacks of %u KiB", 3u, IST_STACK_SIZE / 1024);
}
