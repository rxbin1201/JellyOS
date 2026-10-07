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
 * Over the AUX channel it also reads what the monitor can do, which link
 * the firmware trained and the monitor's modes (EDID); the protocol and the
 * EDID are code shared with the other drivers (dp_aux.c, edid.c).
 *
 * Switching modes (DisplayPort only, and only what the link carries that the
 * firmware trained): the stream to the monitor and the timing generator are
 * stopped, the new timing, pixel clock (the DisplayPort DTO: its phase is
 * the clock in Hz) and picture size are written, and both are started
 * again. The pipe also has some thirty parameters that tell the HUBP when
 * to fetch data, which Linux computes with a large library; here the
 * firmware's values are scaled to the new line time. If no frames come or
 * the pipe reports that it ran out of data, everything is written back.
 * The framebuffers keep their place and their line length.
 *
 * Not yet: a faster link than the firmware's (which needs the port's PHY
 * reprogrammed through the firmware's AtomBIOS tables: a 3440x1440 monitor
 * stays at 60 Hz on a link trained for that), HDMI modes, hot plug, several
 * screens, any kind of acceleration.
 */

#include "drivers/bus/pci/pci.h"
#include "drivers/core/module.h"
#include "drivers/graphics/display.h"
#include "drivers/graphics/dp_aux.h"
#include "drivers/graphics/edid.h"

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

/* VTG m: where in the frame the timing generator starts counting */
#define VTG_CONTROL(m)        (0x0E7A0 + 4u * (uint32_t)(m)) /* bit 31 on, 30:16 first line of blanking, 14:0 "FP2" */

/* Where the GPU sees its video memory (units of 16 MiB) */
#define VM_FB_LOCATION_BASE   0x0E54C
#define VM_FB_LOCATION_TOP    0x0E550

/* HUBP n: the plane that reads the framebuffer */
#define HUBP(n)               (0x370u * (uint32_t)(n))
#define SURFACE_CONFIG(n)     (0x0EA94 + HUBP(n)) /* bits 6:0 pixel format (8: ARGB 8888), 9:8 rotation */
#define VIEWPORT_START(n)     (0x0EAA4 + HUBP(n))
#define VIEWPORT_DIMENSION(n) (0x0EAA8 + HUBP(n)) /* height << 16 | width */
#define VIEWPORT_DIMENSION2(n) (0x0EAB8 + HUBP(n)) /* the same for the "secondary" surface */
#define HUBP_CNTL(n)          (0x0EACC + HUBP(n)) /* bit 0 blanked, bit 1 idle, bit 2 off, bits 7:4 timing generator,
                                                     bit 12 no urgency, bits 30:28 ran out of data */
/* When the HUBP fetches: times in cycles of the reference clock ("refcyc"), places in lines ("dst_y") */
#define TTU_SURFACE(n)        (0x0EBBC + HUBP(n)) /* bits 22:0 refcyc per request */
#define TTU_SURFACE_PRE(n)    (0x0EBC0 + HUBP(n))
#define BLANK_OFFSET_0(n)     (0x0EC18 + HUBP(n)) /* 12:0 refcyc until the end of horizontal blanking, 30:16 a line */
#define BLANK_OFFSET_1(n)     (0x0EC1C + HUBP(n)) /* a place in the frame, in quarter lines */
#define DST_DIMENSIONS(n)     (0x0EC20 + HUBP(n)) /* refcyc per line */
#define VBLANK_PARAMETERS_1(n) (0x0EC34 + HUBP(n))
#define VBLANK_PARAMETERS_3(n) (0x0EC3C + HUBP(n))
#define NOM_PARAMETERS_1(n)   (0x0EC54 + HUBP(n))
#define NOM_PARAMETERS_5(n)   (0x0EC64 + HUBP(n))
#define LINE_DELIVERY_PRE(n)  (0x0EC70 + HUBP(n)) /* 12:0 refcyc per line delivered */
#define LINE_DELIVERY(n)      (0x0EC74 + HUBP(n))
#define REF_TO_PIXEL_FREQ(n)  (0x0EC7C + HUBP(n)) /* reference clock : pixel clock, 19 fraction bits */
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
#define HUBP_IDLE             (1u << 1)
#define HUBP_OFF              (1u << 2)
#define HUBP_NO_URGENCY       (1u << 12)
#define HUBP_UNDERFLOW        (7u << 28)
#define FLIP_IMMEDIATE        (1u << 1)
#define FLIP_PENDING          (1u << 8)
#define CURSOR_ON             (1u << 0)
#define CURSOR_ARGB           (3u << 8)  /* 32 bits per pixel, alpha not premultiplied */
#define CURSOR_PITCH_64       (0u << 16)
#define CURSOR_LINES_8        (3u << 24) /* lines fetched per request, for images up to 64 pixels wide */

/* DPP n: the cursor is mixed into the picture here; its scaler knows the size of the picture */
#define DPP(n)                (0x5ACu * (uint32_t)(n))
#define DPP_CURSOR_CONTROL(n) (0x10680 + DPP(n)) /* bit 0 on, bits 6:4 mode */
#define SCALER_OTG_H_BLANK(n) (0x10700 + DPP(n)) /* copies of the timing generator's blanking */
#define SCALER_OTG_V_BLANK(n) (0x10704 + DPP(n))
#define SCALER_RECOUT_SIZE(n) (0x1070C + DPP(n)) /* height << 16 | width */
#define SCALER_MPC_SIZE(n)    (0x10710 + DPP(n))

