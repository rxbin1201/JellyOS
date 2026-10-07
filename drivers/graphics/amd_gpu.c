/*
 * AMD integrated graphics with the display engine "DCN 2.1": Ryzen 4000 and
 * 5000 processors with Radeon Graphics (Renoir, Lucienne, Cezanne, Barcelo;
 * for example the Ryzen 5 5600G), display part.
 *
 * The UEFI firmware has lit the screen and JellyOS shows its framebuffer.
 * This driver looks at how the firmware set the display engine up and, with
 * "amdgpu=on" on the kernel command line, adds what the engine can do
 * without touching the mode:
 *
 *   - a hardware pointer (the cursor of the pipe's HUBP and DPP, 64x64 ARGB)
 *   - waiting for the vertical blank (the frame counter of the timing
 *     generator is polled every millisecond: interrupts of this GPU come
 *     through a ring buffer that is not set up here)
 *   - a second framebuffer and flipping between the two (the surface address
 *     of the HUBP, taken over by the hardware at the next frame)
 *
 * All of it goes through the display layer's driver interface
 * (drivers/graphics/display.h); nothing above this file knows the GPU.
 *
 * How the pieces of the engine belong together (one "pipe"):
 *
 *   HUBP n  reads the framebuffer from memory    ─┐
 *   DPP n   converts and scales, adds the cursor  ├─ found through the HUBP's
 *   OTG m   makes the timing (sync, blanking)    ─┘  choice of timing generator
 *
 * Memory: the framebuffer lies in the GPU's video memory, which the CPU
 * sees through BAR 0. The GPU addresses it from DCN_VM_FB_LOCATION_BASE on.
 * The second framebuffer and the pointer image are placed behind the
 * firmware's framebuffer in the same memory, which nobody else uses without
 * a 3D driver.
 *
 * Without the option nothing is changed: the driver only reports (dmesg
 * amdgpu), including the raw timing registers that a later mode switch
 * needs.
 *
 * Register information from Linux's amdgpu (dcn_2_1_0_offset.h,
 * dcn20_hubp.c, dcn10_dpp.c). QEMU has no such device: this driver can only
 * be tested on real hardware.
 *
 * Not yet: switching modes, reading the monitor's EDID, hot plug, several
 * screens, any kind of acceleration.
 */

#include "drivers/bus/pci/pci.h"
#include "drivers/core/module.h"
#include "drivers/graphics/display.h"

#include "core/cmdline.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/layout.h"
#include "memory/vmm.h"
#include "scheduler/thread.h"
#include "time/clock.h"

/* --- Registers (byte offsets in BAR 5) ------------------------------------------------- */

#define PIPES                 4

/* Clock generator: one pixel rate control per timing generator */
#define PIXEL_RATE_CNTL(o)    (0x00500 + 0x10u * (uint32_t)(o)) /* bits 1:0 source, bit 4 DisplayPort DTO on */
#define DP_DTO_PHASE(o)       (0x00504 + 0x10u * (uint32_t)(o)) /* pixel clock = reference * phase / modulo */
#define DP_DTO_MODULO(o)      (0x00508 + 0x10u * (uint32_t)(o))

/* Where the GPU sees its video memory (units of 16 MiB) */
#define VM_FB_LOCATION_BASE   0x0E54C
#define VM_FB_LOCATION_TOP    0x0E550

/* HUBP n: the plane that reads the framebuffer */
#define HUBP(n)               (0x370u * (uint32_t)(n))
#define SURFACE_CONFIG(n)     (0x0EA94 + HUBP(n)) /* bits 6:0 pixel format (8: ARGB 8888), 9:8 rotation */
#define VIEWPORT_START(n)     (0x0EAA4 + HUBP(n))
#define VIEWPORT_DIMENSION(n) (0x0EAA8 + HUBP(n)) /* height << 16 | width */
#define HUBP_CNTL(n)          (0x0EACC + HUBP(n)) /* bit 0 blanked, bit 2 off, bits 7:4 timing generator */
#define SURFACE_PITCH(n)      (0x0EB1C + HUBP(n)) /* pixels per line */
#define SURFACE_ADDRESS(n)    (0x0EB28 + HUBP(n)) /* low half; writing it arms the flip: write the high half first */
#define SURFACE_ADDRESS_HIGH(n) (0x0EB2C + HUBP(n))
#define FLIP_CONTROL(n)       (0x0EB6C + HUBP(n)) /* bit 1: flip at once instead of at the next frame; bit 8: pending */
#define CURSOR_SETTINGS(n)    (0x0EC78 + HUBP(n))
#define CURSOR_CONTROL(n)     (0x0ECE0 + HUBP(n)) /* bit 0 on, 10:8 mode, 17:16 pitch, 28:24 lines per request */
#define CURSOR_ADDRESS(n)     (0x0ECE4 + HUBP(n))
#define CURSOR_ADDRESS_HIGH(n) (0x0ECE8 + HUBP(n))
#define CURSOR_SIZE(n)        (0x0ECEC + HUBP(n)) /* width << 16 | height */
#define CURSOR_POSITION(n)    (0x0ECF0 + HUBP(n)) /* x << 16 | y: where the hot spot is on the screen */
#define CURSOR_HOT_SPOT(n)    (0x0ECF4 + HUBP(n)) /* x << 16 | y: the hot spot within the image */
#define CURSOR_DST_OFFSET(n)  (0x0ECFC + HUBP(n))

