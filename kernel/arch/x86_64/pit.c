#include "pit.h"

#include "io.h"

#define PIT_CHANNEL2 0x42
#define PIT_COMMAND  0x43
#define PORT_B       0x61 /* bit 0: channel 2 gate, bit 1: speaker, bit 5: channel 2 output */

#define PIT_CMD_CH2_LOHI_MODE0 0xB0

bool pit_wait_us(uint32_t us)
{
    /* A port read takes around a microsecond on hardware and never less than a few nanoseconds emulated. */
    uint64_t limit = (uint64_t)us * 1000 + 1000000;
    bool finished = true;

    if (us > PIT_MAX_WAIT_US)
        us = PIT_MAX_WAIT_US;
    uint32_t count = (uint32_t)((uint64_t)PIT_FREQUENCY_HZ * us / 1000000);

    /* Gate low and speaker off while programming. */
    uint8_t port_b = inb(PORT_B) & ~0x03;
    outb(PORT_B, port_b);

    outb(PIT_COMMAND, PIT_CMD_CH2_LOHI_MODE0);
    outb(PIT_CHANNEL2, count & 0xFF);
    outb(PIT_CHANNEL2, (count >> 8) & 0xFF);

    /* Gate high starts the count; output goes high when it reaches zero. */
    outb(PORT_B, port_b | 0x01);
    for (uint64_t spins = 0; !(inb(PORT_B) & 0x20); spins++) {
        if (spins == limit) {
            finished = false;
            break;
        }
        __asm__ volatile("pause");
    }

    outb(PORT_B, port_b);
    return finished;
}
