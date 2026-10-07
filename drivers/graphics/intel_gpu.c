/*
 * Intel integrated graphics, generation 9 (Skylake, Kaby Lake, Coffee Lake,
 * Comet Lake: HD/UHD Graphics 5xx/6xx), display part.
 *
 * The UEFI firmware has already lit the screen, but often in a small mode
 * that the display engine scales up, or in whatever its own driver offers.
 * This driver takes the display engine over far enough to show the
 * monitor's own resolution:
 *
 *   - it reads how the firmware set things up: which pipe shows the boot
 *     framebuffer, on which port, with which timings, link and clock
 *   - it reads the monitor's EDID (DisplayPort: over the AUX channel; HDMI:
 *     over the GMBUS I2C controller) and collects the detailed timings
 *   - with "igpu=native" or "igpu=WIDTHxHEIGHT[@HZ]" on the kernel command
 *     line it allocates a framebuffer of that size, maps it into the
 *     graphics address space (GGTT) and switches:
 *       * if the firmware already runs the monitor at that timing and only
 *         scales a smaller picture up, the scaler is turned off
 *       * DisplayPort: new timings on the link the firmware trained, if the
 *         link and the display clock are fast enough
 *       * HDMI: pipe and port off, the port's PLL reprogrammed, on again
 *     If the pipe does not come up, the firmware's mode is restored.
 *
 * Without the option nothing is changed: the driver only reports (dmesg igpu).
 *
 * Once it owns the framebuffer it also offers the display layer's driver
 * operations (drivers/graphics/display.h), which is all the display server
 * ever sees of this file:
 *   - a hardware pointer (the cursor plane with a 64x64 ARGB image)
 *   - waiting for the vertical blank (the pipe's interrupt, delivered by MSI)
 *   - a second framebuffer and flipping between the two (PLANE_SURF)
 *
 * Ported from the display part of the previous JellyOS implementation
 * (minikernel, drivers/gpu/igd*.c), which was developed on a Core i5-8400T
 * with UHD Graphics 630. Register information from Intel's Programmer's
 * Reference Manuals for Skylake/Kaby Lake and from Linux's i915.
 *
 * QEMU has no such device: this driver can only be tested on real hardware.
 *
 * Not yet: DisplayPort link training (a faster link than the firmware's),
 * changing the display clock (CDCLK), several screens, hot plug, changing
 * modes after the start, any kind of acceleration.
 */

#include "drivers/bus/pci/pci.h"
#include "drivers/core/module.h"
#include "drivers/graphics/display.h"

#include "core/boot.h"
#include "core/cmdline.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/layout.h"
#include "memory/pmm.h"
#include "memory/vmm.h"
#include "scheduler/thread.h"
#include "scheduler/wait.h"
#include "time/clock.h"

/* --- Registers (offsets in BAR 0) --------------------------------------------------- */

#define PIPE_OFF(p)           (0x1000u * (uint32_t)(p))
#define HTOTAL(t)             (0x60000 + PIPE_OFF(t)) /* (total - 1) << 16 | (active - 1) */
#define HBLANK(t)             (0x60004 + PIPE_OFF(t))
#define HSYNC(t)              (0x60008 + PIPE_OFF(t)) /* (end - 1) << 16 | (start - 1) */
#define VTOTAL(t)             (0x6000C + PIPE_OFF(t))
#define VBLANK(t)             (0x60010 + PIPE_OFF(t))
#define VSYNC(t)              (0x60014 + PIPE_OFF(t))
#define PIPESRC(p)            (0x6001C + PIPE_OFF(p)) /* (width - 1) << 16 | (height - 1) */
#define VSYNCSHIFT(t)         (0x60028 + PIPE_OFF(t))
#define PIPE_DATA_M1(t)       (0x60030 + PIPE_OFF(t)) /* DisplayPort: TU size << 25 | data M */
#define PIPE_DATA_N1(t)       (0x60034 + PIPE_OFF(t))
#define PIPE_LINK_M1(t)       (0x60040 + PIPE_OFF(t)) /* DisplayPort: pixel clock : link clock */
#define PIPE_LINK_N1(t)       (0x60044 + PIPE_OFF(t))
#define TRANS_DDI_FUNC_CTL(t) (0x60400 + PIPE_OFF(t)) /* bit 31 on, 30:28 port, 26:24 mode, 17/16 sync polarity */
#define PS_CTRL(p, i)         (0x68180 + 0x800u * (uint32_t)(p) + 0x100u * (uint32_t)(i)) /* pipe scaler */
#define PS_WIN_POS(p, i)      (0x68170 + 0x800u * (uint32_t)(p) + 0x100u * (uint32_t)(i))
#define PS_WIN_SZ(p, i)       (0x68174 + 0x800u * (uint32_t)(p) + 0x100u * (uint32_t)(i))
#define PIPECONF(p)           (0x70008 + PIPE_OFF(p)) /* bit 31 enable, bit 30 running */
#define PIPE_FRMCOUNT(p)      (0x70040 + PIPE_OFF(p))
#define CUR_CTL(p)            (0x70080 + PIPE_OFF(p))
#define CUR_BASE(p)           (0x70084 + PIPE_OFF(p)) /* writing arms the cursor registers */
#define CUR_POS(p)            (0x70088 + PIPE_OFF(p))
#define CUR_WM(p, level)      (0x70140 + PIPE_OFF(p) + 4u * (uint32_t)(level))
#define CUR_BUF_CFG(p)        (0x7017C + PIPE_OFF(p))
#define PLANE_SURFLIVE(p)     (0x701AC + PIPE_OFF(p)) /* the surface actually on the screen */
#define MASTER_IRQ            0x44200                 /* bit 31: interrupts on; bits 16-18: pipe A-C has something */
#define DE_PIPE_IMR(p)        (0x44404 + 0x10u * (uint32_t)(p))
#define DE_PIPE_IIR(p)        (0x44408 + 0x10u * (uint32_t)(p))
#define DE_PIPE_IER(p)        (0x4440C + 0x10u * (uint32_t)(p))
#define PIPE_VBLANK           (1u << 0)
#define CURSOR_64_ARGB        0x27
#define DDB_BLOCKS            892                     /* generation 9: 896 blocks, 4 of them for the bypass path */
#define CURSOR_DDB_BLOCKS     32
#define PLANE_CTL(p)          (0x70180 + PIPE_OFF(p)) /* primary plane */
#define PLANE_STRIDE(p)       (0x70188 + PIPE_OFF(p)) /* linear: units of 64 bytes */
#define PLANE_POS(p)          (0x7018C + PIPE_OFF(p))
#define PLANE_SIZE(p)         (0x70190 + PIPE_OFF(p)) /* (height - 1) << 16 | (width - 1) */
#define PLANE_SURF(p)         (0x7019C + PIPE_OFF(p)) /* address in the graphics address space; writing arms the plane */
#define PLANE_OFFSET(p)       (0x701A4 + PIPE_OFF(p))
#define PLANE_WM(p, level)    (0x70240 + PIPE_OFF(p) + 4u * (uint32_t)(level))
#define PLANE_BUF_CFG(p)      (0x7027C + PIPE_OFF(p)) /* share of the display buffer: start | end << 16 */
#define DDI_BUF_CTL(port)     (0x64000 + 0x100u * (uint32_t)(port))
#define DP_AUX_CTL(port)      (0x64010 + 0x100u * (uint32_t)(port))
#define DP_AUX_DATA(port, i)  (0x64014 + 0x100u * (uint32_t)(port) + 4u * (uint32_t)(i))
#define DP_TP_CTL(port)       (0x64040 + 0x100u * (uint32_t)(port))
#define TRANS_CLK_SEL(t)      (0x46140 + 4u * (uint32_t)(t))
#define CDCLK_CTL             0x46000
#define LCPLL2_CTL            0x46014 /* DPLL 1 */
#define WRPLL_CTL(i)          (0x46040 + 0x20u * (uint32_t)(i)) /* DPLL 2, 3 */
#define DPLL_CFGCR1(id)       (0x6C040 + ((uint32_t)(id) - 1) * 8)
#define DPLL_CFGCR2(id)       (0x6C044 + ((uint32_t)(id) - 1) * 8)
#define DPLL_CTRL1            0x6C058 /* per DPLL 6 bits: override, link rate, SSC, HDMI mode */
#define DPLL_CTRL2            0x6C05C /* which DPLL feeds which port */
#define DPLL_STATUS           0x6C060
#define PWR_WELL_CTL_BIOS     0x45400
#define PWR_WELL_CTL_DRIVER   0x45404
#define GFX_FLSH_CNTL         0x101008 /* write: the GPU takes over changed GGTT entries */
#define GMBUS0                0xC5100
#define GMBUS1                0xC5104
#define GMBUS2                0xC5108
#define GMBUS3                0xC510C
#define GMBUS5                0xC5120
#define SOUTH_DSPCLK_GATE_D   0xC2020

