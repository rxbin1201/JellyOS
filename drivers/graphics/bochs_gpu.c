/*
 * The standard VGA card of QEMU and Bochs ("Bochs display interface", PCI
 * 1234:1111): a framebuffer in BAR 0 whose size is set by a handful of
 * registers.
 *
 * The firmware lights it like any other card and JellyOS shows the UEFI
 * framebuffer on it. This driver adds the two things the card can do beyond
 * that: switching modes and showing nothing (the screen "off", as far as an
 * emulated card has one). It is the smallest example of a graphics driver
 * behind the display layer's interface (drivers/graphics/display.h), and it
 * makes mode switching testable in QEMU:
 *
 *   display_set_framebuffer()   all of the video memory instead of one mode's worth
 *   display_set_driver()        display_ops_t with set_mode and power
 *   display_set_modes()         a list of common sizes that fit the video memory
 *
 * The card has no vertical blank interrupt and no pointer plane; the
 * display server does those in software, as on a plain framebuffer.
 */

#include "drivers/bus/pci/pci.h"
#include "drivers/core/module.h"
#include "drivers/graphics/display.h"

#include "core/arch.h"
#include "core/boot.h"
#include "core/log.h"
#include "memory/layout.h"
#include "memory/vmm.h"

/* Registers: 16 bits each, in BAR 2 from 0x500 on, or through the I/O ports 0x1CE (index) and 0x1CF (data) */
#define DISPI_ID          0
#define DISPI_XRES        1
#define DISPI_YRES        2
#define DISPI_BPP         3
#define DISPI_ENABLE      4
#define DISPI_VIRT_WIDTH  6
#define DISPI_VIRT_HEIGHT 7
#define DISPI_X_OFFSET    8
#define DISPI_Y_OFFSET    9
#define DISPI_MEMORY_64K  10

#define DISPI_ENABLED     0x01
#define DISPI_LFB         0x40
#define DISPI_MMIO_OFFSET 0x500
/* The card is a VGA card as well: its ports 0x3C0 to 0x3DF, also in BAR 2 from 0x400 on, a byte each */
#define VGA_MMIO_OFFSET   0x400
#define VGA_ATTRIBUTE     0x3C0 /* index; bit 5 clear: the screen shows nothing */
#define VGA_MISC_WRITE    0x3C2 /* bit 0: colour (the status register is at 0x3DA) */
#define VGA_STATUS        0x3DA /* reading it makes the attribute port take an index next */
#define VGA_SCREEN_ON     0x20
#define DISPI_PORT_INDEX  0x1CE
#define DISPI_PORT_DATA   0x1CF

#define REFRESH_MHZ       60000 /* an emulated card has no timing; the usual value */

typedef struct {
    volatile uint16_t   *mmio; /* NULL: I/O ports */
    volatile uint8_t    *vga;  /* the VGA ports in BAR 2; NULL: I/O ports */
    jelly_display_mode_t modes[JELLY_DISPLAY_MODE_MAX];
    uint32_t             mode_count;
    uint32_t             current;
} bochs_t;

static uint16_t dispi_read(bochs_t *b, uint32_t index)
{
    if (b->mmio)
        return b->mmio[index];
    arch_io_write16(DISPI_PORT_INDEX, (uint16_t)index);
    return arch_io_read16(DISPI_PORT_DATA);
}

static void dispi_write(bochs_t *b, uint32_t index, uint16_t value)
{
    if (b->mmio) {
        b->mmio[index] = value;
        return;
    }
    arch_io_write16(DISPI_PORT_INDEX, (uint16_t)index);
    arch_io_write16(DISPI_PORT_DATA, value);
}

static void program(bochs_t *b, const jelly_display_mode_t *m)
{
    dispi_write(b, DISPI_ENABLE, 0);
    dispi_write(b, DISPI_XRES, (uint16_t)m->width);
    dispi_write(b, DISPI_YRES, (uint16_t)m->height);
    dispi_write(b, DISPI_BPP, 32);
    dispi_write(b, DISPI_VIRT_WIDTH, (uint16_t)m->width);
    dispi_write(b, DISPI_VIRT_HEIGHT, (uint16_t)m->height);
    dispi_write(b, DISPI_X_OFFSET, 0);
    dispi_write(b, DISPI_Y_OFFSET, 0);
    dispi_write(b, DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB); /* also clears the video memory */
}

static status_t bochs_set_mode(display_t *display, uint32_t mode, uint32_t *pitch)
{
    bochs_t *b = display->driver_data;
    const jelly_display_mode_t *m = &b->modes[mode];

    program(b, m);
    if (dispi_read(b, DISPI_XRES) != m->width || dispi_read(b, DISPI_YRES) != m->height) {
        program(b, &b->modes[b->current]); /* the card refused: the mode before */
        return STATUS_DEVICE_ERROR;
    }
    b->current = mode;
    *pitch = m->width * 4;
    return STATUS_SUCCESS;
}

static void vga_write(bochs_t *b, uint16_t port, uint8_t value)
{
    if (b->vga)
        b->vga[port - VGA_ATTRIBUTE] = value;
    else
        arch_io_write8(port, value);
}