/* Output: the buffer before the timing generator, and the timing generator's input */
#define MPCC_OPP_ID(n)        (0x11CCC + 0x6Cu * (uint32_t)(n)) /* which output a blender feeds */
#define OPPBUF_CONTROL(o)     (0x13510 + 0x168u * (uint32_t)(o)) /* bits 13:0 active width */
#define OPTC_INPUT_CONTROL(m) (0x13E28 + 0x40u * (uint32_t)(m)) /* bit 10 ran out of data (bit 12 clears it) */
#define OPTC_WIDTH_CONTROL(m) (0x13E38 + 0x40u * (uint32_t)(m)) /* bits 12:0 width */

/* OTG m: the timing generator */
#define OTG(m)                (0x200u * (uint32_t)(m))
#define OTG_H_TOTAL(m)        (0x13FA8 + OTG(m)) /* total - 1 */
#define OTG_H_BLANK(m)        (0x13FAC + OTG(m)) /* end << 16 | start */
#define OTG_H_SYNC(m)         (0x13FB0 + OTG(m)) /* end << 16 | start: the line begins with the sync pulse */
#define OTG_H_SYNC_CNTL(m)    (0x13FB4 + OTG(m)) /* bit 0: negative polarity */
#define OTG_V_TOTAL(m)        (0x13FBC + OTG(m))
#define OTG_V_BLANK(m)        (0x13FD8 + OTG(m))
#define OTG_V_SYNC(m)         (0x13FDC + OTG(m))
#define OTG_V_SYNC_CNTL(m)    (0x13FE0 + OTG(m))
#define OTG_CONTROL(m)        (0x14004 + OTG(m)) /* bit 0 on, bits 9:8 where to stop, bit 16 running */
#define OTG_FRAME_COUNT(m)    (0x14030 + OTG(m))
#define OTG_CLOCK_CONTROL(m)  (0x14118 + OTG(m)) /* bit 16 busy */
#define OTG_VSTARTUP(m)       (0x1411C + OTG(m)) /* lines before the picture at which the HUBP starts */
#define OTG_VUPDATE(m)        (0x14120 + OTG(m)) /* 15:0 offset in pixels, 25:16 width */
#define OTG_VREADY(m)         (0x14124 + OTG(m))

#define OTG_RUNNING           (1u << 16)

/* Encoders: which connection the timing generator feeds */
#define DIG_FE_CNTL(i)        (0x154A0 + 0x400u * (uint32_t)(i)) /* bits 2:0 timing generator, bit 10 started */
#define DIG_BE_CNTL(i)        (0x155BC + 0x400u * (uint32_t)(i)) /* bits 18:16 mode */
#define DIG_BE_EN_CNTL(i)     (0x155C0 + 0x400u * (uint32_t)(i))
#define DP_LINK_CNTL(i)       (0x15720 + 0x400u * (uint32_t)(i))
#define DP_VID_STREAM_CNTL(i) (0x15730 + 0x400u * (uint32_t)(i)) /* bit 0 on, 9:8 when to stop, bit 16 sending */
#define DP_STEER_FIFO(i)      (0x15734 + 0x400u * (uint32_t)(i)) /* bit 0 reset */
#define DP_VID_TIMING(i)      (0x15740 + 0x400u * (uint32_t)(i)) /* bit 8: M is measured by the hardware */
#define DP_VID_N(i)           (0x15744 + 0x400u * (uint32_t)(i))
#define DP_VID_M(i)           (0x15748 + 0x400u * (uint32_t)(i))
#define DP_MSA_TIMING(i, k)   (0x15830 + 0x400u * (uint32_t)(i) + 4u * (uint32_t)(k)) /* what the monitor is told: 1-4 */
#define DIG_COUNT             5

/* AUX channels: one engine per connector */
#define AUX(i)                (0x70u * (uint32_t)(i))
#define AUX_CONTROL(i)        (0x15040 + AUX(i)) /* bit 0 on */
#define AUX_SW_CONTROL(i)     (0x15044 + AUX(i)) /* bit 0 go, bits 20:16 bytes to send */
#define AUX_ARB_CONTROL(i)    (0x15048 + AUX(i)) /* bits 3:2 who has the channel, bit 16 request, bit 17 done */
#define AUX_INTERRUPT(i)      (0x1504C + AUX(i)) /* bit 1 acknowledge "done" */
#define AUX_SW_STATUS(i)      (0x15050 + AUX(i)) /* bit 0 done, errors, bits 28:24 bytes received */
#define AUX_SW_DATA(i)        (0x15058 + AUX(i)) /* bit 0 read, bits 15:8 a byte, bit 31 start at the first byte */
#define AUX_COUNT             5

#define AUX_OWNER(arb)        (((arb) >> 2) & 3) /* 1: software, 2: the display microcontroller */
#define AUX_DONE              (1u << 0)
#define AUX_TIMEOUT_BITS      (0x7u << 4 | 1u << 7 | 1u << 9) /* no reply, or nothing plugged in */
#define AUX_ERROR_BITS        (1u << 14 | 1u << 20 | 1u << 22 | 1u << 23)