#define HUBP_BLANK            (1u << 0)
#define HUBP_OFF              (1u << 2)
#define FLIP_IMMEDIATE        (1u << 1)
#define FLIP_PENDING          (1u << 8)
#define CURSOR_ON             (1u << 0)
#define CURSOR_ARGB           (3u << 8)  /* 32 bits per pixel, alpha not premultiplied */
#define CURSOR_PITCH_64       (0u << 16)
#define CURSOR_LINES_8        (3u << 24) /* lines fetched per request, for images up to 64 pixels wide */

/* DPP n: the cursor is mixed into the picture here */
#define DPP_CURSOR_CONTROL(n) (0x10680 + 0x5ACu * (uint32_t)(n)) /* bit 0 on, bits 6:4 mode */

/* OTG m: the timing generator */
#define OTG(m)                (0x200u * (uint32_t)(m))
#define OTG_H_TOTAL(m)        (0x13FA8 + OTG(m)) /* total - 1 */
#define OTG_H_BLANK(m)        (0x13FAC + OTG(m)) /* end << 16 | start */
#define OTG_H_SYNC(m)         (0x13FB0 + OTG(m))
#define OTG_V_TOTAL(m)        (0x13FBC + OTG(m))
#define OTG_V_BLANK(m)        (0x13FD8 + OTG(m))
#define OTG_V_SYNC(m)         (0x13FDC + OTG(m))
#define OTG_CONTROL(m)        (0x14004 + OTG(m)) /* bit 0 on, bit 16 running */
#define OTG_FRAME_COUNT(m)    (0x14030 + OTG(m))

#define OTG_RUNNING           (1u << 16)

/* Encoders: which connection the timing generator feeds */
#define DIG_FE_CNTL(i)        (0x154A0 + 0x400u * (uint32_t)(i)) /* bits 2:0 timing generator, bit 10 started */
#define DIG_BE_CNTL(i)        (0x155BC + 0x400u * (uint32_t)(i)) /* bits 18:16 mode */
#define DIG_BE_EN_CNTL(i)     (0x155C0 + 0x400u * (uint32_t)(i))
#define DP_LINK_CNTL(i)       (0x15720 + 0x400u * (uint32_t)(i))
#define DP_VID_STREAM_CNTL(i) (0x15730 + 0x400u * (uint32_t)(i))
#define DP_VID_TIMING(i)      (0x15740 + 0x400u * (uint32_t)(i))
#define DP_VID_N(i)           (0x15744 + 0x400u * (uint32_t)(i))
#define DP_VID_M(i)           (0x15748 + 0x400u * (uint32_t)(i))
#define DIG_COUNT             5

typedef struct {
    pci_device_t     *pci;
    volatile uint8_t *regs;
    uint32_t          regs_size;
    uint64_t          aperture, aperture_size; /* BAR 0: the video memory as the CPU sees it */
    uint64_t          vram_base, vram_size;    /* the same memory as the GPU addresses it */

    int               hubp, otg;               /* the pipe that shows the boot framebuffer */
    uint64_t          framebuffers[2];         /* offsets in the video memory */
    uint64_t          cursor_offset;
    volatile uint32_t *cursor;                 /* the pointer image, mapped */
    bool              flip_pending;
    uint32_t          refresh_mhz;             /* from the pixel clock, 0 if that is not known */
} amdgpu_t;

