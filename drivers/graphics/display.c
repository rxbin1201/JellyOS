/*
 * Display registry and the UEFI GOP framebuffer (README section 33: "Start
 * with the UEFI framebuffer"), and what graphics drivers add to a display.
 * See display.h.
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

#define DRIVER_FLAGS (JELLY_DISPLAY_CURSOR | JELLY_DISPLAY_VBLANK | JELLY_DISPLAY_FLIP | JELLY_DISPLAY_MODES)

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

void display_lock(display_t *d)
{
    mutex_lock(&d->lock);
}

void display_unlock(display_t *d)
{
    mutex_unlock(&d->lock);
}

/* Something about the display changed that its users must look at again. */
static void changed(display_t *d)
{
    d->info.generation++;
    if (d->changed)
        event_signal(d->changed);
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
    d->current_mode = DISPLAY_NO_MODE;
    mutex_init(&d->lock);
    count++;
    klog_info("display: %ux%u framebuffer at 0x%lx (GOP, pitch %u)", fb->width, fb->height, fb->phys_base, fb->pitch);

    /* fbconsole=0 keeps the console on the serial port only. */
    char setting[8];
    if (!cmdline_value("fbconsole", setting, sizeof(setting)) || strcmp(setting, "0") != 0)
        fb_console_init(d);
}

status_t display_set_framebuffer(uint32_t index, uint64_t phys, uint64_t size, uint32_t width, uint32_t height,
                                 uint32_t pitch)
{
    display_t *d = display_get(index);

    if (!d)
        return STATUS_NOT_FOUND;
    if (d->acquired)
        return STATUS_BUSY;
    uint64_t needed = align_up((uint64_t)pitch * height, PAGE_SIZE);
    size = size ? align_up(size, PAGE_SIZE) : needed;
    if (!width || !height || pitch < width * 4 || (phys & (PAGE_SIZE - 1)) || size < needed)
        return STATUS_INVALID_ARGUMENT;
    volatile uint32_t *pixels = (volatile uint32_t *)vmm_map_mmio(phys, size, VM_WRITE_COMBINING);
    if (!pixels)
        return STATUS_OUT_OF_MEMORY;

    /*
     * The console draws from interrupt handlers, too, with a text grid laid out for the old size: it stops
     * drawing first, and fb_console_resize() switches it to the new size in one step.
     */
    mutex_lock(&d->lock);
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
    changed(d);
    mutex_unlock(&d->lock);
    klog_info("display: %u is now %ux%u (framebuffer at 0x%lx, pitch %u)", index, width, height, phys, pitch);
    return STATUS_SUCCESS;
}

/* --- Graphics drivers ------------------------------------------------------------------- */

/* The capability flags follow from what the driver brought. */
static void update_flags(display_t *d)
{
    const display_ops_t *ops = d->ops;
    uint32_t flags = d->info.flags & ~DRIVER_FLAGS;

    if (ops && ops->cursor_image && ops->cursor_move)
        flags |= JELLY_DISPLAY_CURSOR;
    if (ops && ops->wait_vblank)
        flags |= JELLY_DISPLAY_VBLANK;
    if (ops && ops->flip && d->second_phys)
        flags |= JELLY_DISPLAY_FLIP;
    if (ops && ops->set_mode && d->mode_count)
        flags |= JELLY_DISPLAY_MODES;
    d->info.flags = flags;
}

status_t display_set_driver(uint32_t index, const display_ops_t *ops, void *driver_data, uint64_t second_phys)
{
    display_t *d = display_get(index);

    if (!d)
        return STATUS_NOT_FOUND;
    if (d->acquired)
        return STATUS_BUSY;
    mutex_lock(&d->lock);
    d->ops = ops;
    d->driver_data = driver_data;
    d->second_phys = ops && ops->flip ? second_phys : 0;
    if (!ops) {
        d->mode_count = 0;
        d->current_mode = DISPLAY_NO_MODE;
        d->info.refresh_mhz = 0;
    }
    update_flags(d);
    changed(d);
    mutex_unlock(&d->lock);
    klog_info("display: %u: driver provides%s%s%s%s%s", index,
              (d->info.flags & JELLY_DISPLAY_CURSOR) ? " a hardware pointer" : "",
              (d->info.flags & JELLY_DISPLAY_VBLANK) ? " vertical blank timing" : "",
              (d->info.flags & JELLY_DISPLAY_FLIP) ? " page flipping" : "",
              (d->info.flags & JELLY_DISPLAY_MODES) ? " mode switching" : "",
              (d->info.flags & DRIVER_FLAGS) ? "" : " nothing");
    return STATUS_SUCCESS;
}

