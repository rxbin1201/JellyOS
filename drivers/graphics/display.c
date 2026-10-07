/*
 * Display registry and the UEFI GOP framebuffer (README section 33: "Start
 * with the UEFI framebuffer").
 */

#include "drivers/graphics/display.h"

#include "core/arch.h"
#include "core/boot.h"
#include "core/cmdline.h"
#include "core/log.h"
#include "core/string.h"
#include "ipc/ipc.h"
#include "memory/layout.h"
#include "memory/mmu.h"
#include "memory/vmm.h"

static display_t displays[DISPLAY_MAX];
static uint32_t count;

display_t *display_get(uint32_t index)
{
    return index < count ? &displays[index] : NULL;
}

uint32_t display_count(void)
{
    return count;
}

void display_init_boot_framebuffer(void)
{
    const boot_framebuffer_t *fb = &boot_info()->framebuffer;

    if (!fb->phys_base || fb->bpp != 32 || !fb->width || !fb->height) {
        klog_info("display: no usable boot framebuffer");
        return;
    }
    display_t *d = &displays[count];
    uint64_t size = align_up((uint64_t)fb->pitch * fb->height, PAGE_SIZE);
    d->pixels = (volatile uint32_t *)vmm_map_mmio(fb->phys_base, size, VM_WRITE_COMBINING);
    if (!d->pixels) {
        klog_warn("display: cannot map the framebuffer");
        return;
    }
    d->phys = fb->phys_base;
    d->info = (jelly_display_info_t){
        .index = count,
        .width = fb->width,
        .height = fb->height,
        .pitch = fb->pitch,
        .bpp = 32,
        .red_shift = fb->red_shift,
        .red_size = fb->red_size,
        .green_shift = fb->green_shift,
        .green_size = fb->green_size,
        .blue_shift = fb->blue_shift,
        .blue_size = fb->blue_size,
        .size = size,
    };
    count++;
    klog_info("display: %ux%u framebuffer at 0x%lx (GOP, pitch %u)", fb->width, fb->height, fb->phys_base, fb->pitch);

    /* fbconsole=0 keeps the console on the serial port only. */
    char setting[8];
    if (!cmdline_value("fbconsole", setting, sizeof(setting)) || strcmp(setting, "0") != 0)
        fb_console_init(d);
}

status_t display_set_framebuffer(uint32_t index, uint64_t phys, uint32_t width, uint32_t height, uint32_t pitch)
{
    display_t *d = display_get(index);

    if (!d)
        return STATUS_NOT_FOUND;
    if (d->acquired)
        return STATUS_BUSY;
    if (!width || !height || pitch < width * 4 || (phys & (PAGE_SIZE - 1)))
        return STATUS_INVALID_ARGUMENT;
    uint64_t size = align_up((uint64_t)pitch * height, PAGE_SIZE);
    volatile uint32_t *pixels = (volatile uint32_t *)vmm_map_mmio(phys, size, VM_WRITE_COMBINING);
    if (!pixels)
        return STATUS_OUT_OF_MEMORY;

    /*
     * The console draws from interrupt handlers, too, with a text grid laid out for the old size: it stops
     * drawing first, and fb_console_resize() switches it to the new size in one step.
     */
    if (index == 0)
        fb_console_set_active(false);
    uint64_t saved = arch_interrupts_save();
    d->pixels = pixels;
    d->phys = phys;
    d->info.width = width;
    d->info.height = height;
    d->info.pitch = pitch;
    d->info.size = size;
    arch_interrupts_restore(saved);
    if (index == 0)
        fb_console_resize(d);
    klog_info("display: %u is now %ux%u (framebuffer at 0x%lx, pitch %u)", index, width, height, phys, pitch);
    return STATUS_SUCCESS;
}

/* --- Graphics drivers ------------------------------------------------------------------- */

