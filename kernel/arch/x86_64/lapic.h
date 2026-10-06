/*
 * Local APIC (xAPIC via MMIO, or x2APIC via MSRs when available) and its timer.
 */

#ifndef ARCH_X86_64_LAPIC_H
#define ARCH_X86_64_LAPIC_H

#include <jelly/status.h>
#include <stdint.h>

status_t lapic_init(void);

/* Calibrate against the PIT and start a periodic interrupt at hz. */
status_t lapic_timer_start(uint32_t hz);

void     lapic_eoi(void);
uint32_t lapic_id(void);

#endif
