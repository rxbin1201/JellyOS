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

static void release(void *context)
{
    display_t *d = context;
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