#define ENABLE                (1u << 31)
#define PIPE_RUNNING          (1u << 30)
#define DDI_MODE(f)           (((f) >> 24) & 7) /* 0 HDMI, 1 DVI, 2 DisplayPort SST */
#define DDI_PORT(f)           (((f) >> 28) & 7)
#define DP_TP_TRAIN_MASK      (7u << 8)
#define DP_TP_TRAIN_IDLE      (2u << 8)
#define DP_TP_TRAIN_NORMAL    (3u << 8)

#define GMBUS_SW_CLR_INT      (1u << 31)
#define GMBUS_SW_RDY          (1u << 30)
#define GMBUS_CYCLE_WAIT      (1u << 25)
#define GMBUS_CYCLE_INDEX     (2u << 25)
#define GMBUS_CYCLE_STOP      (4u << 25)
#define GMBUS_HW_WAIT_PHASE   (1u << 14)
#define GMBUS_HW_RDY          (1u << 11)
#define GMBUS_NAK             (1u << 10)
#define GMBUS_ACTIVE          (1u << 9)
#define GMBUS_CLOCK_GATE_OFF  (1u << 31)

#define AUX_SEND_BUSY         (1u << 31)
#define AUX_DONE              (1u << 30)
#define AUX_TIME_OUT          (1u << 28)
#define AUX_TIME_OUT_MAX      (3u << 26)
#define AUX_RECEIVE_ERROR     (1u << 25)

#define GGTT_OFFSET           (8u << 20) /* the global GTT lies in BAR 0 from 8 MiB on, 64-bit entries */
#define PTE_VALID             1ull
#define PTE_ADDRESS           0x7FFFFFF000ull

#define HDMI_MAX_KHZ          300000 /* generation 9: HDMI up to 300 MHz pixel clock */
#define MAX_MODES             24

typedef struct {
    uint32_t khz;                    /* pixel clock */
    uint32_t ha, hso, hsw, ht;       /* active, sync offset, sync width, total */
    uint32_t va, vso, vsw, vt;
    bool     hpos, vpos, interlaced; /* sync polarity positive */
} timing_t;

/* Everything a mode is in the registers */
typedef struct {
    uint32_t htotal, hblank, hsync, vtotal, vblank, vsync, vsyncshift, pipesrc;
    uint32_t ddi_func, cfgcr1, cfgcr2;
    uint32_t plane_stride, plane_size, plane_surf;
    uint32_t data_m, data_n, link_m, link_n;
} hw_mode_t;

typedef struct {
    pci_device_t     *pci;
    volatile uint8_t *regs;
    volatile uint64_t *ggtt;
    uint32_t          ggtt_entries;
    uint64_t          stolen_base, stolen_size;

    int               pipe;          /* the pipe showing the boot framebuffer */
    int               port, dpll;
    bool              dp, hdmi, scaler;
    uint32_t          source_w, source_h;      /* what the plane shows now */
    uint32_t          link_khz, lanes;         /* DisplayPort link as trained by the firmware */
    uint32_t          pixel_khz;               /* current pixel clock */
    uint32_t          limit_khz;               /* fastest mode possible without retraining or a new display clock */
    uint32_t          pipeconf, plane_ctl, clk_sel, buf_ctl, tp_ctl;
    hw_mode_t         boot;

    uint32_t          boot_buf_cfg;            /* the firmware's share of the display buffer for the plane */

    /* Once the driver owns the screen */
    uint32_t          surfaces[2];             /* the two framebuffers in the graphics address space */
    uint64_t          framebuffers[2];         /* their memory */
    uint32_t          pending_surface;         /* flipped to, not yet confirmed on the screen */
    uint32_t          cursor_surface;
    uint64_t          cursor_phys;
    bool              cursor_visible;
    uint32_t          irq;
    wait_queue_t      vblank_waiters;
    volatile uint64_t vblank_count;

    timing_t          current;                 /* the timing the firmware drives the monitor with */
    timing_t          modes[MAX_MODES];
    uint32_t          mode_count;
} igpu_t;

static uint32_t rd(igpu_t *g, uint32_t reg)
{
    return *(volatile uint32_t *)(g->regs + reg);
}

static void wr(igpu_t *g, uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(g->regs + reg) = value;
}

static uint64_t now_ms(void)
{
    return clock_monotonic_ns() / 1000000;
}

/* Busy-wait until (reg & mask) == want. The clock ticks in milliseconds, so short waits round up. */
static bool wait_bits(igpu_t *g, uint32_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
    uint64_t end = now_ms() + ms + 1;
    while ((rd(g, reg) & mask) != want) {
        if (now_ms() > end)
            return false;
    }
    return true;
}

static void sleep_ms(uint32_t ms)
{
    thread_sleep((uint64_t)ms * 1000000);
}

static void wait_frame(igpu_t *g)
{
    uint32_t frame = rd(g, PIPE_FRMCOUNT(g->pipe));
    uint64_t end = now_ms() + 60;
    while (rd(g, PIPE_FRMCOUNT(g->pipe)) == frame && now_ms() < end)
        ;
}

/* --- Monitor data: GMBUS (HDMI, DVI) -------------------------------------------------- */

static int gmbus_wait(igpu_t *g, uint32_t bits)
{
    uint64_t end = now_ms() + 60;
    for (;;) {
        uint32_t status = rd(g, GMBUS2);
        if (status & GMBUS_NAK)
            return -1;
        if (status & bits)
            return 0;
        if (now_ms() > end)
            return -2;
    }
}

static void gmbus_reset(igpu_t *g)
{
    wait_bits(g, GMBUS2, GMBUS_ACTIVE, 0, 10);
    wr(g, GMBUS1, GMBUS_SW_CLR_INT);
    wr(g, GMBUS1, 0);
    wr(g, GMBUS0, 0);
}

/* Read `length` bytes from offset `index` of I2C device `address` on pin pair `pin`. 0 on success. */
static int gmbus_read(igpu_t *g, uint32_t pin, uint8_t address, uint8_t index, uint8_t *buffer, uint32_t length)
{
    uint32_t gate = rd(g, SOUTH_DSPCLK_GATE_D);
    int result = 0;

    wr(g, SOUTH_DSPCLK_GATE_D, gate | GMBUS_CLOCK_GATE_OFF);
    gmbus_reset(g);
    wr(g, GMBUS5, 0);
    wr(g, GMBUS0, pin); /* 100 kHz */
    wr(g, GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_INDEX | GMBUS_CYCLE_WAIT | (length << 16) | ((uint32_t)index << 8) |
                      ((uint32_t)address << 1) | 1);
    for (uint32_t got = 0; got < length && result == 0;) {
        result = gmbus_wait(g, GMBUS_HW_RDY);
        if (result)
            break;
        uint32_t value = rd(g, GMBUS3);
        for (int k = 0; k < 4 && got < length; k++, value >>= 8)
            buffer[got++] = (uint8_t)value;
    }
    if (result == 0)
        result = gmbus_wait(g, GMBUS_HW_WAIT_PHASE);
    if (result == 0) {
        wr(g, GMBUS1, GMBUS_SW_RDY | GMBUS_CYCLE_STOP);
        wait_bits(g, GMBUS2, GMBUS_ACTIVE, 0, 10);
        wr(g, GMBUS0, 0);
    } else {
        gmbus_reset(g);
    }
    wr(g, SOUTH_DSPCLK_GATE_D, gate);
    return result;
}

static bool edid_block_ok(const uint8_t *block)
{
    uint8_t sum = 0;
    for (int i = 0; i < 128; i++)
        sum = (uint8_t)(sum + block[i]);
    return sum == 0;
}

static bool edid_header_ok(const uint8_t *edid)
{
    return edid[0] == 0 && edid[1] == 0xFF && edid[7] == 0;
}

/* EDID over DDC. Pin pairs: on 300-series chipsets port B, C, D = 1, 2, 3; older boards 5, 4, 6. Returns blocks. */
static int edid_read_ddc(igpu_t *g, int port, uint8_t *edid)
{
    uint32_t wanted = port >= 1 && port <= 3 ? (uint32_t)port : 0, pin = 0;
    uint32_t order[7] = { wanted, 1, 2, 3, 4, 5, 6 };

    for (int i = 0; i < 7 && !pin; i++) {
        uint32_t candidate = order[i];
        if (candidate == 0 || (i > 0 && candidate == wanted))
            continue;
        if (gmbus_read(g, candidate, 0x50, 0, edid, 128) == 0 && edid_header_ok(edid) && edid_block_ok(edid))
            pin = candidate;
    }
    if (!pin)
        return 0;
    if (!edid[126] || gmbus_read(g, pin, 0x50, 128, edid + 128, 128) != 0 || !edid_block_ok(edid + 128))
        return 1;
    return 2;
}