/* Clocks (the hardware counts them in units of 100 kHz) and the mailbox of the system management unit */
#define CLK_DISPCLK           0x5BA28 /* display clock: must be at least the pixel clock */
#define CLK_DPPCLK            0x5BA2C
#define CLK_DPREFCLK          0x5BA30 /* reference of the DisplayPort DTOs */
#define CLK_DCFCLK            0x5BA34
#define SMU_MESSAGE           0x58A0C
#define SMU_ARGUMENT          0x58A4C
#define SMU_RESPONSE          0x58A6C

#define REGISTER_WINDOW       0x60000u
#define MAX_MODES             24
#define MAX_WRITES            48

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

    int               aux_engine;              /* the AUX channel the monitor answers on, -1 if none */
    uint32_t          link_khz, lanes;         /* DisplayPort link as the firmware trained it */
    uint32_t          limit_khz;               /* fastest pixel clock that link carries */
    uint8_t           edid[256];
    display_timing_t  modes[MAX_MODES];
    uint32_t          mode_count;

    int               encoder;                 /* the encoder our timing generator feeds, -1 if not found */
    bool              displayport;
    display_timing_t  current;                 /* the timing on the screen */
} amdgpu_t;

/* A register and the value it is to get: a mode switch is a list of these, and so is its undoing. */
typedef struct {
    uint32_t reg, value;
} reg_write_t;

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

/* Busy-wait until (reg & mask) == want. The clock ticks in milliseconds, so short waits round up. */
static bool wait_bits(amdgpu_t *g, uint32_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
    uint64_t end = clock_monotonic_ns() / 1000000 + ms + 1;
    while ((rd(g, reg) & mask) != want) {
        if (clock_monotonic_ns() / 1000000 > end)
            return false;
    }
    return true;
}

/* --- DisplayPort AUX channel ------------------------------------------------------------ */

typedef struct {
    amdgpu_t *g;
    int       engine;
} aux_engine_t;

/* One AUX message with engine i (after Linux's dce_aux.c). The protocol above it is in dp_aux.c. */
static int aux_once(void *context, const uint8_t *tx, int tx_length, uint8_t *rx, int rx_max)
{
    aux_engine_t *where = context;
    amdgpu_t *g = where->g;
    uint32_t i = (uint32_t)where->engine;
    int result;

    /* Only engines the firmware left switched on, and not while the display microcontroller uses the channel. */
    if (!(rd(g, AUX_CONTROL(i)) & 1u) || AUX_OWNER(rd(g, AUX_ARB_CONTROL(i))) == 2)
        return DP_AUX_STUCK;
    wr(g, AUX_ARB_CONTROL(i), rd(g, AUX_ARB_CONTROL(i)) | 1u << 16);
    if (AUX_OWNER(rd(g, AUX_ARB_CONTROL(i))) != 1) {
        result = DP_AUX_STUCK;
    } else {
        wr(g, AUX_INTERRUPT(i), rd(g, AUX_INTERRUPT(i)) | 1u << 1);
        wait_bits(g, AUX_SW_STATUS(i), AUX_DONE, 0, 2);
        wr(g, AUX_SW_CONTROL(i), (rd(g, AUX_SW_CONTROL(i)) & ~(0x1Fu << 16 | 0xFu << 4 | 1u)) | (uint32_t)tx_length << 16);
        wr(g, AUX_SW_DATA(i), 1u << 31 | (uint32_t)tx[0] << 8); /* from the first byte on, each write is the next one */
        for (int k = 1; k < tx_length; k++)
            wr(g, AUX_SW_DATA(i), (uint32_t)tx[k] << 8);
        wr(g, AUX_SW_CONTROL(i), rd(g, AUX_SW_CONTROL(i)) | 1u);

        uint32_t status = wait_bits(g, AUX_SW_STATUS(i), AUX_DONE, AUX_DONE, 10) ? rd(g, AUX_SW_STATUS(i)) : 0;
        int n = (int)((status >> 24) & 0x1F);
        if (!(status & AUX_DONE) || (status & AUX_TIMEOUT_BITS)) {
            result = DP_AUX_NO_ANSWER;
        } else if ((status & AUX_ERROR_BITS) || n == 0) {
            result = DP_AUX_ERROR;
        } else {
            wr(g, AUX_SW_DATA(i), 1u << 31 | 1u); /* read, from the first byte on */
            for (int k = 0; k < n; k++) {
                uint8_t byte = (uint8_t)(rd(g, AUX_SW_DATA(i)) >> 8);
                if (k < rx_max)
                    rx[k] = byte;
            }
            result = n < rx_max ? n : rx_max;
        }
    }
    wr(g, AUX_ARB_CONTROL(i), (rd(g, AUX_ARB_CONTROL(i)) & ~(1u << 16)) | 1u << 17);
    return result;
}

/*
 * The monitor over the AUX channel: which engine it answers on, what it can do, what link the firmware
 * trained, and its modes. Nothing is changed; all of it is what switching modes will build on.
 */
