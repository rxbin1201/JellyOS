/*
 * Early console on COM1 (16550 UART), usable before any other kernel subsystem.
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

#define LINE_CTRL_DLAB   0x80
#define LINE_CTRL_8N1    0x03
#define LINE_STATUS_THRE 0x20

void arch_early_console_init(void)
{
    outb(COM1 + UART_INT_ENABLE, 0x00);
    outb(COM1 + UART_LINE_CTRL, LINE_CTRL_DLAB);
    outb(COM1 + UART_DATA, 0x01);        /* divisor low: 115200 baud */
    outb(COM1 + UART_INT_ENABLE, 0x00);  /* divisor high */
    outb(COM1 + UART_LINE_CTRL, LINE_CTRL_8N1);
    outb(COM1 + UART_FIFO_CTRL, 0xC7);   /* enable and clear FIFOs */
    outb(COM1 + UART_MODEM_CTRL, 0x03);  /* DTR, RTS */
}

static void put_char(char c)
{
    while (!(inb(COM1 + UART_LINE_STATUS) & LINE_STATUS_THRE))
        ;
    outb(COM1 + UART_DATA, (uint8_t)c);
}

void arch_early_console_write(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            put_char('\r');
        put_char(*s);
    }
}