/* --- Monitor data: DisplayPort AUX channel ------------------------------------------- */

/* One AUX message. Returns the bytes received (rx[0] is the reply code), -1 no answer, -2 error, -3 channel stuck. */
static int aux_once(igpu_t *g, int port, const uint8_t *tx, int tx_length, uint8_t *rx, int rx_max)
{
    uint32_t ctl = DP_AUX_CTL(port), status;

    if (!wait_bits(g, ctl, AUX_SEND_BUSY, 0, 10))
        return -3;
    for (int i = 0; i < tx_length; i += 4) {
        uint32_t value = 0;
        for (int k = 0; k < 4; k++)
            value |= (uint32_t)(i + k < tx_length ? tx[i + k] : 0) << (24 - 8 * k);
        wr(g, DP_AUX_DATA(port, i / 4), value);
    }
    /* 32 precharge and sync pulses each, the longest time-out */
    wr(g, ctl, AUX_SEND_BUSY | AUX_DONE | AUX_TIME_OUT | AUX_TIME_OUT_MAX | AUX_RECEIVE_ERROR |
                   (uint32_t)tx_length << 20 | 31u << 5 | 31u);
    if (!wait_bits(g, ctl, AUX_SEND_BUSY, 0, 10))
        return -3;
    status = rd(g, ctl);
    wr(g, ctl, status | AUX_DONE | AUX_TIME_OUT | AUX_RECEIVE_ERROR);
    if (status & AUX_TIME_OUT)
        return -1;
    if (status & AUX_RECEIVE_ERROR)
        return -2;
    int n = (int)((status >> 20) & 0x1F);
    if (n == 0 || n > 20)
        return -2;
    for (int i = 0; i < n && i < rx_max; i += 4) {
        uint32_t value = rd(g, DP_AUX_DATA(port, i / 4));
        for (int k = 0; k < 4 && i + k < n && i + k < rx_max; k++)
            rx[i + k] = (uint8_t)(value >> (24 - 8 * k));
    }
    return n < rx_max ? n : rx_max;
}

/* With retries for receive errors and DEFER (the monitor may put us off). -4: NACK. */
static int aux_transfer(igpu_t *g, int port, const uint8_t *tx, int tx_length, uint8_t *rx, int rx_max, bool i2c)
{
    int timeouts = 0;

    for (int tries = 0; tries < 50; tries++) {
        int n = aux_once(g, port, tx, tx_length, rx, rx_max);
        if (n == -1) {
            if (++timeouts >= 3)
                return -1;
            continue;
        }
        if (n == -3)
            return -3;
        if (n < 0) {
            sleep_ms(1);
            continue;
        }
        uint8_t native = rx[0] & 0x30, i2c_reply = rx[0] & 0xC0;
        if (native == 0x10 || (i2c && i2c_reply == 0x40))
            return -4;
        if (native == 0x20 || (i2c && i2c_reply == 0x80)) {
            sleep_ms(1);
            continue;
        }
        return n;
    }
    return -2;
}

static int dpcd_read(igpu_t *g, int port, uint32_t address, uint8_t *buffer, int length)
{
    uint8_t tx[4] = { (uint8_t)(0x9 << 4 | ((address >> 16) & 0xF)), (uint8_t)(address >> 8), (uint8_t)address,
                      (uint8_t)(length - 1) };
    uint8_t rx[20];
    int n = aux_transfer(g, port, tx, 4, rx, 1 + length, false);

    if (n < 1)
        return n < 0 ? n : -2;
    memcpy(buffer, rx + 1, (size_t)(n - 1));
    return n - 1;
}

/* EDID as I2C over AUX (address 0x50): write offset 0, read in pieces of 16 bytes, stop. Returns blocks. */
static int edid_read_aux_once(igpu_t *g, int port, uint8_t *edid)
{
    uint8_t rx[20];
    uint8_t start[5] = { 0x4 << 4, 0, 0x50, 0, 0 }; /* I2C write, middle of transaction: one byte, offset 0 */
    int want = 128, got = 0;

    if (aux_transfer(g, port, start, 5, rx, 20, true) < 1)
        return 0;
    while (got < want) {
        int length = want - got > 16 ? 16 : want - got;
        uint8_t read[4] = { 0x5 << 4, 0, 0x50, (uint8_t)(length - 1) }; /* I2C read, middle of transaction */
        int n = aux_transfer(g, port, read, 4, rx, 1 + length, true);
        if (n < 2)
            break;
        memcpy(edid + got, rx + 1, (size_t)(n - 1));
        got += n - 1;
        if (got == 128 && edid[126] && edid_header_ok(edid))
            want = 256;
    }
    uint8_t stop[3] = { 0x1 << 4, 0, 0x50 }; /* address only, without "middle of transaction": stop */
    aux_transfer(g, port, stop, 3, rx, 20, true);
    if (got < 128 || !edid_header_ok(edid))
        return 0;
    return got >= 256 ? 2 : 1;
}

/* Right after power-on a monitor may answer incompletely: up to five attempts. */
static int edid_read_aux(igpu_t *g, int port, uint8_t *edid)
{
    int blocks = 0;

    for (int tries = 0; tries < 5; tries++) {
        blocks = edid_read_aux_once(g, port, edid);
        int want = blocks && edid[126] ? 2 : 1;
        if (blocks >= want && edid_block_ok(edid) && (want < 2 || edid_block_ok(edid + 128)))
            return blocks;
        sleep_ms(20);
    }
    return blocks && edid_block_ok(edid) ? 1 : 0;
}

/* --- Timings ------------------------------------------------------------------------- */

/* A detailed timing descriptor (18 bytes); false for the other kinds of descriptors. */
static bool timing_parse(const uint8_t *d, timing_t *t)
{
    t->khz = (uint32_t)(d[0] | d[1] << 8) * 10;
    if (!t->khz)
        return false;
    t->ha = d[2] | (uint32_t)(d[4] & 0xF0) << 4;
    t->ht = t->ha + (d[3] | (uint32_t)(d[4] & 0x0F) << 8);
    t->va = d[5] | (uint32_t)(d[7] & 0xF0) << 4;
    t->vt = t->va + (d[6] | (uint32_t)(d[7] & 0x0F) << 8);
    t->hso = d[8] | (uint32_t)(d[11] & 0xC0) << 2;
    t->hsw = d[9] | (uint32_t)(d[11] & 0x30) << 4;
    t->vso = (uint32_t)(d[10] >> 4) | (uint32_t)(d[11] & 0x0C) << 2;
    t->vsw = (uint32_t)(d[10] & 0xF) | (uint32_t)(d[11] & 0x03) << 4;
    t->interlaced = d[17] >> 7;
    bool separate = ((d[17] >> 3) & 3) == 3; /* digital separate sync; otherwise positive, as usual */
    t->hpos = separate ? (d[17] >> 1) & 1 : true;
    t->vpos = separate ? (d[17] >> 2) & 1 : true;
    return t->ha && t->va && t->ht > t->ha && t->vt > t->va;
}

static uint32_t timing_hz100(const timing_t *t)
{
    return (uint32_t)((uint64_t)t->khz * 100000 / ((uint64_t)t->ht * t->vt));
}

/* The same mode: size, totals and (within 1 %) the pixel clock agree. The same size at another rate is another mode. */
static bool same_timing(const timing_t *a, const timing_t *b)
{
    uint32_t difference = a->khz > b->khz ? a->khz - b->khz : b->khz - a->khz;
    return a->ha == b->ha && a->va == b->va && a->ht == b->ht && a->vt == b->vt && difference <= a->khz / 100;
}

static void add_mode(igpu_t *g, const timing_t *t)
{
    for (uint32_t i = 0; i < g->mode_count; i++) {
        if (same_timing(&g->modes[i], t))
            return;
    }
    if (g->mode_count < MAX_MODES)
        g->modes[g->mode_count++] = *t;
}