status_t display_set_driver(uint32_t index, const display_ops_t *ops, void *driver_data, uint64_t second_phys)
{
    display_t *d = display_get(index);

    if (!d)
        return STATUS_NOT_FOUND;
    if (d->acquired)
        return STATUS_BUSY;
    d->ops = ops;
    d->driver_data = driver_data;
    d->second_phys = ops && ops->flip ? second_phys : 0;
    d->info.flags &= ~(JELLY_DISPLAY_CURSOR | JELLY_DISPLAY_VBLANK | JELLY_DISPLAY_FLIP);
    if (ops && ops->cursor_image && ops->cursor_move)
        d->info.flags |= JELLY_DISPLAY_CURSOR;
    if (ops && ops->wait_vblank)
        d->info.flags |= JELLY_DISPLAY_VBLANK;
    if (d->second_phys)
        d->info.flags |= JELLY_DISPLAY_FLIP;
    klog_info("display: %u: driver provides%s%s%s%s", index,
              (d->info.flags & JELLY_DISPLAY_CURSOR) ? " a hardware pointer" : "",
              (d->info.flags & JELLY_DISPLAY_VBLANK) ? " vertical blank timing" : "",
              (d->info.flags & JELLY_DISPLAY_FLIP) ? " page flipping" : "",
              (d->info.flags & (JELLY_DISPLAY_CURSOR | JELLY_DISPLAY_VBLANK | JELLY_DISPLAY_FLIP)) ? "" : " nothing");
    return STATUS_SUCCESS;
}

status_t display_cursor(uint32_t index, const jelly_cursor_t *cursor, const uint32_t *pixels)
{
    display_t *d = display_get(index);

    if (!d)
        return STATUS_NOT_FOUND;
    if (!(d->info.flags & JELLY_DISPLAY_CURSOR))
        return STATUS_NOT_SUPPORTED;
    if (cursor->flags & JELLY_CURSOR_IMAGE) {
        status_t status = pixels ? d->ops->cursor_image(d, pixels) : STATUS_INVALID_ARGUMENT;
        if (STATUS_IS_ERROR(status))
            return status;
    }
    d->ops->cursor_move(d, cursor->x, cursor->y, cursor->flags & JELLY_CURSOR_VISIBLE);
    return STATUS_SUCCESS;
}

status_t display_wait_vblank(uint32_t index, uint64_t timeout_ns)
{
    display_t *d = display_get(index);

    if (!d)
        return STATUS_NOT_FOUND;
    if (!(d->info.flags & JELLY_DISPLAY_VBLANK))
        return STATUS_NOT_SUPPORTED;
    return d->ops->wait_vblank(d, timeout_ns);
}

status_t display_flip(uint32_t index, uint32_t buffer)
{
    display_t *d = display_get(index);

    if (!d)
        return STATUS_NOT_FOUND;
    if (!(d->info.flags & JELLY_DISPLAY_FLIP))
        return STATUS_NOT_SUPPORTED;
    if (buffer > 1)
        return STATUS_INVALID_ARGUMENT;
    return d->ops->flip(d, buffer);
}

static void buffer_released(void *context)
{
    (void)context; /* the framebuffer itself decides when the display is free again */
}

status_t display_buffer(uint32_t index, uint32_t buffer, object_t **memory)
{
    display_t *d = display_get(index);

    if (!d)
        return STATUS_NOT_FOUND;
    if (!(d->info.flags & JELLY_DISPLAY_FLIP))
        return STATUS_NOT_SUPPORTED;
    if (buffer != 1)
        return STATUS_INVALID_ARGUMENT; /* buffer 0 is what SYS_DISPLAY_ACQUIRE hands out */
    return shm_create_device(d->second_phys, d->info.size, buffer_released, d, memory);
}

static void release(void *context)
{
    display_t *d = context;
    /* Back to what the kernel console draws on, without a pointer on top. */
    if (d->info.flags & JELLY_DISPLAY_FLIP)
        d->ops->flip(d, 0);
    if (d->info.flags & JELLY_DISPLAY_CURSOR)
        d->ops->cursor_move(d, 0, 0, false);
    d->acquired = false;
    d->info.flags &= ~JELLY_DISPLAY_ACQUIRED;
    klog_info("display: %u released", d->info.index);
    if (d->info.index == 0)
        fb_console_set_active(true);
}

status_t display_acquire(uint32_t index, object_t **memory)
{
    display_t *d = display_get(index);
    if (!d)
        return STATUS_NOT_FOUND;
    if (d->acquired)
        return STATUS_BUSY;
    status_t status = shm_create_device(d->phys, d->info.size, release, d, memory);
    if (STATUS_IS_ERROR(status))
        return status;
    d->acquired = true;
    d->info.flags |= JELLY_DISPLAY_ACQUIRED;
    if (index == 0)
        fb_console_set_active(false);
    klog_info("display: %u acquired by a display server", index);
    return STATUS_SUCCESS;
}