status_t display_set_modes(uint32_t index, const jelly_display_mode_t *modes, uint32_t mode_count, uint32_t current)
{
    display_t *d = display_get(index);

    if (!d)
        return STATUS_NOT_FOUND;
    if (mode_count > JELLY_DISPLAY_MODE_MAX || (current != DISPLAY_NO_MODE && current >= mode_count))
        return STATUS_INVALID_ARGUMENT;
    mutex_lock(&d->lock);
    for (uint32_t i = 0; i < mode_count; i++) {
        d->modes[i] = modes[i];
        d->modes[i].flags = (modes[i].flags & JELLY_MODE_PREFERRED) | (i == current ? JELLY_MODE_CURRENT : 0);
    }
    d->mode_count = mode_count;
    d->current_mode = current;
    if (current != DISPLAY_NO_MODE)
        d->info.refresh_mhz = modes[current].refresh_mhz;
    update_flags(d);
    changed(d);
    mutex_unlock(&d->lock);
    klog_info("display: %u: %u mode%s%s", index, mode_count, mode_count == 1 ? "" : "s",
              (d->info.flags & JELLY_DISPLAY_MODES) ? ", switchable" : "");
    return STATUS_SUCCESS;
}

void display_set_connected(uint32_t index, bool connected)
{
    display_t *d = display_get(index);

    if (!d || connected == !(d->info.flags & JELLY_DISPLAY_DISCONNECTED))
        return;
    mutex_lock(&d->lock);
    if (connected)
        d->info.flags &= ~JELLY_DISPLAY_DISCONNECTED;
    else
        d->info.flags |= JELLY_DISPLAY_DISCONNECTED;
    changed(d);
    mutex_unlock(&d->lock);
    klog_info("display: %u: monitor %s", index, connected ? "connected" : "disconnected");
}

status_t display_cursor(uint32_t index, const jelly_cursor_t *cursor, const uint32_t *pixels)
{
    display_t *d = display_get(index);
    status_t status = STATUS_SUCCESS;

    if (!d)
        return STATUS_NOT_FOUND;
    if (!(d->info.flags & JELLY_DISPLAY_CURSOR))
        return STATUS_NOT_SUPPORTED;
    mutex_lock(&d->lock);
    if (cursor->flags & JELLY_CURSOR_IMAGE)
        status = pixels ? d->ops->cursor_image(d, pixels) : STATUS_INVALID_ARGUMENT;
    if (!STATUS_IS_ERROR(status))
        d->ops->cursor_move(d, cursor->x, cursor->y, cursor->flags & JELLY_CURSOR_VISIBLE);
    mutex_unlock(&d->lock);
    return status;
}