static void read_monitor(amdgpu_t *g)
{
    aux_engine_t where = { g, 0 };
    dp_aux_t aux = { &where, aux_once };
    uint8_t caps[16], link[2];

    g->aux_engine = -1;
    for (int i = 0; i < AUX_COUNT && g->aux_engine < 0; i++) {
        where.engine = i;
        klog_debug("amdgpu: AUX engine %d: control 0x%x, arbitration 0x%x, status 0x%x", i, rd(g, AUX_CONTROL(i)),
                   rd(g, AUX_ARB_CONTROL(i)), rd(g, AUX_SW_STATUS(i)));
        if (dp_dpcd_read(&aux, DPCD_REVISION, caps, 16) == 16 && caps[0] && caps[1])
            g->aux_engine = i;
    }
    if (g->aux_engine < 0) {
        klog_info("amdgpu: no monitor answers on an AUX channel (not DisplayPort, or the channel is not set up)");
        return;
    }
    klog_info("amdgpu: AUX channel %d: the monitor accepts up to %u lanes at %u.%02u Gbit/s (DisplayPort %u.%u)",
              g->aux_engine, caps[2] & 0x1F, caps[1] * 27 / 100, caps[1] * 27 % 100, caps[0] >> 4, caps[0] & 0xF);
    if (dp_dpcd_read(&aux, DPCD_LINK_BW_SET, link, 2) == 2 && link[0] && (link[1] & 0x1F)) {
        g->link_khz = (uint32_t)link[0] * 27000;
        g->lanes = link[1] & 0x1F;
        /* 24 bits per pixel over 8b/10b lanes */
        g->limit_khz = (uint32_t)((uint64_t)g->link_khz * 8 * g->lanes / 24);
        klog_info("amdgpu: DisplayPort link as trained: %u lane%s at %u.%02u Gbit/s; modes up to %u kHz pixel clock",
                  g->lanes, g->lanes == 1 ? "" : "s", g->link_khz / 100000, g->link_khz / 1000 % 100, g->limit_khz);
    }
    int blocks = dp_edid_read(&aux, g->edid);
    if (!blocks) {
        klog_warn("amdgpu: the monitor's data (EDID) cannot be read");
        return;
    }
    g->mode_count = edid_collect_timings(g->edid, blocks, g->modes, 0, MAX_MODES);
    display_timing_sort(g->modes, g->mode_count);
    for (uint32_t i = 0; i < g->mode_count; i++) {
        const display_timing_t *t = &g->modes[i];
        uint32_t hz = display_timing_hz100(t);
        klog_info("amdgpu: monitor mode %ux%u at %u.%02u Hz, pixel clock %u kHz (total %ux%u)%s", t->ha, t->va, hz / 100,
                  hz % 100, t->khz, t->ht, t->vt,
                  g->limit_khz && t->khz > g->limit_khz ? " (too fast for the link as it is)" : "");
    }
}

/* The clocks and, at debug level, the registers of the pipe as the firmware left them (dmesg amdgpu). */
static void dump_state(amdgpu_t *g)
{
    static const uint32_t ranges[][2] = {
        { 0x00480, 0x00560 }, /* clock generator */
        { 0x0E400, 0x0E600 }, /* memory hub */
        { 0x0EA80, 0x0ED10 }, /* HUBP 0 */
        { 0x10680, 0x10740 }, /* DPP 0: cursor, scaler */
        { 0x11CC0, 0x11D00 }, /* blender 0 */
        { 0x13FA0, 0x14160 }, /* timing generator 0 */
        { 0x15720, 0x15880 }, /* encoder 0: DisplayPort part */
        { 0x0E7A0, 0x0E7C0 }, /* where the timing generators start */
        { 0x13400, 0x13540 }, /* output formatter and buffer 0 */
        { 0x13E20, 0x13F20 }, /* inputs of the timing generators */
    };

    klog_info("amdgpu: clocks: display %u kHz, DPP %u kHz, DisplayPort reference %u kHz, memory hub %u kHz; SMU 0x%x",
              rd(g, CLK_DISPCLK) * 100, rd(g, CLK_DPPCLK) * 100, rd(g, CLK_DPREFCLK) * 100, rd(g, CLK_DCFCLK) * 100,
              rd(g, SMU_RESPONSE));
    for (size_t r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++) {
        /* Blocks of another pipe than 0 lie at the same distance as their first registers. */
        uint32_t shift = r == 2 ? HUBP(g->hubp) : r == 3 ? DPP(g->hubp) : r == 5 ? OTG(g->otg)
                         : r == 6 && g->encoder > 0 ? 0x400u * (uint32_t)g->encoder : 0;
        for (uint32_t reg = ranges[r][0] + shift; reg < ranges[r][1] + shift; reg += 32)
            klog_debug("amdgpu: reg %05x: %08x %08x %08x %08x %08x %08x %08x %08x", reg, rd(g, reg), rd(g, reg + 4),
                       rd(g, reg + 8), rd(g, reg + 12), rd(g, reg + 16), rd(g, reg + 20), rd(g, reg + 24),
                       rd(g, reg + 28));
    }
}