static uint32_t rd(amdgpu_t *g, uint32_t reg)
{
    return *(volatile uint32_t *)(g->regs + reg);
}

static void wr(amdgpu_t *g, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(g->regs + reg) = value;
}

static void sleep_ms(uint32_t ms)
{
    thread_sleep((uint64_t)ms * 1000000);
}

/* --- Driver operations for the display layer ------------------------------------------ */

static amdgpu_t *amdgpu_of(display_t *display)
{
    return display->driver_data;
}

static status_t amdgpu_wait_vblank(display_t *display, uint64_t timeout_ns)
{
    amdgpu_t *g = amdgpu_of(display);
    uint64_t end = clock_monotonic_ns() + timeout_ns;
    uint32_t frame = rd(g, OTG_FRAME_COUNT(g->otg));

    /* The next frame; after a flip, also until the hardware has taken the new address. */
    while (rd(g, OTG_FRAME_COUNT(g->otg)) == frame ||
           (g->flip_pending && (rd(g, FLIP_CONTROL(g->hubp)) & FLIP_PENDING))) {
        if (clock_monotonic_ns() >= end)
            return STATUS_TIMEOUT;
        sleep_ms(1);
    }
    g->flip_pending = false;
    return STATUS_SUCCESS;
}

static void surface_show(amdgpu_t *g, uint64_t offset)
{
    uint64_t address = g->vram_base + offset;

    wr(g, FLIP_CONTROL(g->hubp), rd(g, FLIP_CONTROL(g->hubp)) & ~FLIP_IMMEDIATE); /* at the next frame: no tearing */
    wr(g, SURFACE_ADDRESS_HIGH(g->hubp), (uint32_t)(address >> 32));
    wr(g, SURFACE_ADDRESS(g->hubp), (uint32_t)address);
}

static status_t amdgpu_flip(display_t *display, uint32_t buffer)
{
    amdgpu_t *g = amdgpu_of(display);

    surface_show(g, g->framebuffers[buffer]);
    g->flip_pending = true;
    return STATUS_SUCCESS;
}

static status_t amdgpu_cursor_image(display_t *display, const uint32_t *pixels)
{
    amdgpu_t *g = amdgpu_of(display);
    uint64_t address = g->vram_base + g->cursor_offset;

    for (uint32_t i = 0; i < JELLY_CURSOR_SIZE * JELLY_CURSOR_SIZE; i++)
        g->cursor[i] = pixels[i];
    __asm__ volatile("sfence" : : : "memory"); /* the mapping is write-combining: out of the CPU's buffers */
    wr(g, CURSOR_ADDRESS_HIGH(g->hubp), (uint32_t)(address >> 32));
    wr(g, CURSOR_ADDRESS(g->hubp), (uint32_t)address);
    wr(g, CURSOR_SIZE(g->hubp), JELLY_CURSOR_SIZE << 16 | JELLY_CURSOR_SIZE);
    wr(g, CURSOR_SETTINGS(g->hubp), 3u << 8); /* as Linux: no line offset, request deadline adjusted by 3 */
    wr(g, CURSOR_CONTROL(g->hubp), (rd(g, CURSOR_CONTROL(g->hubp)) & CURSOR_ON) | CURSOR_ARGB | CURSOR_PITCH_64 |
                                       CURSOR_LINES_8);
    wr(g, DPP_CURSOR_CONTROL(g->hubp), (rd(g, DPP_CURSOR_CONTROL(g->hubp)) & 1u) | 3u << 4);
    return STATUS_SUCCESS;
}

static void amdgpu_cursor_move(display_t *display, int32_t x, int32_t y, bool visible)
{
    amdgpu_t *g = amdgpu_of(display);
    int32_t size = JELLY_CURSOR_SIZE;

    /* The registers hold the hot spot's place on the screen; an image hanging over the left or top edge
     * is shown by moving the hot spot into the image instead. */
    if (x <= -size || y <= -size || x >= (int32_t)display->info.width || y >= (int32_t)display->info.height)
        visible = false;
    uint32_t hot_x = x < 0 ? (uint32_t)-x : 0, hot_y = y < 0 ? (uint32_t)-y : 0;
    uint32_t at_x = x < 0 ? 0 : (uint32_t)x, at_y = y < 0 ? 0 : (uint32_t)y;

    if (visible) {
        wr(g, CURSOR_POSITION(g->hubp), at_x << 16 | at_y);
        wr(g, CURSOR_HOT_SPOT(g->hubp), hot_x << 16 | hot_y);
        wr(g, CURSOR_DST_OFFSET(g->hubp), 0);
    }
    uint32_t control = rd(g, CURSOR_CONTROL(g->hubp)), mixer = rd(g, DPP_CURSOR_CONTROL(g->hubp));
    if (visible != !!(control & CURSOR_ON))
        wr(g, CURSOR_CONTROL(g->hubp), visible ? control | CURSOR_ON : control & ~CURSOR_ON);
    if (visible != !!(mixer & 1u))
        wr(g, DPP_CURSOR_CONTROL(g->hubp), visible ? mixer | 1u : mixer & ~1u);
}

