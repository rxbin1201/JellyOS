/*
 * 8254 PIT, used only as a reference clock to calibrate other timers.
 */

#ifndef ARCH_X86_64_PIT_H
#define ARCH_X86_64_PIT_H

#include <stdint.h>

#define PIT_FREQUENCY_HZ 1193182u
#define PIT_MAX_WAIT_US  50000u

#include <stdbool.h>

/*
 * Busy-wait using PIT channel 2 (at most PIT_MAX_WAIT_US). False if the
 * counter never finishes: some firmware stops the PIT's clock to save power.
 */
bool pit_wait_us(uint32_t us);

#endif