static void surface_show(amdgpu_t *g, uint64_t offset)
{
    uint64_t address = g->vram_base + offset;

    wr(g, FLIP_CONTROL(g->hubp), rd(g, FLIP_CONTROL(g->hubp)) & ~FLIP_IMMEDIATE); /* at the next frame: no tearing */
    wr(g, SURFACE_ADDRESS_HIGH(g->hubp), (uint32_t)(address >> 32));
    wr(g, SURFACE_ADDRESS(g->hubp), (uint32_t)address);
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

/* --- Switching modes ------------------------------------------------------------------- */

/* The stream to the monitor, the HUBP and the timing generator off, in that order. */
static void pipe_stop(amdgpu_t *g)
{
    uint32_t e = (uint32_t)g->encoder, stream = rd(g, DP_VID_STREAM_CNTL(e));

    if (stream & 1u) {
        /* Stop at the start of the next vertical blank (2), then wait until nothing is sent any more. */
        wr(g, DP_VID_STREAM_CNTL(e), (stream & ~(3u << 8)) | 2u << 8);
        wr(g, DP_VID_STREAM_CNTL(e), ((stream & ~(3u << 8)) | 2u << 8) & ~1u);
        if (!wait_bits(g, DP_VID_STREAM_CNTL(e), 1u << 16, 0, 120))
            klog_warn("amdgpu: the video stream does not stop (0x%x)", rd(g, DP_VID_STREAM_CNTL(e)));
    }
    wr(g, DP_STEER_FIFO(e), rd(g, DP_STEER_FIFO(e)) | 1u);
    wr(g, HUBP_CNTL(g->hubp), rd(g, HUBP_CNTL(g->hubp)) | HUBP_BLANK | HUBP_NO_URGENCY);
    wait_bits(g, HUBP_CNTL(g->hubp), HUBP_IDLE, HUBP_IDLE, 3);
    wr(g, OTG_CONTROL(g->otg), (rd(g, OTG_CONTROL(g->otg)) | 3u << 8) & ~1u);
    wr(g, VTG_CONTROL(g->otg), rd(g, VTG_CONTROL(g->otg)) & ~(1u << 31));
    if (!wait_bits(g, OTG_CLOCK_CONTROL(g->otg), 1u << 16, 0, 120))
        klog_warn("amdgpu: the timing generator does not stop (0x%x)", rd(g, OTG_CONTROL(g->otg)));
}

/* The reverse; `t` is the timing that was written. */
static void pipe_start(amdgpu_t *g, const display_timing_t *t)
{
    uint32_t e = (uint32_t)g->encoder;

    wr(g, VTG_CONTROL(g->otg), rd(g, VTG_CONTROL(g->otg)) | 1u << 31);
    wr(g, OTG_CONTROL(g->otg), rd(g, OTG_CONTROL(g->otg)) | 3u << 8 | 1u);
    if (!wait_bits(g, OTG_CONTROL(g->otg), OTG_RUNNING, OTG_RUNNING, 120))
        klog_warn("amdgpu: the timing generator does not start (0x%x)", rd(g, OTG_CONTROL(g->otg)));
    wr(g, HUBP_CNTL(g->hubp), rd(g, HUBP_CNTL(g->hubp)) & ~(HUBP_BLANK | HUBP_NO_URGENCY));

    /* The monitor is told pixel clock : link clock as M : N. A first value, then the hardware measures it. */
    wr(g, DP_VID_TIMING(e), rd(g, DP_VID_TIMING(e)) & ~(1u << 8));
    wr(g, DP_VID_N(e), 0x8000);
    wr(g, DP_VID_M(e), (uint32_t)((uint64_t)0x8000 * t->khz / g->link_khz));
    wr(g, DP_VID_TIMING(e), rd(g, DP_VID_TIMING(e)) | 1u << 8);
    /* The buffers between timing generator and encoder start empty. */
    wr(g, DIG_FE_CNTL(e), rd(g, DIG_FE_CNTL(e)) | 1u << 10);
    (void)rd(g, DIG_FE_CNTL(e));
    wr(g, DIG_FE_CNTL(e), rd(g, DIG_FE_CNTL(e)) & ~(1u << 10));
    wr(g, DP_STEER_FIFO(e), rd(g, DP_STEER_FIFO(e)) | 1u);
    (void)rd(g, DP_STEER_FIFO(e));
    wr(g, DP_STEER_FIFO(e), rd(g, DP_STEER_FIFO(e)) & ~1u);
    sleep_ms(1);
    wr(g, DP_VID_STREAM_CNTL(e), rd(g, DP_VID_STREAM_CNTL(e)) | 1u); /* from the next frame on */
}

static void plan(reg_write_t *list, uint32_t *count, uint32_t reg, uint32_t value)
{
    if (*count < MAX_WRITES)
        list[(*count)++] = (reg_write_t){ reg, value };
}

/* A time in reference clock cycles, kept in the low `bits` of a register, stretched by to : from. */
static void plan_scaled(amdgpu_t *g, reg_write_t *list, uint32_t *count, uint32_t reg, uint32_t bits, uint64_t to,
                        uint64_t from)
{
    uint32_t old = rd(g, reg), mask = (1u << bits) - 1;
    uint64_t value = from ? (uint64_t)(old & mask) * to / from : old & mask;

    if (old & mask)
        plan(list, count, reg, (old & ~mask) | (uint32_t)(value > mask ? mask : value));
}

/* A register that holds the old picture size in its low `bits` gets the new one; others are left alone. */
static void plan_size(amdgpu_t *g, reg_write_t *list, uint32_t *count, uint32_t reg, uint32_t bits, uint32_t old_size,
                      uint32_t new_size)
{
    uint32_t value = rd(g, reg), mask = bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;

    if ((value & mask) == old_size)
        plan(list, count, reg, (value & ~mask) | new_size);
}

/* Everything that depends on the timing, for timing t; `old` is what the registers hold now. */
static uint32_t plan_mode(amdgpu_t *g, const display_timing_t *t, const display_timing_t *old, reg_write_t *list)
{
    uint32_t count = 0, n = (uint32_t)g->hubp, m = (uint32_t)g->otg, e = (uint32_t)g->encoder;
    uint32_t h_blank_start = t->ht - t->hso, h_blank_end = h_blank_start - t->ha;
    uint32_t v_blank_start = t->vt - t->vso, v_blank_end = v_blank_start - t->va;
    uint32_t old_h_blank = rd(g, OTG_H_BLANK(m)), old_v_blank = rd(g, OTG_V_BLANK(m));
    uint32_t old_size = old->va << 16 | old->ha, new_size = t->va << 16 | t->ha;
    uint32_t opp = rd(g, MPCC_OPP_ID(n)) & 0xF;

    /* Pixel clock */
    plan(list, &count, DP_DTO_PHASE(m), t->khz * 1000);

    /* Timing generator */
    plan(list, &count, OTG_H_TOTAL(m), t->ht - 1);
    plan(list, &count, OTG_H_SYNC(m), t->hsw << 16);
    plan(list, &count, OTG_H_BLANK(m), h_blank_end << 16 | h_blank_start);
    plan(list, &count, OTG_H_SYNC_CNTL(m), (rd(g, OTG_H_SYNC_CNTL(m)) & ~1u) | (t->hpos ? 0 : 1));
    plan(list, &count, OTG_V_TOTAL(m), t->vt - 1);
    plan(list, &count, OTG_V_SYNC(m), t->vsw << 16);
    plan(list, &count, OTG_V_BLANK(m), v_blank_end << 16 | v_blank_start);
    plan(list, &count, OTG_V_SYNC_CNTL(m), (rd(g, OTG_V_SYNC_CNTL(m)) & ~1u) | (t->vpos ? 0 : 1));
    /* The HUBP starts some lines before the picture (within the blanking), the update window lies within a line. */
    uint32_t startup = rd(g, OTG_VSTARTUP(m)) & 0x3FF, blanking = t->vt - t->va;
    if (startup >= blanking)
        startup = blanking - 1;
    plan(list, &count, OTG_VSTARTUP(m), startup);
    uint32_t update = rd(g, OTG_VUPDATE(m));
    plan(list, &count, OTG_VUPDATE(m), (update & 0xFFFF0000) | (uint32_t)((uint64_t)(update & 0xFFFF) * t->ht / old->ht));
    uint32_t first = v_blank_end + 1 < startup ? startup - v_blank_end - 1 : 0;
    plan(list, &count, VTG_CONTROL(m), (rd(g, VTG_CONTROL(m)) & 1u << 31) | v_blank_start << 16 | first);

    /* Picture size: the plane, the scaler (not scaling), the output buffer, the timing generator's input */
    plan(list, &count, VIEWPORT_DIMENSION(n), new_size);
    plan_size(g, list, &count, VIEWPORT_DIMENSION2(n), 32, old_size, new_size);
    plan_size(g, list, &count, SCALER_RECOUT_SIZE(n), 32, old_size, new_size);
    plan_size(g, list, &count, SCALER_MPC_SIZE(n), 32, old_size, new_size);
    plan_size(g, list, &count, SCALER_OTG_H_BLANK(n), 32, (old_h_blank >> 16) << 16 | (old_h_blank & 0x3FFF),
              h_blank_end << 16 | h_blank_start);
    plan_size(g, list, &count, SCALER_OTG_V_BLANK(n), 32, (old_v_blank >> 16) << 16 | (old_v_blank & 0x3FFF),
              v_blank_end << 16 | v_blank_start);
    if (opp < 6)
        plan_size(g, list, &count, OPPBUF_CONTROL(opp), 14, old->ha, t->ha);
    plan_size(g, list, &count, OPTC_WIDTH_CONTROL(m), 13, old->ha, t->ha);

    /*
     * When the HUBP fetches. Times in reference clock cycles follow the time a line takes; the ratio of the
     * clocks follows the pixel clock; places in the frame follow the picture.
     */
    uint64_t line_to = (uint64_t)t->ht * old->khz, line_from = (uint64_t)old->ht * t->khz;
    plan_scaled(g, list, &count, DST_DIMENSIONS(n), 21, line_to, line_from);
    plan_scaled(g, list, &count, REF_TO_PIXEL_FREQ(n), 21, old->khz, t->khz);
    plan_scaled(g, list, &count, VBLANK_PARAMETERS_1(n), 23, line_to, line_from);
    plan_scaled(g, list, &count, VBLANK_PARAMETERS_3(n), 23, line_to, line_from);
    plan_scaled(g, list, &count, NOM_PARAMETERS_1(n), 23, line_to, line_from);
    plan_scaled(g, list, &count, NOM_PARAMETERS_5(n), 23, line_to, line_from);
    plan_scaled(g, list, &count, LINE_DELIVERY(n), 13, line_to, line_from);
    plan_scaled(g, list, &count, LINE_DELIVERY_PRE(n), 13, line_to, line_from);
    plan_scaled(g, list, &count, TTU_SURFACE(n), 23, line_to, line_from);
    plan_scaled(g, list, &count, TTU_SURFACE_PRE(n), 23, line_to, line_from);
    uint32_t offset = rd(g, BLANK_OFFSET_0(n)), old_h_end = old_h_blank >> 16, old_v_end = old_v_blank >> 16;
    uint64_t h_end = old_h_end ? (uint64_t)(offset & 0x1FFF) * h_blank_end * old->khz / ((uint64_t)old_h_end * t->khz) : 0;
    uint32_t v_end = old_v_end ? ((offset >> 16) & 0x7FFF) * v_blank_end / old_v_end : 0;
    plan(list, &count, BLANK_OFFSET_0(n), (uint32_t)(h_end > 0x1FFF ? 0x1FFF : h_end) | v_end << 16);
    plan_scaled(g, list, &count, BLANK_OFFSET_1(n), 18, t->va, old->va);

    /* What the monitor is told about the timing (main stream attributes) */
    plan(list, &count, DP_MSA_TIMING(e, 0), t->ht << 16 | t->vt);
    plan(list, &count, DP_MSA_TIMING(e, 1), h_blank_end << 16 | v_blank_end); /* picture start after sync start */
    plan(list, &count, DP_MSA_TIMING(e, 2), (t->hpos ? 0 : 1u << 31) | t->hsw << 16 | (t->vpos ? 0 : 1u << 15) | t->vsw);
    plan(list, &count, DP_MSA_TIMING(e, 3), t->ha << 16 | t->va);
    return count;
}

/* Stop the pipe, write a list (remembering what was there in `undo`), start it. True if frames come out intact. */
static bool pipe_apply(amdgpu_t *g, const reg_write_t *list, uint32_t count, reg_write_t *undo,
                       const display_timing_t *t)
{
    pipe_stop(g);
    for (uint32_t i = 0; i < count; i++) {
        if (undo)
            undo[i] = (reg_write_t){ list[i].reg, rd(g, list[i].reg) };
        wr(g, list[i].reg, list[i].value);
    }
    surface_show(g, g->framebuffers[0]);
    g->flip_pending = false;
    pipe_start(g, t);

    /* Let it settle, forget what happened while starting, then watch a quarter of a second. */
    sleep_ms(100);
    wr(g, OPTC_INPUT_CONTROL(g->otg), rd(g, OPTC_INPUT_CONTROL(g->otg)) | 1u << 12);
    wr(g, HUBP_CNTL(g->hubp), rd(g, HUBP_CNTL(g->hubp)) | 1u << 31);
    wr(g, HUBP_CNTL(g->hubp), rd(g, HUBP_CNTL(g->hubp)) & ~(1u << 31));
    uint32_t before = rd(g, OTG_FRAME_COUNT(g->otg));
    sleep_ms(250);
    uint32_t frames = rd(g, OTG_FRAME_COUNT(g->otg)) - before;
    uint32_t input = rd(g, OPTC_INPUT_CONTROL(g->otg)), plane = rd(g, HUBP_CNTL(g->hubp));
    bool starved = (input & (1u << 10)) || (plane & HUBP_UNDERFLOW);
    if (frames < 3 || starved)
        klog_warn("amdgpu: %u frames in 250 ms%s (timing generator input 0x%x, plane 0x%x, stream 0x%x)", frames,
                  starved ? ", and the pipe ran out of data" : "", input, plane,
                  rd(g, DP_VID_STREAM_CNTL((uint32_t)g->encoder)));
    return frames >= 3 && !starved;
}

static status_t amdgpu_set_mode(display_t *display, uint32_t mode, uint32_t *pitch)
{
    static reg_write_t list[MAX_WRITES], undo[MAX_WRITES];
    amdgpu_t *g = amdgpu_of(display);

    if (mode >= g->mode_count)
        return STATUS_INVALID_ARGUMENT;
    const display_timing_t *t = &g->modes[mode];
    display_timing_t old = g->current;
    uint32_t count = plan_mode(g, t, &old, list);

    klog_info("amdgpu: switching to %ux%u, pixel clock %u kHz (%u registers)", t->ha, t->va, t->khz, count);
    /* The old picture has another size: black until the console or the display server has drawn. */
    memset((void *)display->pixels, 0, (size_t)display->info.size);
    if (!pipe_apply(g, list, count, undo, t)) {
        klog_warn("amdgpu: %ux%u does not come up: back to the mode before", t->ha, t->va);
        pipe_apply(g, undo, count, NULL, &old);
        return STATUS_DEVICE_ERROR;
    }
    g->current = *t;
    g->refresh_mhz = display_timing_mhz(t);
    *pitch = display->info.pitch; /* the framebuffers keep their line length; the picture is their top left part */
    return STATUS_SUCCESS;
}

/*
 * The list of modes for the display layer: the monitor's modes that the link carries and that fit into the
 * framebuffers, with the one on the screen among them. Entry i there is g->modes[i] here.
 */
static void publish_modes(amdgpu_t *g, display_t *d, bool switchable)
{
    jelly_display_mode_t list[MAX_MODES];
    uint32_t kept = 0, current = DISPLAY_NO_MODE;
    bool known = g->current.khz && g->current.ha == d->info.width && g->current.va == d->info.height;

    if (known)
        g->mode_count = display_timing_add(g->modes, g->mode_count, MAX_MODES, &g->current);
    for (uint32_t i = 0; i < g->mode_count && switchable; i++) {
        const display_timing_t *t = &g->modes[i];
        bool shown = known && display_timing_same(t, &g->current);
        bool fits = t->ha * 4 <= d->info.pitch && (uint64_t)t->va * d->info.pitch <= d->info.size;
        if (shown || (fits && g->limit_khz && t->khz <= g->limit_khz))
            g->modes[kept++] = *t;
    }
    if (!switchable && known) {
        g->modes[0] = g->current; /* a list of one: what the screen shows */
        kept = 1;
    }
    g->mode_count = kept;
    display_timing_sort(g->modes, kept);
    for (uint32_t i = 0; i < kept; i++) {
        const display_timing_t *t = &g->modes[i];
        list[i] = (jelly_display_mode_t){ t->ha, t->va, display_timing_mhz(t), i == 0 ? JELLY_MODE_PREFERRED : 0 };
        if (known && display_timing_same(t, &g->current))
            current = i;
    }
    if (kept)
        display_set_modes(0, list, kept, current);
}

static display_ops_t amdgpu_ops; /* filled with what works on this machine */

/* --- Reading the firmware's state ------------------------------------------------------ */

static const char *connection_name(amdgpu_t *g)
{
    static const char *const modes[8] = { "DisplayPort", "LVDS", "DVI", "HDMI", "?", "DisplayPort (MST)", "?", "?" };

    g->encoder = -1;
    for (int i = 0; i < DIG_COUNT; i++) {
        uint32_t front = rd(g, DIG_FE_CNTL(i)), back = rd(g, DIG_BE_CNTL(i));
        /* The back end names its front ends as a bit mask; a back end that is on has a mode and a front end. */
        if ((front & 7) == (uint32_t)g->otg && ((back >> 8) & 0x7F & (1u << i)) && (rd(g, DIG_BE_EN_CNTL(i)) & 1u)) {
            g->encoder = i;
            g->displayport = ((back >> 16) & 7) == 0;
            return modes[(back >> 16) & 7];
        }
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
    if ((rd(g, PIXEL_RATE_CNTL(m)) & (1u << 4)) && h_total > 1 && v_total > 1) {
        uint32_t h_sync = rd(g, OTG_H_SYNC(m)), v_sync = rd(g, OTG_V_SYNC(m));
        display_timing_t *now = &g->current;
        g->refresh_mhz = (uint32_t)((uint64_t)rd(g, DP_DTO_PHASE(m)) * 1000 / ((uint64_t)h_total * v_total));
        /* The timing generator counts from the start of the sync pulse; blanking ends where the picture begins. */
        now->khz = rd(g, DP_DTO_PHASE(m)) / 1000;
        now->ha = h_active;
        now->ht = h_total;
        now->hsw = (h_sync >> 16) - (h_sync & 0x7FFF);
        now->hso = h_total - (h_blank & 0x7FFF);
        now->va = v_active;
        now->vt = v_total;
        now->vsw = (v_sync >> 16) - (v_sync & 0x7FFF);
        now->vso = v_total - (v_blank & 0x7FFF);
        now->hpos = !(rd(g, OTG_H_SYNC_CNTL(m)) & 1u);
        now->vpos = !(rd(g, OTG_V_SYNC_CNTL(m)) & 1u);
    }
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
    if (g->regs_size < REGISTER_WINDOW) {
        klog_warn("amdgpu: the register BAR has only %u KiB", g->regs_size >> 10);
        return STATUS_NOT_SUPPORTED;
    }
    g->regs = (volatile uint8_t *)vmm_map_mmio(pci->bars[5].phys, REGISTER_WINDOW, VM_UNCACHED);
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
    read_monitor(g);
    dump_state(g);
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
    /* Modes can be switched on a DisplayPort link whose speed is known, with the pixel clock from the DTO. */
    bool switchable = g->displayport && g->encoder >= 0 && g->link_khz && g->current.khz && measured >= 10000 &&
                      (rd(g, PIXEL_RATE_CNTL(g->otg)) & (1u << 4));
    if (switchable)
        amdgpu_ops.set_mode = amdgpu_set_mode;
    else
        klog_info("amdgpu: the mode cannot be switched here (%s)",
                  !g->displayport ? "not a DisplayPort connection" : "the link or the pixel clock is not known");
    status = display_set_driver(0, &amdgpu_ops, g, amdgpu_ops.flip ? g->aperture + g->framebuffers[1] : 0);
    if (!STATUS_IS_ERROR(status))
        publish_modes(g, d, switchable);
    (void)refresh;
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