/* The detailed timings of the base block and of CTA extension blocks: the modes the monitor itself names. */
static void collect_modes(igpu_t *g, const uint8_t *edid, int blocks)
{
    for (int block = 0; block < blocks; block++) {
        const uint8_t *e = edid + 128 * block;
        uint32_t first = block == 0 ? 54 : e[2], end = block == 0 ? 126 : 127;
        if (block > 0 && (e[0] != 0x02 || first < 4))
            continue;
        for (uint32_t i = first; i + 18 <= end; i += 18) {
            timing_t t;
            if (timing_parse(e + i, &t) && !t.interlaced && t.ha >= 640 && t.va >= 400)
                add_mode(g, &t);
        }
    }
}

/* --- Reading the firmware's state ------------------------------------------------------ */

/* Pixel clock of an HDMI PLL from its configuration registers, kHz. */
static uint32_t hdmi_pll_khz(uint32_t cfgcr1, uint32_t cfgcr2)
{
    static const uint32_t pdiv[] = { 1, 2, 3, 0, 7, 0, 0, 0 }, kdiv[] = { 5, 2, 3, 1 };
    uint64_t dco = 24000ull * (cfgcr1 & 0x1FF) + 24000ull * ((cfgcr1 >> 9) & 0x7FFF) / 0x8000;
    uint32_t p = pdiv[(cfgcr2 >> 2) & 7], k = kdiv[(cfgcr2 >> 5) & 3];
    uint32_t q = (cfgcr2 & (1u << 7)) ? (cfgcr2 >> 8) & 0xFF : 1;
    return p && q && k ? (uint32_t)(dco / (p * q * k) / 5) : 0;
}

static uint32_t pll_ctl_reg(int id)
{
    return id == 1 ? LCPLL2_CTL : WRPLL_CTL(id - 2);
}

static void mode_read(igpu_t *g, hw_mode_t *m)
{
    int p = g->pipe;

    m->htotal = rd(g, HTOTAL(p));
    m->hblank = rd(g, HBLANK(p));
    m->hsync = rd(g, HSYNC(p));
    m->vtotal = rd(g, VTOTAL(p));
    m->vblank = rd(g, VBLANK(p));
    m->vsync = rd(g, VSYNC(p));
    m->vsyncshift = rd(g, VSYNCSHIFT(p));
    m->pipesrc = rd(g, PIPESRC(p));
    m->ddi_func = rd(g, TRANS_DDI_FUNC_CTL(p));
    m->cfgcr1 = g->dpll >= 1 ? rd(g, DPLL_CFGCR1(g->dpll)) : 0;
    m->cfgcr2 = g->dpll >= 1 ? rd(g, DPLL_CFGCR2(g->dpll)) : 0;
    m->data_m = rd(g, PIPE_DATA_M1(p));
    m->data_n = rd(g, PIPE_DATA_N1(p));
    m->link_m = rd(g, PIPE_LINK_M1(p));
    m->link_n = rd(g, PIPE_LINK_N1(p));
    m->plane_stride = rd(g, PLANE_STRIDE(p));
    m->plane_size = rd(g, PLANE_SIZE(p));
    m->plane_surf = rd(g, PLANE_SURF(p)) & ~0xFFFu;
}

/* Find the pipe that shows the boot framebuffer and read how it is driven. */
static bool read_state(igpu_t *g)
{
    const boot_framebuffer_t *fb = &boot_info()->framebuffer;
    uint64_t aperture = g->pci->bars[2].phys;
    uint32_t wells = rd(g, PWR_WELL_CTL_BIOS) | rd(g, PWR_WELL_CTL_DRIVER);

    g->pipe = -1;
    for (int p = 0; p < 3; p++) {
        if (p > 0 && !(wells & (1u << 30))) /* pipes B and C are in power well 2: do not touch them without power */
            break;
        uint32_t conf = rd(g, PIPECONF(p)), ctl = rd(g, PLANE_CTL(p));
        uint32_t surface = rd(g, PLANE_SURF(p)) & ~0xFFFu;
        if (!(conf & ENABLE))
            continue;
        klog_info("igpu: pipe %c: on, plane %s, surface 0x%x", 'A' + p, (ctl & ENABLE) ? "on" : "off", surface);
        if ((ctl & ENABLE) && aperture && fb->phys_base == aperture + surface)
            g->pipe = p;
    }
    if (g->pipe < 0) {
        klog_warn("igpu: no pipe shows the boot framebuffer (0x%lx, aperture 0x%lx): nothing to take over",
                  fb->phys_base, aperture);
        return false;
    }

    int p = g->pipe;
    uint32_t ddi = rd(g, TRANS_DDI_FUNC_CTL(p)), control1 = rd(g, DPLL_CTRL1), control2 = rd(g, DPLL_CTRL2);
    g->port = (int)DDI_PORT(ddi);
    g->dpll = (control2 & (1u << (g->port * 3))) ? (int)((control2 >> (g->port * 3 + 1)) & 3) : -1;
    g->pipeconf = rd(g, PIPECONF(p));
    g->plane_ctl = rd(g, PLANE_CTL(p));
    g->clk_sel = rd(g, TRANS_CLK_SEL(p));
    g->buf_ctl = rd(g, DDI_BUF_CTL(g->port));
    g->tp_ctl = rd(g, DP_TP_CTL(g->port));
    g->scaler = (rd(g, PS_CTRL(p, 0)) | rd(g, PS_CTRL(p, 1))) & ENABLE;
    g->boot_buf_cfg = rd(g, PLANE_BUF_CFG(p));
    mode_read(g, &g->boot);
    g->source_w = (g->boot.pipesrc >> 16) + 1;
    g->source_h = (g->boot.pipesrc & 0xFFFF) + 1;

    uint32_t cdclk_khz = ((rd(g, CDCLK_CTL) & 0x7FF) + 2) * 500;
    if ((ddi & ENABLE) && DDI_MODE(ddi) == 2 && g->dpll >= 0) {
        static const uint32_t rates[] = { 540000, 270000, 162000, 324000, 216000, 432000 }; /* link symbol clock, kHz */
        uint32_t code = (control1 >> (g->dpll * 6 + 1)) & 7;
        g->dp = true;
        g->link_khz = code < 6 ? rates[code] : 0;
        g->lanes = ((ddi >> 1) & 7) + 1;
        if (g->link_khz && g->boot.link_n)
            g->pixel_khz = (uint32_t)((uint64_t)g->link_khz * g->boot.link_m / g->boot.link_n);
        /* 24 bits per pixel over 8b/10b lanes; one pixel per pipe clock; the display clock bounds the pipe */
        g->limit_khz = (uint32_t)((uint64_t)g->link_khz * 8 * g->lanes / 24);
        if (g->link_khz < g->limit_khz)
            g->limit_khz = g->link_khz;
    } else if ((ddi & ENABLE) && DDI_MODE(ddi) <= 1 && g->dpll >= 1 && ((control1 >> (g->dpll * 6 + 5)) & 1)) {
        g->hdmi = true;
        g->pixel_khz = hdmi_pll_khz(g->boot.cfgcr1, g->boot.cfgcr2);
        g->limit_khz = HDMI_MAX_KHZ;
    }
    if (cdclk_khz < g->limit_khz)
        g->limit_khz = cdclk_khz;

    timing_t *now = &g->current;
    now->khz = g->pixel_khz;
    now->ha = (g->boot.htotal & 0xFFFF) + 1;
    now->ht = (g->boot.htotal >> 16) + 1;
    now->hso = (g->boot.hsync & 0xFFFF) + 1 - now->ha;
    now->hsw = (g->boot.hsync >> 16) - (g->boot.hsync & 0xFFFF);
    now->va = (g->boot.vtotal & 0xFFFF) + 1;
    now->vt = (g->boot.vtotal >> 16) + 1;
    now->vso = (g->boot.vsync & 0xFFFF) + 1 - now->va;
    now->vsw = (g->boot.vsync >> 16) - (g->boot.vsync & 0xFFFF);
    now->hpos = (g->boot.ddi_func >> 16) & 1;
    now->vpos = (g->boot.ddi_func >> 17) & 1;
    uint32_t hz = now->khz ? (uint32_t)((uint64_t)now->khz * 100000 / ((uint64_t)now->ht * now->vt)) : 0;

    klog_info("igpu: pipe %c shows the boot framebuffer %ux%u; port %c, %s", 'A' + p, g->source_w, g->source_h,
              'A' + g->port, g->dp ? "DisplayPort" : g->hdmi ? "HDMI" : "unknown connection");
    klog_info("igpu: timing %ux%u at %u.%02u Hz (total %ux%u), pixel clock %u kHz, display clock %u kHz%s", now->ha,
              now->va, hz / 100, hz % 100, now->ht, now->vt, g->pixel_khz, cdclk_khz,
              g->scaler ? ", the pipe scaler is on (a smaller picture scaled up)" : "");
    if (now->khz && now->ht > now->ha && now->vt > now->va)
        add_mode(g, now); /* always a candidate: it is known to work */
    if (g->dp)
        klog_info("igpu: DisplayPort link: %u lane%s at %u.%02u Gbit/s; modes up to %u kHz pixel clock without retraining",
                  g->lanes, g->lanes == 1 ? "" : "s", g->link_khz / 100000, g->link_khz / 1000 % 100, g->limit_khz);
    return g->dp || g->hdmi;
}