status_t display_wait_vblank(uint32_t index, uint64_t timeout_ns)
{
    display_t *d = display_get(index);

    if (!d)
        return STATUS_NOT_FOUND;
    if (!(d->info.flags & JELLY_DISPLAY_VBLANK))
        return STATUS_NOT_SUPPORTED;
    return d->ops->wait_vblank(d, timeout_ns); /* without the lock: it sleeps */
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
    mutex_lock(&d->lock);
    status_t status = d->ops->flip(d, buffer);
    mutex_unlock(&d->lock);
    return status;
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

/* --- Modes ---------------------------------------------------------------------------- */

status_t display_modes(uint32_t index, jelly_display_mode_t *modes, uint32_t max, uint32_t *mode_count)
{
    display_t *d = display_get(index);

    if (!d)
        return STATUS_NOT_FOUND;
    mutex_lock(&d->lock);
    if (d->mode_count == 0) {
        /* No driver that knows modes: the one the firmware set up. */
        if (max)
            modes[0] = (jelly_display_mode_t){ d->info.width, d->info.height, d->info.refresh_mhz,
                                               JELLY_MODE_CURRENT | JELLY_MODE_PREFERRED };
        *mode_count = 1;
    } else {
        for (uint32_t i = 0; i < d->mode_count && i < max; i++)
            modes[i] = d->modes[i];
        *mode_count = d->mode_count;
    }
    mutex_unlock(&d->lock);
    return STATUS_SUCCESS;
}

status_t display_set_mode(uint32_t index, uint32_t mode)
{
    display_t *d = display_get(index);
    uint32_t pitch = 0;

    if (!d)
        return STATUS_NOT_FOUND;
    if (!(d->info.flags & JELLY_DISPLAY_MODES))
        return STATUS_NOT_SUPPORTED;
    mutex_lock(&d->lock);
    if (mode >= d->mode_count) {
        mutex_unlock(&d->lock);
        return STATUS_INVALID_ARGUMENT;
    }
    if (mode == d->current_mode) {
        mutex_unlock(&d->lock);
        return STATUS_SUCCESS;
    }
    const jelly_display_mode_t *m = &d->modes[mode];
    /* The console keeps off the screen while its size is in the air; a display server is told afterwards. */
    bool console = index == 0 && !d->acquired;
    if (console)
        fb_console_set_active(false);
    status_t status = d->ops->set_mode(d, mode, &pitch);
    if (!STATUS_IS_ERROR(status) && (pitch < m->width * 4 || (uint64_t)pitch * m->height > d->info.size))
        status = STATUS_DEVICE_ERROR; /* the driver's framebuffer does not hold this mode */
    if (!STATUS_IS_ERROR(status)) {
        uint64_t saved = arch_interrupts_save();
        d->info.width = m->width;
        d->info.height = m->height;
        d->info.pitch = pitch;
        d->info.refresh_mhz = m->refresh_mhz;
        arch_interrupts_restore(saved);
        if (d->current_mode != DISPLAY_NO_MODE)
            d->modes[d->current_mode].flags &= ~JELLY_MODE_CURRENT;
        d->modes[mode].flags |= JELLY_MODE_CURRENT;
        d->current_mode = mode;
    }
    if (console)
        fb_console_resize(d); /* also after a failure: the driver put the old mode back, the screen is empty */
    else
        d->console_stale = true;
    changed(d);
    mutex_unlock(&d->lock);
    if (STATUS_IS_ERROR(status))
        klog_warn("display: %u: mode %ux%u does not come up (%s)", index, m->width, m->height, status_name(status));
    else
        klog_info("display: %u is now %ux%u at %u.%02u Hz", index, d->info.width, d->info.height,
                  d->info.refresh_mhz / 1000, d->info.refresh_mhz % 1000 / 10);
    return status;
}

status_t display_watch(uint32_t index, object_t **event)
{
    display_t *d = display_get(index);
    status_t status = STATUS_SUCCESS;

    if (!d)
        return STATUS_NOT_FOUND;
    mutex_lock(&d->lock);
    if (!d->changed)
        status = event_create(0, &d->changed);
    if (!STATUS_IS_ERROR(status)) {
        object_retain(d->changed);
        *event = d->changed;
    }
    mutex_unlock(&d->lock);
    return status;
}

/* --- Ownership ------------------------------------------------------------------------ */

static void release(void *context)
{
    display_t *d = context;

    mutex_lock(&d->lock);
    /* Back to what the kernel console draws on, without a pointer on top. */
    if (d->info.flags & JELLY_DISPLAY_FLIP)
        d->ops->flip(d, 0);
    if (d->info.flags & JELLY_DISPLAY_CURSOR)
        d->ops->cursor_move(d, 0, 0, false);
    d->acquired = false;
    d->info.flags &= ~JELLY_DISPLAY_ACQUIRED;
    bool stale = d->console_stale;
    d->console_stale = false;
    mutex_unlock(&d->lock);
    klog_info("display: %u released", d->info.index);
    if (d->info.index != 0)
        return;
    if (stale)
        fb_console_resize(d); /* the mode changed meanwhile: the text grid no longer fits */
    else
        fb_console_set_active(true);
}

status_t display_acquire(uint32_t index, object_t **memory)
{
    display_t *d = display_get(index);
    if (!d)
        return STATUS_NOT_FOUND;
    mutex_lock(&d->lock);
    status_t status = d->acquired ? STATUS_BUSY : shm_create_device(d->phys, d->info.size, release, d, memory);
    if (!STATUS_IS_ERROR(status)) {
        d->acquired = true;
        d->info.flags |= JELLY_DISPLAY_ACQUIRED;
        if (index == 0)
            fb_console_set_active(false);
    }
    mutex_unlock(&d->lock);
    if (!STATUS_IS_ERROR(status))
        klog_info("display: %u acquired by a display server", index);
    return status;
}
