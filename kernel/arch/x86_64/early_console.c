/*
 * Early console on COM1 (16550 UART), usable before any other kernel subsystem.
 *
 * Many PCs have no serial port. Then nothing is written: waiting for a
 * transmitter that does not exist would hang the kernel at its first
 * message. A port that stops accepting characters is given up, too.
 */

#include "core/arch.h"
#include "io.h"

#define COM1             0x3F8
#define UART_DATA        0
#define UART_INT_ENABLE  1
#define UART_FIFO_CTRL   2
#define UART_LINE_CTRL   3
#define UART_MODEM_CTRL  4
#define UART_LINE_STATUS 5
#define UART_SCRATCH     7

#define LINE_CTRL_DLAB   0x80
#define LINE_CTRL_8N1    0x03
#define LINE_STATUS_THRE 0x20

#define TRANSMIT_SPINS 200000 /* far longer than a character takes at 115200 baud */

static bool present;

/* A UART keeps what is written to its scratch register; an empty bus does not. */
static bool uart_answers(void)
{
    outb(COM1 + UART_SCRATCH, 0x5A);
    if (inb(COM1 + UART_SCRATCH) != 0x5A)
        return false;
    outb(COM1 + UART_SCRATCH, 0xA5);
    return inb(COM1 + UART_SCRATCH) == 0xA5 && inb(COM1 + UART_LINE_STATUS) != 0xFF;
}

bool arch_early_console_present(void)
{
    return present;
}

void arch_early_console_init(void)
{
    present = uart_answers();
    if (!present)
        return;
    outb(COM1 + UART_INT_ENABLE, 0x00);
    outb(COM1 + UART_LINE_CTRL, LINE_CTRL_DLAB);
    outb(COM1 + UART_DATA, 0x01);        /* divisor low: 115200 baud */
    outb(COM1 + UART_INT_ENABLE, 0x00);  /* divisor high */
    outb(COM1 + UART_LINE_CTRL, LINE_CTRL_8N1);
    outb(COM1 + UART_FIFO_CTRL, 0xC7);   /* enable and clear FIFOs */
    outb(COM1 + UART_MODEM_CTRL, 0x03);  /* DTR, RTS */
}

void arch_early_console_put(char c)
{
    if (!present)
        return;
    for (uint32_t spins = 0; !(inb(COM1 + UART_LINE_STATUS) & LINE_STATUS_THRE); spins++) {
        if (spins == TRANSMIT_SPINS) {
            present = false; /* stuck: stop using it */
            return;
        }
    }
    outb(COM1 + UART_DATA, (uint8_t)c);
}

void arch_early_console_write(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            arch_early_console_put('\r');
        arch_early_console_put(*s);
    }
}