/* --- Switching modes ------------------------------------------------------------------- */

/* PLL settings for an HDMI pixel clock (after i915's skl_ddi_hdmi_pll_dividers). */
static bool hdmi_pll_compute(uint32_t khz, uint32_t *cfgcr1, uint32_t *cfgcr2)
{
    static const uint32_t even[] = { 4,  6,  8,  10, 12, 14, 16, 18, 20, 24, 28, 30, 32, 36, 40, 42, 44, 48,
                                     52, 54, 56, 60, 64, 66, 68, 70, 72, 76, 78, 80, 84, 88, 90, 92, 96, 98 };
    static const uint32_t odd[] = { 3, 5, 7, 9, 15, 21, 35 };
    static const uint64_t central[] = { 8400000000ull, 9000000000ull, 9600000000ull };
    uint64_t afe = (uint64_t)khz * 1000 * 5, best_deviation = ~0ull, best_central = 0;
    uint32_t best = 0;

    /* The oscillator runs near 8.4, 9.0 or 9.6 GHz (at most 1 % above, 6 % below); even dividers first. */
    for (int pass = 0; pass < 2 && !best; pass++) {
        const uint32_t *list = pass == 0 ? even : odd;
        size_t count = pass == 0 ? sizeof(even) / sizeof(even[0]) : sizeof(odd) / sizeof(odd[0]);
        for (size_t i = 0; i < count; i++) {
            for (int c = 0; c < 3; c++) {
                uint64_t dco = afe * list[i];
                uint64_t difference = dco > central[c] ? dco - central[c] : central[c] - dco;
                uint64_t deviation = difference * 10000 / central[c]; /* in 0.01 % */
                if (dco >= central[c] ? deviation >= 100 : deviation >= 600)
                    continue;
                if (deviation < best_deviation) {
                    best_deviation = deviation;
                    best = list[i];
                    best_central = central[c];
                }
            }
        }
    }
    if (!best)
        return false;

    uint32_t p0 = 0, p1 = 0, p2 = 0;
    if (best % 2 == 0) {
        uint32_t half = best / 2;
        if (half == 1 || half == 2 || half == 3 || half == 5) {
            p0 = 2, p1 = 1, p2 = half;
        } else if (half % 2 == 0) {
            p0 = 2, p1 = half / 2, p2 = 2;
        } else if (half % 3 == 0) {
            p0 = 3, p1 = half / 3, p2 = 2;
        } else if (half % 7 == 0) {
            p0 = 7, p1 = half / 7, p2 = 2;
        }
    } else if (best == 3 || best == 9) {
        p0 = 3, p1 = 1, p2 = best / 3;
    } else if (best == 5) {
        p0 = 1, p1 = 1, p2 = 5;
    } else if (best == 7) {
        p0 = 7, p1 = 1, p2 = 1;
    } else if (best == 15) {
        p0 = 3, p1 = 1, p2 = 5;
    } else if (best == 21) {
        p0 = 7, p1 = 1, p2 = 3;
    } else if (best == 35) {
        p0 = 7, p1 = 1, p2 = 5;
    }
    if (!p0)
        return false;
    uint32_t pdiv = p0 == 1 ? 0 : p0 == 2 ? 1 : p0 == 3 ? 2 : 4;
    uint32_t kdiv = p2 == 5 ? 0 : p2 == 2 ? 1 : p2 == 3 ? 2 : 3;
    uint32_t frequency = best_central == 9600000000ull ? 0 : best_central == 9000000000ull ? 1 : 3;
    uint64_t dco = afe * p0 * p1 * p2;
    uint64_t integer = dco / 24000000ull;
    uint64_t fraction = (dco / 24 - integer * 1000000ull) * 0x8000 / 1000000ull;
    *cfgcr1 = ENABLE | (uint32_t)(fraction & 0x7FFF) << 9 | (uint32_t)(integer & 0x1FF);
    *cfgcr2 = (p1 != 1 ? (p1 & 0xFF) << 8 | 1u << 7 : 0) | kdiv << 5 | pdiv << 2 | frequency;
    return true;
}

/* DisplayPort M/N values (as i915's compute_m_n): ratios with N a power of two, at most 24 bits. */
static void compute_m_n(uint64_t m, uint64_t n, uint32_t *result_m, uint32_t *result_n)
{
    uint64_t big_n = 1;
    while (big_n < n)
        big_n <<= 1;
    if (big_n > 0x800000)
        big_n = 0x800000;
    uint64_t big_m = m * big_n / n;
    while (big_m > 0xFFFFFF) {
        big_m >>= 1;
        big_n >>= 1;
    }
    *result_m = (uint32_t)big_m;
    *result_n = (uint32_t)big_n;
}

/* The register values of timing t with a plane of the same size from `surface` (`stride` bytes per line). */
static void mode_fill(igpu_t *g, const timing_t *t, uint32_t surface, uint32_t stride, hw_mode_t *m)
{
    *m = g->boot;
    m->htotal = (t->ht - 1) << 16 | (t->ha - 1);
    m->hblank = (t->ht - 1) << 16 | (t->ha - 1);
    m->hsync = (t->ha + t->hso + t->hsw - 1) << 16 | (t->ha + t->hso - 1);
    m->vtotal = (t->vt - 1) << 16 | (t->va - 1);
    m->vblank = (t->vt - 1) << 16 | (t->va - 1);
    m->vsync = (t->va + t->vso + t->vsw - 1) << 16 | (t->va + t->vso - 1);
    m->vsyncshift = 0;
    m->pipesrc = (t->ha - 1) << 16 | (t->va - 1);
    m->ddi_func = (g->boot.ddi_func & ~(3u << 16)) | (t->hpos ? 1u << 16 : 0) | (t->vpos ? 1u << 17 : 0);
    m->plane_stride = stride / 64;
    m->plane_size = (t->va - 1) << 16 | (t->ha - 1);
    m->plane_surf = surface;
    if (g->dp) {
        uint32_t data_m;
        compute_m_n(24ull * t->khz, (uint64_t)g->link_khz * g->lanes * 8, &data_m, &m->data_n);
        m->data_m = (63u << 25) | data_m; /* transfer units of 64 symbols */
        compute_m_n(t->khz, g->link_khz, &m->link_m, &m->link_n);
    }
}

/*
 * Watermarks for a plane `width` pixels wide at pixel clock khz: level 0 (the
 * one that must hold) for one line of latency, the power saving levels off.
 * The plane keeps the share of the display buffer the firmware gave it.
 */
static void write_watermarks(igpu_t *g, uint32_t width)
{
    int p = g->pipe;
    uint32_t share = rd(g, PLANE_BUF_CFG(p));
    uint32_t blocks_available = ((share >> 16) & 0x3FF) - (share & 0x3FF) + 1;
    uint32_t blocks = (width * 4 + 511) / 512 + 1; /* one line in blocks of 512 bytes, plus one */

    if (blocks >= blocks_available)
        blocks = blocks_available - 1;
    wr(g, PLANE_WM(p, 0), ENABLE | 1u << 14 | blocks);
    for (int level = 1; level < 8; level++)
        wr(g, PLANE_WM(p, level), 0);
}

static void scalers_off(igpu_t *g)
{
    for (int i = 0; i < 2; i++) {
        wr(g, PS_CTRL(g->pipe, i), 0);
        wr(g, PS_WIN_POS(g->pipe, i), 0);
        wr(g, PS_WIN_SZ(g->pipe, i), 0); /* writing the size arms the scaler registers */
    }
}