static display_ops_t amdgpu_ops; /* filled with what works on this machine */

/* --- Reading the firmware's state ------------------------------------------------------ */

static const char *connection_name(amdgpu_t *g)
{
    static const char *const modes[8] = { "DisplayPort", "LVDS", "DVI", "HDMI", "?", "DisplayPort (MST)", "?", "?" };

    for (int i = 0; i < DIG_COUNT; i++) {
        uint32_t front = rd(g, DIG_FE_CNTL(i)), back = rd(g, DIG_BE_CNTL(i));
        /* The back end names its front ends as a bit mask; a back end that is on has a mode and a front end. */
        if ((front & 7) == (uint32_t)g->otg && ((back >> 8) & 0x7F & (1u << i)) && (rd(g, DIG_BE_EN_CNTL(i)) & 1u))
            return modes[(back >> 16) & 7];
    }
    return "unknown connection";
}

/* Find the pipe that shows the boot framebuffer. False if the firmware's setup is not what this driver expects. */
static bool read_state(amdgpu_t *g, display_t *d)
{
    g->vram_base = (uint64_t)(rd(g, VM_FB_LOCATION_BASE) & 0xFFFFFF) << 24;
    uint64_t top = ((uint64_t)(rd(g, VM_FB_LOCATION_TOP) & 0xFFFFFF) << 24) + (1ull << 24);
    g->vram_size = top > g->vram_base ? top - g->vram_base : 0;
    klog_info("amdgpu: video memory: %lu MiB, for the GPU at 0x%lx, for the CPU at 0x%lx (%lu MiB visible)",
              g->vram_size >> 20, g->vram_base, g->aperture, g->aperture_size >> 20);

    g->hubp = -1;
    for (int n = 0; n < PIPES; n++) {
        uint32_t control = rd(g, HUBP_CNTL(n));
        uint64_t address = (uint64_t)rd(g, SURFACE_ADDRESS_HIGH(n)) << 32 | rd(g, SURFACE_ADDRESS(n));
        uint32_t timing = (control >> 4) & 0xF, size = rd(g, VIEWPORT_DIMENSION(n));

        if ((control & (HUBP_OFF | HUBP_BLANK)) || control == 0xFFFFFFFF || timing >= PIPES)
            continue;
        bool running = rd(g, OTG_CONTROL(timing)) & OTG_RUNNING;
        klog_info("amdgpu: plane %d: %ux%u from 0x%lx (pitch %u, format %u), timing generator %u %s", n, size & 0x3FFF,
                  (size >> 16) & 0x3FFF, address, rd(g, SURFACE_PITCH(n)) & 0x3FFF, rd(g, SURFACE_CONFIG(n)) & 0x7F,
                  timing, running ? "running" : "off");
        if (running && g->hubp < 0 && d->phys >= g->aperture &&
            address == g->vram_base + (d->phys - g->aperture)) {
            g->hubp = n;
            g->otg = (int)timing;
        }
    }
    if (g->hubp < 0) {
        klog_warn("amdgpu: no plane shows the boot framebuffer (at 0x%lx for the CPU): nothing to take over", d->phys);
        return false;
    }

    int m = g->otg;
    uint32_t h_total = rd(g, OTG_H_TOTAL(m)) + 1, v_total = rd(g, OTG_V_TOTAL(m)) + 1;
    uint32_t h_blank = rd(g, OTG_H_BLANK(m)), v_blank = rd(g, OTG_V_BLANK(m));
    uint32_t h_active = (h_blank & 0x7FFF) - (h_blank >> 16), v_active = (v_blank & 0x7FFF) - (v_blank >> 16);
    klog_info("amdgpu: plane %d shows the boot framebuffer; timing generator %d, %s", g->hubp, m, connection_name(g));
    klog_info("amdgpu: timing %ux%u (total %ux%u), sync 0x%x 0x%x, blank 0x%x 0x%x", h_active, v_active, h_total,
              v_total, rd(g, OTG_H_SYNC(m)), rd(g, OTG_V_SYNC(m)), h_blank, v_blank);
    klog_info("amdgpu: pixel rate control 0x%x, DTO phase %u, modulo %u", rd(g, PIXEL_RATE_CNTL(m)),
              rd(g, DP_DTO_PHASE(m)), rd(g, DP_DTO_MODULO(m)));
    /* With the DisplayPort DTO the phase is the pixel clock in Hz: the refresh rate follows exactly. */
    if ((rd(g, PIXEL_RATE_CNTL(m)) & (1u << 4)) && h_total > 1 && v_total > 1)
        g->refresh_mhz = (uint32_t)((uint64_t)rd(g, DP_DTO_PHASE(m)) * 1000 / ((uint64_t)h_total * v_total));
    /* The encoders as the firmware left them (for the mode switching still to come). */
    for (int i = 0; i < DIG_COUNT; i++) {
        uint32_t front = rd(g, DIG_FE_CNTL(i)), back = rd(g, DIG_BE_CNTL(i)), on = rd(g, DIG_BE_EN_CNTL(i));
        if (on != 0xFFFFFFFF && (on & 1u))
            klog_info("amdgpu: encoder %d: front 0x%x, back 0x%x (on 0x%x), link 0x%x, stream 0x%x, video timing 0x%x, "
                      "N %u, M %u", i, front, back, on, rd(g, DP_LINK_CNTL(i)), rd(g, DP_VID_STREAM_CNTL(i)),
                      rd(g, DP_VID_TIMING(i)), rd(g, DP_VID_N(i)), rd(g, DP_VID_M(i)));
    }

    /* What the pointer and the second framebuffer rely on. */
    uint32_t view = rd(g, VIEWPORT_DIMENSION(g->hubp)), start = rd(g, VIEWPORT_START(g->hubp));
    if ((view & 0x3FFF) != d->info.width || ((view >> 16) & 0x3FFF) != d->info.height || start != 0) {
        klog_warn("amdgpu: the firmware scales or crops the picture (plane %ux%u at 0x%x, screen %ux%u): not touched",
                  view & 0x3FFF, (view >> 16) & 0x3FFF, start, h_active, v_active);
        return false;
    }
    if ((rd(g, SURFACE_CONFIG(g->hubp)) & 0x37F) != 8) {
        klog_warn("amdgpu: the plane is not unrotated 32-bit RGB (configuration 0x%x): not touched",
                  rd(g, SURFACE_CONFIG(g->hubp)));
        return false;
    }
    return true;
}

