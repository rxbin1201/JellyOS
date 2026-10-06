#include "lapic.h"

#include "cpu.h"
#include "interrupt.h"
#include "pit.h"

#include "core/log.h"
#include "memory/vmm.h"
#include "time/clock.h"

#include <stdbool.h>

/* Register offsets (xAPIC MMIO); x2APIC MSR = 0x800 + offset / 16. */
#define LAPIC_ID            0x020
#define LAPIC_VERSION       0x030
#define LAPIC_TPR           0x080
#define LAPIC_EOI           0x0B0
#define LAPIC_SVR           0x0F0
#define LAPIC_ESR           0x280
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_LVT_LINT0     0x350
#define LAPIC_LVT_ERROR     0x370
#define LAPIC_TIMER_INITIAL 0x380
#define LAPIC_TIMER_CURRENT 0x390
#define LAPIC_TIMER_DIVIDE  0x3E0

#define APIC_BASE_ENABLE    (1ULL << 11)
#define APIC_BASE_X2APIC    (1ULL << 10)
#define APIC_BASE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define SVR_ENABLE          (1u << 8)
#define LVT_MASKED          (1u << 16)
#define LVT_TIMER_PERIODIC  (1u << 17)
#define TIMER_DIVIDE_16     0x3

#define CALIBRATION_US      10000u

static bool x2apic;
static volatile uint32_t *mmio;
static bool ready;

static uint32_t read_reg(uint32_t reg)
{
    if (x2apic)
        return (uint32_t)cpu_read_msr(0x800 + (reg >> 4));
    return mmio[reg / 4];
}

static void write_reg(uint32_t reg, uint32_t value)
{
    if (x2apic)
        cpu_write_msr(0x800 + (reg >> 4), value);
    else
        mmio[reg / 4] = value;
}

void lapic_eoi(void)
{
    write_reg(LAPIC_EOI, 0);
}

uint32_t lapic_id(void)
{
    if (!ready)
        return 0;
    uint32_t id = read_reg(LAPIC_ID);
    return x2apic ? id : id >> 24;
}

static void spurious_handler(struct arch_interrupt_frame *frame)
{
    (void)frame; /* spurious interrupts must not be acknowledged */
}

static void error_handler(struct arch_interrupt_frame *frame)
{
    (void)frame;
    write_reg(LAPIC_ESR, 0);
    klog_warn("lapic: error interrupt, ESR=0x%x", read_reg(LAPIC_ESR));
    lapic_eoi();
}

static void timer_handler(struct arch_interrupt_frame *frame)
{
    (void)frame;
    clock_tick();
    lapic_eoi();
}

status_t lapic_init(void)
{
    if (!cpu_features.apic) {
        klog_error("lapic: CPU has no local APIC");
        return STATUS_NOT_SUPPORTED;
    }

    uint64_t base = cpu_read_msr(MSR_APIC_BASE);
    uint64_t phys = base & APIC_BASE_ADDR_MASK;

    x2apic = cpu_features.x2apic;
    base |= APIC_BASE_ENABLE;
    if (x2apic)
        base |= APIC_BASE_X2APIC;
    cpu_write_msr(MSR_APIC_BASE, base);

    if (!x2apic) {
        mmio = vmm_map_mmio(phys, 0x1000, VM_UNCACHED);
        if (!mmio) {
            klog_error("lapic: cannot map registers at 0x%lx", phys);
            return STATUS_OUT_OF_MEMORY;
        }
    }

    interrupt_set_handler(VECTOR_LAPIC_SPURIOUS, spurious_handler);
    interrupt_set_handler(VECTOR_LAPIC_ERROR, error_handler);
    interrupt_set_handler(VECTOR_LAPIC_TIMER, timer_handler);

    write_reg(LAPIC_TPR, 0);
    write_reg(LAPIC_LVT_LINT0, LVT_MASKED); /* legacy PIC input, PIC is masked */
    write_reg(LAPIC_LVT_ERROR, VECTOR_LAPIC_ERROR);
    write_reg(LAPIC_ESR, 0);
    write_reg(LAPIC_ESR, 0);
    write_reg(LAPIC_SVR, SVR_ENABLE | VECTOR_LAPIC_SPURIOUS);
    lapic_eoi();
    ready = true;

    klog_info("lapic: %s mode, id %u, version 0x%x", x2apic ? "x2APIC" : "xAPIC", lapic_id(),
              read_reg(LAPIC_VERSION) & 0xFF);
    if (!x2apic)
        klog_debug("lapic: registers at phys 0x%lx mapped uncached at %p", phys, (void *)mmio);
    return STATUS_SUCCESS;
}

status_t lapic_timer_start(uint32_t hz)
{
    /* Count down from the maximum for a known PIT interval. */
    write_reg(LAPIC_TIMER_DIVIDE, TIMER_DIVIDE_16);
    write_reg(LAPIC_LVT_TIMER, LVT_MASKED | VECTOR_LAPIC_TIMER);
    write_reg(LAPIC_TIMER_INITIAL, 0xFFFFFFFF);
    pit_wait_us(CALIBRATION_US);
    uint32_t elapsed = 0xFFFFFFFF - read_reg(LAPIC_TIMER_CURRENT);
    write_reg(LAPIC_TIMER_INITIAL, 0);

    uint64_t timer_hz = (uint64_t)elapsed * (1000000 / CALIBRATION_US);
    uint64_t count = timer_hz / hz;
    if (count == 0 || count > 0xFFFFFFFF) {
        klog_error("lapic: timer calibration failed (%u counts in %u us)", elapsed, CALIBRATION_US);
        return STATUS_DEVICE_ERROR;
    }

    clock_init(1000000000ull / hz);
    write_reg(LAPIC_LVT_TIMER, LVT_TIMER_PERIODIC | VECTOR_LAPIC_TIMER);
    write_reg(LAPIC_TIMER_INITIAL, (uint32_t)count);

    klog_info("lapic: timer %lu kHz (divided), periodic at %u Hz", timer_hz / 1000, hz);
    return STATUS_SUCCESS;
}