static void write_timings_and_plane(igpu_t *g, const hw_mode_t *m, bool boot_mode)
{
    int p = g->pipe;

    wr(g, HTOTAL(p), m->htotal);
    wr(g, HBLANK(p), m->hblank);
    wr(g, HSYNC(p), m->hsync);
    wr(g, VTOTAL(p), m->vtotal);
    wr(g, VBLANK(p), m->vblank);
    wr(g, VSYNC(p), m->vsync);
    wr(g, VSYNCSHIFT(p), m->vsyncshift);
    wr(g, PIPESRC(p), m->pipesrc);
    if (g->dp) {
        wr(g, PIPE_DATA_M1(p), m->data_m);
        wr(g, PIPE_DATA_N1(p), m->data_n);
        wr(g, PIPE_LINK_M1(p), m->link_m);
        wr(g, PIPE_LINK_N1(p), m->link_n);
    }
    wr(g, TRANS_DDI_FUNC_CTL(p), m->ddi_func);
    if (!boot_mode) {
        scalers_off(g);
        /* The pipe is off: split the display buffer anew, the plane first, the last blocks for the cursor. */
        wr(g, PLANE_BUF_CFG(p), (DDB_BLOCKS - CURSOR_DDB_BLOCKS - 1) << 16);
        write_watermarks(g, (m->plane_size & 0xFFFF) + 1);
    } else {
        wr(g, PLANE_BUF_CFG(p), g->boot_buf_cfg);
    }
    wr(g, PLANE_STRIDE(p), m->plane_stride);
    wr(g, PLANE_POS(p), 0);
    wr(g, PLANE_OFFSET(p), 0);
    wr(g, PLANE_SIZE(p), m->plane_size);
    wr(g, PLANE_CTL(p), g->plane_ctl);
    wr(g, PLANE_SURF(p), m->plane_surf);
}

/* Planes, pipe and transcoder off. DisplayPort: the link stays up and sends idle patterns. */
static void pipe_off(igpu_t *g)
{
    int p = g->pipe;

    wr(g, PLANE_CTL(p), g->plane_ctl & ~ENABLE);
    wr(g, PLANE_SURF(p), rd(g, PLANE_SURF(p)));
    wr(g, CUR_CTL(p), 0);
    wr(g, CUR_BASE(p), rd(g, CUR_BASE(p)));
    wait_frame(g);
    if (g->dp) {
        wr(g, DP_TP_CTL(g->port), (g->tp_ctl & ~DP_TP_TRAIN_MASK) | DP_TP_TRAIN_IDLE);
        sleep_ms(2);
    }
    wr(g, PIPECONF(p), g->pipeconf & ~ENABLE);
    if (!wait_bits(g, PIPECONF(p), PIPE_RUNNING, 0, 100))
        klog_warn("igpu: the pipe does not stop (PIPECONF 0x%x)", rd(g, PIPECONF(p)));
    wr(g, TRANS_DDI_FUNC_CTL(p), rd(g, TRANS_DDI_FUNC_CTL(p)) & ~(ENABLE | 7u << 28));
    if (g->hdmi) {
        /* HDMI changes the pixel clock, which is the port's PLL: clock, port and PLL off as well. */
        wr(g, TRANS_CLK_SEL(p), 0);
        wr(g, DDI_BUF_CTL(g->port), g->buf_ctl & ~ENABLE);
        sleep_ms(1);
        wr(g, DPLL_CTRL2, rd(g, DPLL_CTRL2) | 1u << (g->port + 15));
        wr(g, pll_ctl_reg(g->dpll), rd(g, pll_ctl_reg(g->dpll)) & ~ENABLE);
        (void)rd(g, pll_ctl_reg(g->dpll));
    }
}

/* The reverse, with mode m. True if the pipe runs. */
static bool pipe_on(igpu_t *g, const hw_mode_t *m, bool boot_mode)
{
    int p = g->pipe;
    bool ok = true;

    if (g->hdmi) {
        wr(g, DPLL_CFGCR1(g->dpll), m->cfgcr1);
        wr(g, DPLL_CFGCR2(g->dpll), m->cfgcr2);
        (void)rd(g, DPLL_CFGCR2(g->dpll));
        wr(g, pll_ctl_reg(g->dpll), rd(g, pll_ctl_reg(g->dpll)) | ENABLE);
        if (!wait_bits(g, DPLL_STATUS, 1u << (g->dpll * 8), 1u << (g->dpll * 8), 5)) {
            klog_warn("igpu: PLL %d does not lock (status 0x%x)", g->dpll, rd(g, DPLL_STATUS));
            ok = false;
        }
        wr(g, DPLL_CTRL2, rd(g, DPLL_CTRL2) & ~(1u << (g->port + 15)));
        wr(g, TRANS_CLK_SEL(p), g->clk_sel);
    }
    write_timings_and_plane(g, m, boot_mode);
    wr(g, PIPECONF(p), g->pipeconf | ENABLE);
    if (!wait_bits(g, PIPECONF(p), PIPE_RUNNING, PIPE_RUNNING, 100)) {
        klog_warn("igpu: the pipe does not start (PIPECONF 0x%x)", rd(g, PIPECONF(p)));
        ok = false;
    }
    if (g->dp) {
        wr(g, DP_TP_CTL(g->port), (g->tp_ctl & ~DP_TP_TRAIN_MASK) | DP_TP_TRAIN_NORMAL);
    } else {
        wr(g, DDI_BUF_CTL(g->port), g->buf_ctl | ENABLE);
        sleep_ms(1);
    }
    return ok;
}

/* Does the screen actually refresh? Counts frames for a fifth of a second. */
static bool frames_running(igpu_t *g)
{
    uint32_t before = rd(g, PIPE_FRMCOUNT(g->pipe));
    sleep_ms(200);
    return rd(g, PIPE_FRMCOUNT(g->pipe)) - before >= 3;
}

/*
 * `bytes` bytes of contiguous memory, cleared, entered into the global
 * graphics translation table from entry `first` on. Returns the address in
 * the graphics address space, 0 on failure.
 *
 * The memory is cleared through the kernel's cached direct map, while it is
 * later written through write-combining mappings: the zeros are pushed out
 * of the cache at once, or cache lines written back later would wipe out
 * newer pixels (the display engine reads memory, not the cache).
 */
static uint32_t graphics_memory_alloc(igpu_t *g, uint64_t bytes, uint32_t first, uint64_t *phys)
{
    uint32_t pages = (uint32_t)(align_up(bytes, PAGE_SIZE) / PAGE_SIZE);

    if (!g->ggtt || first + pages > g->ggtt_entries) {
        klog_warn("igpu: the graphics translation table is too small");
        return 0;
    }
    /* The firmware enters only what it uses (its framebuffer in stolen memory); the rest is whatever was there. */
    for (uint32_t i = 0; i < pages; i++) {
        uint64_t entry = g->ggtt[first + i];
        if ((entry & PTE_VALID) && (entry & PTE_ADDRESS) >= g->stolen_base &&
            (entry & PTE_ADDRESS) < g->stolen_base + g->stolen_size) {
            klog_warn("igpu: translation table entry 0x%x is in use (0x%lx)", first + i, entry);
            return 0;
        }
    }
    if (STATUS_IS_ERROR(pmm_alloc_pages(pages, phys))) {
        klog_warn("igpu: no %u KiB of contiguous memory", (uint32_t)(bytes >> 10));
        return 0;
    }
    memset(phys_to_virt(*phys), 0, (size_t)pages * PAGE_SIZE);
    for (uint64_t offset = 0; offset < (uint64_t)pages * PAGE_SIZE; offset += 64)
        __asm__ volatile("clflush (%0)" : : "r"((uint8_t *)phys_to_virt(*phys) + offset) : "memory");
    __asm__ volatile("mfence" : : : "memory");

    for (uint32_t i = 0; i < pages; i++)
        g->ggtt[first + i] = (*phys + (uint64_t)i * PAGE_SIZE) | PTE_VALID;
    wr(g, GFX_FLSH_CNTL, 1);
    (void)rd(g, GFX_FLSH_CNTL);
    return first << 12;
}

/* --- Driver operations for the display layer ------------------------------------------ */

static igpu_t *igpu_of(display_t *display)
{
    return display->driver_data;
}

static void igpu_interrupt(void *context)
{
    igpu_t *g = context;
    uint32_t master = rd(g, MASTER_IRQ);

    /* As i915 does it: master bit off, read and acknowledge the cause, master bit on. */
    wr(g, MASTER_IRQ, 0);
    if (master & (1u << (16 + g->pipe))) {
        uint32_t cause = rd(g, DE_PIPE_IIR(g->pipe));
        if (cause & PIPE_VBLANK) {
            wr(g, DE_PIPE_IIR(g->pipe), PIPE_VBLANK);
            g->vblank_count++;
            wait_queue_wake_all(&g->vblank_waiters, STATUS_SUCCESS);
        }
    }
    wr(g, MASTER_IRQ, ENABLE);
}