/* Frames per 1000 seconds, measured: the pixel clock is not known without the firmware's tables. */
static uint32_t measure_refresh(amdgpu_t *g)
{
    uint32_t before = rd(g, OTG_FRAME_COUNT(g->otg));
    uint64_t start = clock_monotonic_ns();

    sleep_ms(500);
    uint32_t frames = rd(g, OTG_FRAME_COUNT(g->otg)) - before;
    uint64_t ns = clock_monotonic_ns() - start;
    return ns ? (uint32_t)((uint64_t)frames * 1000000000000ull / ns) : 0;
}

/* --- Start ----------------------------------------------------------------------------- */

static bool supported(uint16_t device)
{
    return device == 0x1636 || device == 0x1638 || device == 0x164C || device == 0x15E7;
}

static status_t amdgpu_probe(device_t *device)
{
    static amdgpu_t gpu; /* one integrated GPU per machine */
    amdgpu_t *g = &gpu;
    pci_device_t *pci = pci_from_device(device);
    display_t *d = display_get(0);
    char option[16];

    if (!supported(device->id.device)) {
        klog_info("amdgpu: AMD graphics 1002:%04x has no driver here: left to the firmware's framebuffer",
                  device->id.device);
        return STATUS_NOT_SUPPORTED;
    }
    if (g->regs || !d || !pci->bars[0].phys || !pci->bars[5].phys || pci->bars[5].io)
        return STATUS_NOT_SUPPORTED;
    g->pci = pci;
    status_t status = pci_enable_device(pci, false);
    if (STATUS_IS_ERROR(status))
        return status;
    g->regs_size = (uint32_t)pci->bars[5].size;
    if (g->regs_size < 0x20000) {
        klog_warn("amdgpu: the register BAR has only %u KiB", g->regs_size >> 10);
        return STATUS_NOT_SUPPORTED;
    }
    g->regs = (volatile uint8_t *)vmm_map_mmio(pci->bars[5].phys, 0x20000, VM_UNCACHED);
    if (!g->regs)
        return STATUS_OUT_OF_MEMORY;
    g->aperture = pci->bars[0].phys;
    g->aperture_size = pci->bars[0].size;
    klog_info("amdgpu: AMD graphics 1002:%04x (display engine DCN 2.1), registers at 0x%lx", device->id.device,
              pci->bars[5].phys);

    if (!read_state(g, d)) {
        klog_info("amdgpu: the display stays as the firmware set it up");
        return STATUS_SUCCESS;
    }
    uint32_t measured = measure_refresh(g), refresh = g->refresh_mhz ? g->refresh_mhz : measured;
    klog_info("amdgpu: %u.%02u frames per second (%s; the frame counter says about %u)", refresh / 1000,
              refresh % 1000 / 10, g->refresh_mhz ? "from the pixel clock" : "measured", measured / 1000);
    if (!cmdline_value("amdgpu", option, sizeof(option)) || strcmp(option, "on") != 0) {
        klog_info("amdgpu: nothing changed; boot with amdgpu=on for the hardware pointer and page flipping");
        return STATUS_SUCCESS;
    }

    /* Behind the firmware's framebuffer, on boundaries of 2 MiB: the second framebuffer, then the pointer. */
    uint64_t usable = g->vram_size < g->aperture_size ? g->vram_size : g->aperture_size;
    g->framebuffers[0] = d->phys - g->aperture;
    g->framebuffers[1] = align_up(g->framebuffers[0] + d->info.size, 2ull << 20);
    g->cursor_offset = align_up(g->framebuffers[1] + d->info.size, 2ull << 20);
    bool room = g->cursor_offset + PAGE_SIZE * 4 <= usable;
    if (!room)
        klog_info("amdgpu: only %lu MiB of video memory: no second framebuffer", usable >> 20);

    if (measured >= 10000) {
        amdgpu_ops.wait_vblank = amdgpu_wait_vblank;
        void *second = room ? (void *)vmm_map_mmio(g->aperture + g->framebuffers[1], d->info.size, VM_WRITE_COMBINING)
                            : NULL;
        if (second) {
            memset(second, 0, (size_t)d->info.size);
            amdgpu_ops.flip = amdgpu_flip;
        }
    } else {
        klog_warn("amdgpu: the frame counter does not run: no vertical blank timing");
    }
    if (room)
        g->cursor = (volatile uint32_t *)vmm_map_mmio(g->aperture + g->cursor_offset, PAGE_SIZE * 4, VM_WRITE_COMBINING);
    if (g->cursor) {
        for (uint32_t i = 0; i < JELLY_CURSOR_SIZE * JELLY_CURSOR_SIZE; i++)
            g->cursor[i] = 0; /* transparent until the display server sets an image */
        amdgpu_ops.cursor_image = amdgpu_cursor_image;
        amdgpu_ops.cursor_move = amdgpu_cursor_move;
    }
    status = display_set_driver(0, &amdgpu_ops, g, amdgpu_ops.flip ? g->aperture + g->framebuffers[1] : 0);
    if (!STATUS_IS_ERROR(status)) {
        /* One mode, the firmware's, but with its refresh rate known. */
        jelly_display_mode_t mode = { d->info.width, d->info.height, refresh, JELLY_MODE_PREFERRED };
        display_set_modes(0, &mode, 1, 0);
    }
    return STATUS_SUCCESS;
}

static const device_match_t amdgpu_ids[] = {
    { 0x1002, 0, 0x03, 0x00, MATCH_VENDOR | MATCH_CLASS }, /* AMD display controller */
    DEVICE_MATCH_END,
};

static driver_t amdgpu_driver = {
    .name = "amd-gpu",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_DISPLAY,
    .ids = amdgpu_ids,
    .probe = amdgpu_probe,
};

static status_t amdgpu_module_init(void)
{
    return driver_register(&amdgpu_driver);
}

static const char *const amdgpu_dependencies[] = { "pci", NULL };

MODULE(.name = "amd_gpu", .description = "AMD integrated graphics (DCN 2.1: Ryzen 4000/5000 G), display", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 12, 0), .dependencies = amdgpu_dependencies, .init = amdgpu_module_init);
