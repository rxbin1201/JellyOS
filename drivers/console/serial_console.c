/*
 * Serial console: /dev/console on COM1.
 *
 * Output goes straight to the UART. Input arrives by interrupt (ISA IRQ 4
 * through the IOAPIC) and passes a line discipline in the kernel: characters
 * are echoed, Backspace edits the current line, Enter completes it, Ctrl-C
 * discards it and Ctrl-D on an empty line reports end of input. Readers get
 * at most one completed line per read.
 *
 * Everything written here is mirrored to the framebuffer console (if one is
 * active), so the console is visible on the screen as well.
 */

#include "drivers/core/module.h"
#include "fs/vfs/vfs.h"

#include "core/arch.h"
#include "core/log.h"
#include "core/string.h"

#define COM1               0x3F8
#define UART_DATA          0
#define UART_INT_ENABLE    1
#define UART_MODEM_CTRL    4
#define UART_LINE_STATUS   5
#define LINE_STATUS_DATA   0x01
#define LINE_STATUS_THRE   0x20
#define INT_RECEIVED_DATA  0x01
#define MODEM_OUT2         0x08 /* routes the UART interrupt on PC hardware */
#define COM1_IRQ           4

#define LINE_MAX           256
#define READY_SIZE         4096
#define CHAR_EOF_MARK      0x04

static char line[LINE_MAX];
static size_t line_length;
static char ready[READY_SIZE];  /* completed lines (ring) */
static size_t ready_head, ready_count;
static wait_queue_t readers;

static void put_raw(char c)
{
    while (!(arch_io_read8(COM1 + UART_LINE_STATUS) & LINE_STATUS_THRE))
        ;
    arch_io_write8(COM1 + UART_DATA, (uint8_t)c);
    kconsole_mirror_char(c); /* the screen shows the console too */
}

static void put_char(char c)
{
    if (c == '\n')
        put_raw('\r');
    put_raw(c);
}

static void push_ready(char c)
{
    if (ready_count == READY_SIZE)
        return; /* input overflow: drop */
    ready[(ready_head + ready_count) % READY_SIZE] = c;
    ready_count++;
}

/* Line discipline, called in interrupt context. */
static void receive(char c)
{
    switch (c) {
    case '\r':
    case '\n':
        put_char('\n');
        for (size_t i = 0; i < line_length; i++)
            push_ready(line[i]);
        push_ready('\n');
        line_length = 0;
        wait_queue_wake_all(&readers, STATUS_SUCCESS);
        break;
    case 0x7F: /* Backspace (DEL) */
    case 0x08:
        if (line_length) {
            line_length--;
            put_raw('\b');
            put_raw(' ');
            put_raw('\b');
        }
        break;
    case 0x03: /* Ctrl-C: discard the line */
        put_raw('^');
        put_raw('C');
        put_char('\n');
        line_length = 0;
        break;
    case 0x04: /* Ctrl-D: end of input when the line is empty */
        if (line_length == 0) {
            push_ready(CHAR_EOF_MARK);
            wait_queue_wake_all(&readers, STATUS_SUCCESS);
        }
        break;
    default:
        if ((unsigned char)c >= 0x20 && c != 0x7F && line_length < LINE_MAX - 1) {
            line[line_length++] = c;
            put_raw(c);
        }
        break;
    }
}

static void uart_interrupt(void *context)
{
    (void)context;
    while (arch_io_read8(COM1 + UART_LINE_STATUS) & LINE_STATUS_DATA)
        receive((char)arch_io_read8(COM1 + UART_DATA));
}

/* --- /dev/console ------------------------------------------------------------- */

static status_t console_read(vnode_t *v, uint64_t offset, void *buffer, size_t size, size_t *done)
{
    char *out = buffer;
    status_t status = STATUS_SUCCESS;

    (void)v, (void)offset;
    *done = 0;
    if (size == 0)
        return STATUS_SUCCESS;

    uint64_t flags = arch_interrupts_save();
    while (ready_count == 0 && status == STATUS_SUCCESS)
        status = wait_queue_block(&readers, WAIT_FOREVER);

    if (status == STATUS_SUCCESS) {
        if (ready[ready_head] == CHAR_EOF_MARK) {
            ready_head = (ready_head + 1) % READY_SIZE; /* end of input: a read of 0 bytes */
            ready_count--;
        } else {
            while (ready_count && *done < size) {
                char c = ready[ready_head];
                if (c == CHAR_EOF_MARK)
                    break;
                out[(*done)++] = c;
                ready_head = (ready_head + 1) % READY_SIZE;
                ready_count--;
                if (c == '\n')
                    break; /* one line per read */
            }
        }
    }
    arch_interrupts_restore(flags);
    return status;
}

static status_t console_write(vnode_t *v, uint64_t offset, const void *buffer, size_t size, size_t *done)
{
    const char *in = buffer;

    (void)v, (void)offset;
    uint64_t flags = arch_interrupts_save(); /* keep the line together with kernel log output */
    for (size_t i = 0; i < size; i++)
        put_char(in[i]);
    arch_interrupts_restore(flags);
    *done = size;
    return STATUS_SUCCESS;
}

static const vnode_ops_t console_ops = {
    .read = console_read,
    .write = console_write,
};

static status_t serial_console_init(void)
{
    uint32_t irq, gsi = 0;

    wait_queue_init(&readers);
    status_t status = arch_irq_allocate(uart_interrupt, NULL, &irq);
    if (!STATUS_IS_ERROR(status))
        status = arch_irq_route_isa(COM1_IRQ, irq, &gsi);
    if (STATUS_IS_ERROR(status)) {
        klog_warn("console: no input interrupt (%s), /dev/console is output only", status_name(status));
    } else {
        arch_io_write8(COM1 + UART_MODEM_CTRL, arch_io_read8(COM1 + UART_MODEM_CTRL) | MODEM_OUT2);
        arch_io_write8(COM1 + UART_INT_ENABLE, INT_RECEIVED_DATA);
        while (arch_io_read8(COM1 + UART_LINE_STATUS) & LINE_STATUS_DATA)
            arch_io_read8(COM1 + UART_DATA); /* drop stale input */
    }

    status = devfs_register("console", &console_ops, 0620, NULL);
    if (!STATUS_IS_ERROR(status))
        klog_info("console: /dev/console on COM1 (input via GSI %u)", gsi);
    return status;
}

MODULE(.name = "serial_console", .description = "Serial console on COM1", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 7, 0), .init = serial_console_init);