/* Blank or not (as Linux's bochs driver does it): the mode and the video memory stay. */
static void screen(bochs_t *b, bool on)
{
    vga_write(b, VGA_MISC_WRITE, 0x01);
    if (b->vga)
        (void)b->vga[VGA_STATUS - VGA_ATTRIBUTE];
    else
        (void)arch_io_read8(VGA_STATUS);
    vga_write(b, VGA_ATTRIBUTE, on ? VGA_SCREEN_ON : 0);
}

static status_t bochs_power(display_t *display, bool on)
{
    screen(display->driver_data, on);
    return STATUS_SUCCESS;
}

/* A panic's report is in the framebuffer, which is what the card shows: only a blank screen is in the way. */
static void bochs_panic(display_t *display)
{
    screen(display->driver_data, true);
}

static const display_ops_t bochs_ops = { .set_mode = bochs_set_mode, .power = bochs_power, .panic = bochs_panic };

static void add_mode(bochs_t *b, uint32_t width, uint32_t height, uint64_t memory)
{
    if ((uint64_t)width * height * 4 > memory || b->mode_count == JELLY_DISPLAY_MODE_MAX)
        return;
    for (uint32_t i = 0; i < b->mode_count; i++) {
        if (b->modes[i].width == width && b->modes[i].height == height)
            return;
    }
    b->modes[b->mode_count++] = (jelly_display_mode_t){ width, height, REFRESH_MHZ, 0 };
}

static status_t bochs_probe(device_t *device)
{
    static bochs_t card; /* one such card shows the boot framebuffer */
    static const uint16_t sizes[][2] = { { 1920, 1080 }, { 1600, 900 }, { 1440, 900 }, { 1280, 1024 }, { 1280, 800 },
                                         { 1280, 720 },  { 1024, 768 }, { 800, 600 },  { 640, 480 } };
    bochs_t *b = &card;
    pci_device_t *pci = pci_from_device(device);
    display_t *d = display_get(0);

    if (b->mode_count || !d || !pci->bars[0].phys || pci->bars[0].io)
        return STATUS_NOT_SUPPORTED;
    if (d->phys != pci->bars[0].phys || d->info.bpp != 32 || d->info.pitch != d->info.width * 4) {
        klog_info("bochs-gpu: the boot framebuffer is not this card's: left alone");
        return STATUS_NOT_SUPPORTED;
    }
    status_t status = pci_enable_device(pci, false);
    if (STATUS_IS_ERROR(status))
        return status;
    if (pci->bars[2].phys && !pci->bars[2].io && pci->bars[2].size >= DISPI_MMIO_OFFSET + 0x20) {
        volatile uint8_t *registers = (volatile uint8_t *)vmm_map_mmio(pci->bars[2].phys, PAGE_SIZE, VM_UNCACHED);
        if (registers) {
            b->mmio = (volatile uint16_t *)(registers + DISPI_MMIO_OFFSET);
            b->vga = registers + VGA_MMIO_OFFSET;
        }
    }
    uint16_t id = dispi_read(b, DISPI_ID);
    if ((id & 0xFFF0) != 0xB0C0) {
        klog_info("bochs-gpu: no Bochs display interface (id 0x%x)", id);
        return STATUS_NOT_SUPPORTED;
    }
    uint64_t memory = (uint64_t)dispi_read(b, DISPI_MEMORY_64K) * 65536;
    if (!memory || memory > pci->bars[0].size)
        memory = pci->bars[0].size;

    /* The firmware's mode first: it is what the screen shows, and the one to fall back to. */
    add_mode(b, d->info.width, d->info.height, memory);
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
        add_mode(b, sizes[i][0], sizes[i][1], memory);
    if (!b->mode_count)
        return STATUS_NOT_SUPPORTED;
    b->modes[0].flags = JELLY_MODE_PREFERRED;
    b->current = 0;

    klog_info("bochs-gpu: standard VGA 1234:%04x, %lu MiB video memory, registers through %s", device->id.device,
              memory >> 20, b->mmio ? "BAR 2" : "I/O ports");
    /* The same memory as before, but all of it: every mode fits without the framebuffer moving. */
    status = display_set_framebuffer(0, d->phys, memory, d->info.width, d->info.height, d->info.pitch);
    if (!STATUS_IS_ERROR(status))
        status = display_set_driver(0, &bochs_ops, b, 0);
    if (!STATUS_IS_ERROR(status))
        status = display_set_modes(0, b->modes, b->mode_count, 0);
    if (STATUS_IS_ERROR(status))
        klog_warn("bochs-gpu: the display does not take the driver (%s)", status_name(status));
    return status;
}

static const device_match_t bochs_ids[] = {
    { 0x1234, 0x1111, 0, 0, MATCH_VENDOR | MATCH_DEVICE },
    DEVICE_MATCH_END,
};

static driver_t bochs_driver = {
    .name = "bochs-gpu",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_DISPLAY,
    .ids = bochs_ids,
    .probe = bochs_probe,
};

static status_t bochs_module_init(void)
{
    return driver_register(&bochs_driver);
}

static const char *const bochs_dependencies[] = { "pci", NULL };

MODULE(.name = "bochs_gpu", .description = "QEMU/Bochs standard VGA: mode switching", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 12, 0), .dependencies = bochs_dependencies, .init = bochs_module_init);