static status_t igpu_wait_vblank(display_t *display, uint64_t timeout_ns)
{
    igpu_t *g = igpu_of(display);
    uint64_t deadline = wait_deadline(timeout_ns);
    status_t status = STATUS_SUCCESS;

    /* One vertical blank; after a flip one more if the new surface is not on the screen yet. */
    for (int round = 0; round < 3 && status == STATUS_SUCCESS; round++) {
        uint64_t flags = arch_interrupts_save();
        uint64_t seen = g->vblank_count;
        while (g->vblank_count == seen && status == STATUS_SUCCESS)
            status = wait_queue_block(&g->vblank_waiters, deadline);
        arch_interrupts_restore(flags);
        if (!g->pending_surface || (rd(g, PLANE_SURFLIVE(g->pipe)) & ~0xFFFu) == g->pending_surface)
            break;
    }
    g->pending_surface = 0;
    return status;
}

static status_t igpu_flip(display_t *display, uint32_t buffer)
{
    igpu_t *g = igpu_of(display);

    if (!g->surfaces[buffer])
        return STATUS_NOT_SUPPORTED;
    g->pending_surface = g->surfaces[buffer];
    wr(g, PLANE_SURF(g->pipe), g->surfaces[buffer]); /* takes effect at the next vertical blank */
    return STATUS_SUCCESS;
}

static status_t igpu_cursor_image(display_t *display, const uint32_t *pixels)
{
    igpu_t *g = igpu_of(display);
    size_t bytes = JELLY_CURSOR_SIZE * JELLY_CURSOR_SIZE * sizeof(uint32_t);
    uint8_t *image = phys_to_virt(g->cursor_phys);

    memcpy(image, pixels, bytes);
    for (size_t offset = 0; offset < bytes; offset += 64)
        __asm__ volatile("clflush (%0)" : : "r"(image + offset) : "memory");
    __asm__ volatile("mfence" : : : "memory");
    wr(g, CUR_BASE(g->pipe), g->cursor_surface);
    return STATUS_SUCCESS;
}

static void igpu_cursor_move(display_t *display, int32_t x, int32_t y, bool visible)
{
    igpu_t *g = igpu_of(display);
    uint32_t position = 0;

    /* Sign and magnitude: the image may hang over the left and top edges. */
    position |= x < 0 ? (1u << 15) | ((uint32_t)-x & 0xFFF) : ((uint32_t)x & 0xFFF);
    position |= y < 0 ? (1u << 31) | (((uint32_t)-y & 0xFFF) << 16) : (((uint32_t)y & 0xFFF) << 16);
    if (visible != g->cursor_visible) {
        wr(g, CUR_CTL(g->pipe), visible ? CURSOR_64_ARGB : 0);
        g->cursor_visible = visible;
    }
    wr(g, CUR_POS(g->pipe), position);
    wr(g, CUR_BASE(g->pipe), g->cursor_surface); /* position and visibility apply at the next frame */
}

/* The cursor plane: its image in graphics memory, its share of the display buffer and a watermark. */
static bool cursor_setup(igpu_t *g, uint32_t first_entry)
{
    int p = g->pipe;
    uint32_t plane_end = (rd(g, PLANE_BUF_CFG(p)) >> 16) & 0x3FF;
    uint32_t start = plane_end + 1, end = start + CURSOR_DDB_BLOCKS - 1;

    if (end >= DDB_BLOCKS) {
        klog_info("igpu: no room in the display buffer for the cursor plane (the plane ends at block %u)", plane_end);
        return false;
    }
    g->cursor_surface = graphics_memory_alloc(g, JELLY_CURSOR_SIZE * JELLY_CURSOR_SIZE * 4, first_entry, &g->cursor_phys);
    if (!g->cursor_surface)
        return false;
    wr(g, CUR_BUF_CFG(p), end << 16 | start);
    wr(g, CUR_WM(p, 0), ENABLE | 1u << 14 | 8);
    for (int level = 1; level < 8; level++)
        wr(g, CUR_WM(p, level), 0);
    wr(g, CUR_CTL(p), 0); /* invisible until the display server shows it */
    wr(g, CUR_BASE(p), g->cursor_surface);
    return true;
}

/* The pipe's vertical blank interrupt. False if it cannot be had (the driver then offers no timing or flipping). */
static bool vblank_setup(igpu_t *g)
{
    int p = g->pipe;

    wait_queue_init(&g->vblank_waiters);
    if (STATUS_IS_ERROR(pci_enable_msi(g->pci, igpu_interrupt, g, &g->irq))) {
        klog_info("igpu: no MSI interrupt: no vertical blank timing");
        return false;
    }
    wr(g, DE_PIPE_IIR(p), PIPE_VBLANK);
    wr(g, DE_PIPE_IER(p), rd(g, DE_PIPE_IER(p)) | PIPE_VBLANK);
    wr(g, DE_PIPE_IMR(p), rd(g, DE_PIPE_IMR(p)) & ~PIPE_VBLANK);
    wr(g, MASTER_IRQ, ENABLE);

    /* Does it arrive? Two frames at least in a tenth of a second. */
    uint64_t before = g->vblank_count;
    sleep_ms(100);
    if (g->vblank_count < before + 2) {
        klog_warn("igpu: the vertical blank interrupt does not arrive (IIR 0x%x, master 0x%x)", rd(g, DE_PIPE_IIR(p)),
                  rd(g, MASTER_IRQ));
        wr(g, DE_PIPE_IMR(p), rd(g, DE_PIPE_IMR(p)) | PIPE_VBLANK);
        wr(g, DE_PIPE_IER(p), rd(g, DE_PIPE_IER(p)) & ~PIPE_VBLANK);
        wr(g, MASTER_IRQ, 0);
        pci_disable_msi(g->pci);
        return false;
    }
    return true;
}

static display_ops_t igpu_ops; /* filled with what works on this machine */

/* Switch to timing t. True on success; on failure the firmware's mode is back. */
static bool mode_set(igpu_t *g, const timing_t *t)
{
    uint32_t stride = (uint32_t)align_up((uint64_t)t->ha * 4, 64);
    uint64_t bytes = (uint64_t)stride * t->va;
    uint32_t pages = (uint32_t)(align_up(bytes, PAGE_SIZE) / PAGE_SIZE), base = g->ggtt_entries / 4;
    bool unchanged = same_timing(t, &g->current);
    hw_mode_t mode;

    if ((g->plane_ctl & (7u << 10)) || ((g->plane_ctl >> 24) & 0xF) != 4) {
        klog_warn("igpu: the firmware's plane is not linear 32-bit RGB (PLANE_CTL 0x%x): not touched", g->plane_ctl);
        return false;
    }
    /* At 1 GiB of the graphics address space, far from the firmware's entries: two framebuffers, then the cursor. */
    g->surfaces[0] = graphics_memory_alloc(g, bytes, base, &g->framebuffers[0]);
    if (!g->surfaces[0])
        return false;
    uint32_t surface = g->surfaces[0];

    if (unchanged) {
        /* The monitor already gets this timing: everything here latches at the next frame, the pipe keeps running. */
        klog_info("igpu: the monitor already runs at this timing: %s",
                  g->scaler ? "turning the scaler off" : "taking the plane over");
        mode = g->boot;
        mode.pipesrc = (t->ha - 1) << 16 | (t->va - 1);
        mode.plane_stride = stride / 64;
        mode.plane_size = (t->va - 1) << 16 | (t->ha - 1);
        mode.plane_surf = surface;
        scalers_off(g);
        write_watermarks(g, t->ha);
        wr(g, PIPESRC(g->pipe), mode.pipesrc);
        wr(g, PLANE_STRIDE(g->pipe), mode.plane_stride);
        wr(g, PLANE_POS(g->pipe), 0);
        wr(g, PLANE_OFFSET(g->pipe), 0);
        wr(g, PLANE_SIZE(g->pipe), mode.plane_size);
        wr(g, PLANE_SURF(g->pipe), mode.plane_surf);
        wait_frame(g);
        wait_frame(g);
    } else {
        bool possible = t->khz <= g->limit_khz;
        if (!possible)
            klog_warn("igpu: %ux%u needs a pixel clock of %u kHz, possible are %u kHz", t->ha, t->va, t->khz,
                      g->limit_khz);
        mode_fill(g, t, surface, stride, &mode);
        if (possible && g->hdmi && !hdmi_pll_compute(t->khz, &mode.cfgcr1, &mode.cfgcr2)) {
            klog_warn("igpu: no PLL setting for %u kHz", t->khz);
            possible = false;
        }
        if (!possible) {
            pmm_free_pages(g->framebuffers[0], pages);
            g->surfaces[0] = 0;
            return false;
        }
        klog_info("igpu: switching the %s to %ux%u, pixel clock %u kHz", g->dp ? "DisplayPort timing" : "HDMI port",
                  t->ha, t->va, t->khz);
        pipe_off(g);
        if (!pipe_on(g, &mode, false) || !frames_running(g)) {
            klog_warn("igpu: %ux%u does not come up: back to the firmware's mode", t->ha, t->va);
            pipe_off(g);
            pipe_on(g, &g->boot, true);
            g->surfaces[0] = 0;
            return false;
        }
        g->current = *t;
    }

    status_t status = display_set_framebuffer(0, g->framebuffers[0], t->ha, t->va, stride);
    if (STATUS_IS_ERROR(status)) {
        klog_warn("igpu: the display does not take the new framebuffer (%s)", status_name(status));
        return false;
    }
    uint32_t hz = timing_hz100(t);
    klog_info("igpu: now %ux%u at %u.%02u Hz, framebuffer of %u MiB at 0x%lx", t->ha, t->va, hz / 100, hz % 100,
              (uint32_t)(bytes >> 20), g->framebuffers[0]);

    /* What else the hardware can do for the display server; each part on its own. */
    if (vblank_setup(g)) {
        igpu_ops.wait_vblank = igpu_wait_vblank;
        g->surfaces[1] = graphics_memory_alloc(g, bytes, base + pages, &g->framebuffers[1]);
        if (g->surfaces[1])
            igpu_ops.flip = igpu_flip; /* flipping needs the interrupt to know when a flip has happened */
    }
    if (cursor_setup(g, base + 2 * pages)) {
        igpu_ops.cursor_image = igpu_cursor_image;
        igpu_ops.cursor_move = igpu_cursor_move;
    }
    display_set_driver(0, &igpu_ops, g, g->surfaces[1] ? g->framebuffers[1] : 0);
    return true;
}

