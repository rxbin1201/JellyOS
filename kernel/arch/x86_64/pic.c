/*
 * Legacy 8259 PIC: remapped away from the exception vectors and fully masked.
 * JellyOS uses the Local APIC; the PIC must still be tamed so it cannot raise
 * stray interrupts on vectors 0x08-0x0F.
 */

#include "pic.h"

#include "interrupt.h"
#include "io.h"

#include "core/log.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

#define ICW1_INIT_ICW4 0x11
#define ICW4_8086      0x01

static void io_wait(void)
{
    outb(0x80, 0); /* unused port, gives the PIC time to settle */
}

void pic_disable(void)
{
    outb(PIC1_COMMAND, ICW1_INIT_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT_ICW4);
    io_wait();
    outb(PIC1_DATA, VECTOR_PIC_BASE);      /* ICW2: vector offsets */
    io_wait();
    outb(PIC2_DATA, VECTOR_PIC_BASE + 8);
    io_wait();
    outb(PIC1_DATA, 0x04);                 /* ICW3: slave on IRQ2 */
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    outb(PIC1_DATA, 0xFF);                 /* mask everything */
    outb(PIC2_DATA, 0xFF);
    klog_debug("pic: remapped to 0x%x and masked", VECTOR_PIC_BASE);
}