/* "igpu=native" or "igpu=WIDTHxHEIGHT[@HZ]": the mode to switch to, NULL if there is none. */
static const timing_t *choose_mode(igpu_t *g, const char *option)
{
    uint32_t width = 0, height = 0, hz = 0;
    const timing_t *best = NULL;
    const char *s = option;

    if (strcmp(option, "native") != 0) {
        while (*s >= '0' && *s <= '9')
            width = width * 10 + (uint32_t)(*s++ - '0');
        if (*s == 'x')
            s++;
        while (*s >= '0' && *s <= '9')
            height = height * 10 + (uint32_t)(*s++ - '0');
        if (*s == '@') {
            s++;
            while (*s >= '0' && *s <= '9')
                hz = hz * 10 + (uint32_t)(*s++ - '0');
        }
        if (!width || !height) {
            klog_warn("igpu: igpu=%s is neither native nor WIDTHxHEIGHT[@HZ]", option);
            return NULL;
        }
    }
    for (uint32_t i = 0; i < g->mode_count; i++) {
        const timing_t *t = &g->modes[i];
        /* The timing the firmware already uses works in any case; everything else must fit the connection. */
        bool reachable = t->khz <= g->limit_khz || same_timing(t, &g->current);
        uint32_t rate = (timing_hz100(t) + 50) / 100;
        if (!reachable || (width && (t->ha != width || t->va != height)) || (hz && rate != hz))
            continue;
        if (!best || (uint64_t)t->ha * t->va > (uint64_t)best->ha * best->va ||
            ((uint64_t)t->ha * t->va == (uint64_t)best->ha * best->va && timing_hz100(t) > timing_hz100(best)))
            best = t;
    }
    if (!best)
        klog_warn("igpu: igpu=%s: no such mode among those the monitor names and the connection allows", option);
    return best;
}

/* --- Start ----------------------------------------------------------------------------- */

static bool generation9(uint16_t device)
{
    uint16_t family = device & 0xFF00;
    return family == 0x1900 || family == 0x5900 || family == 0x3E00 || family == 0x8700 || family == 0x9B00;
}

static status_t igpu_probe(device_t *device)
{
    static igpu_t gpu; /* one integrated GPU per machine */
    static uint8_t edid[256];
    igpu_t *g = &gpu;
    pci_device_t *pci = pci_from_device(device);
    char option[32];

    if (!generation9(device->id.device)) {
        klog_info("igpu: Intel graphics 8086:%04x is not generation 9: left to the firmware's framebuffer",
                  device->id.device);
        return STATUS_NOT_SUPPORTED;
    }
    if (g->regs || !pci->bars[0].phys)
        return STATUS_NOT_SUPPORTED;
    g->pci = pci;
    status_t status = pci_enable_device(pci, false);
    if (STATUS_IS_ERROR(status))
        return status;
    /* BAR 0: 16 MiB, registers in the first 2 MiB, the translation table in the upper half */
    g->regs = (volatile uint8_t *)vmm_map_mmio(pci->bars[0].phys, 2u << 20, VM_UNCACHED);
    if (!g->regs)
        return STATUS_OUT_OF_MEMORY;

    /* Stolen memory (PCI 0x5C: base, 0x50: size) and the size of the translation table (0x50 bits 7:6) */
    uint32_t gmch = pci_read32(pci, 0x50), gms = (gmch >> 8) & 0xFF, ggms = (gmch >> 6) & 3;
    g->stolen_base = pci_read32(pci, 0x5C) & ~0xFFFFFu;
    g->stolen_size = (uint64_t)(gms < 0xF0 ? gms * 32 : 4 * (gms - 0xF0 + 1)) << 20;
    uint32_t ggtt_bytes = ggms ? (1u << ggms) << 20 : 0;
    if (ggtt_bytes) {
        g->ggtt = (volatile uint64_t *)vmm_map_mmio(pci->bars[0].phys + GGTT_OFFSET, ggtt_bytes, VM_UNCACHED);
        g->ggtt_entries = g->ggtt ? ggtt_bytes / 8 : 0;
    }
    klog_info("igpu: Intel graphics 8086:%04x (generation 9), %lu MiB stolen memory, %u MiB graphics address space",
              device->id.device, g->stolen_size >> 20, g->ggtt_entries / 256);

    if (!read_state(g)) {
        klog_info("igpu: the display stays as the firmware set it up");
        return STATUS_SUCCESS;
    }
    if (g->dp) {
        /* What the monitor could do (DPCD 0x001: link rate in units of 0.27 Gbit/s, 0x002: lanes): the firmware
         * does not always train the fastest link, and a faster one would need link training here. */
        uint8_t caps[16];
        if (dpcd_read(g, g->port, 0x000, caps, 16) == 16)
            klog_info("igpu: the monitor accepts up to %u lanes at %u.%02u Gbit/s (DisplayPort %u.%u)", caps[2] & 0x1F,
                      caps[1] * 27 / 100, caps[1] * 27 % 100, caps[0] >> 4, caps[0] & 0xF);
    }
    int blocks = g->dp ? edid_read_aux(g, g->port, edid) : edid_read_ddc(g, g->port, edid);
    if (!blocks) {
        klog_warn("igpu: the monitor's data (EDID) cannot be read");
        return STATUS_SUCCESS;
    }
    collect_modes(g, edid, blocks);
    for (uint32_t i = 0; i < g->mode_count; i++) {
        const timing_t *t = &g->modes[i];
        uint32_t hz = timing_hz100(t);
        klog_info("igpu: monitor mode %ux%u at %u.%02u Hz, pixel clock %u kHz%s", t->ha, t->va, hz / 100, hz % 100,
                  t->khz, same_timing(t, &g->current) ? " (the current one)"
                          : t->khz > g->limit_khz     ? " (too fast for this connection as it is)"
                                                      : "");
    }

    if (!cmdline_value("igpu", option, sizeof(option))) {
        klog_info("igpu: nothing changed; boot with igpu=native (or igpu=WIDTHxHEIGHT) to switch modes");
        return STATUS_SUCCESS;
    }
    const timing_t *wanted = choose_mode(g, option);
    if (!wanted)
        return STATUS_SUCCESS;
    /* Also if the screen already shows this mode: the driver's own framebuffers are what the extras build on. */
    mode_set(g, wanted);
    return STATUS_SUCCESS;
}

static const device_match_t igpu_ids[] = {
    { 0x8086, 0, 0x03, 0x00, MATCH_VENDOR | MATCH_CLASS | MATCH_SUBCLASS }, /* Intel VGA-compatible controller */
    DEVICE_MATCH_END,
};

static driver_t igpu_driver = {
    .name = "intel-gpu",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_DISPLAY,
    .ids = igpu_ids,
    .probe = igpu_probe,
};

static status_t igpu_module_init(void)
{
    return driver_register(&igpu_driver);
}

static const char *const igpu_dependencies[] = { "pci", NULL };

MODULE(.name = "intel_gpu", .description = "Intel integrated graphics (generation 9), display", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 12, 0), .dependencies = igpu_dependencies, .init = igpu_module_init);
