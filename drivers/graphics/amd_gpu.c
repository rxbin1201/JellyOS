/*
 * AMD integrated graphics with the display engine "DCN 2.1": Ryzen 4000 and
 * 5000 processors with Radeon Graphics (Renoir, Lucienne, Cezanne, Barcelo;
 * for example the Ryzen 5 5600G), display part.
 *
 * The UEFI firmware has lit the screen and JellyOS shows its framebuffer.
 * This driver looks at how the firmware set the display engine up and, with
 * "amdgpu=on" or "amdgpu=native" on the kernel command line, adds what the
 * engine can do ("native" also switches to the monitor's best mode at once;
 * "on" leaves the firmware's mode until somebody chooses another):
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
 * A mode that needs a faster link than the firmware trained (a 3440x1440
 * monitor at 100 Hz on a link set up for 60 Hz) gets one. The port's PHY
 * is not programmed through registers the driver knows but by a program in
 * the video BIOS ("AtomBIOS command table" DIG1TransmitterControl), which
 * the driver runs with the interpreter in atom.c; the video BIOS comes from
 * the ACPI table VFCT. So: display clock up (a message to the system
 * management unit), stream and timing generator off, transmitter off and on
 * at the new rate, link training (dp_aux.c, with this file's functions for
 * patterns and signal levels), then the mode as above. If training fails,
 * the old rate is brought back.
 *
 * A thread looks at the connection once a second (hot plug): it reads the
 * monitor's link status over the AUX channel. No answer means the monitor
 * is gone. When it is back, or reports that it lost the link (it was
 * switched off and on), the link is trained again. A monitor that comes
 * back is asked for its EDID again; another monitor gets its own list of
 * modes and, if the mode on the screen is not among them, its best one.
 *
 * HDMI (and DVI) connectors are driven with DVI signalling, as the firmware
 * does: no info frames. There the pixel clock is not a DTO but the PLL of
 * the port's PHY, set by the video BIOS's table SetPixelClock; the monitor's
 * EDID comes over the DDC line with the hardware I2C engine, and whether a
 * monitor is there is the hot plug pin.
 *
 * The board's connectors are in a list in the video BIOS. On this hardware
 * connector n uses PHY n, encoder n, AUX channel or DDC line n and hot plug
 * pin n. While no monitor is on the connector in use, the pins of the others
 * are looked at; a monitor there gets the picture: the old transmitter and
 * encoder are switched off, the new encoder is connected to the timing
 * generator and set to the connector's kind of signal, and its transmitter
 * is brought up (for DisplayPort with link training at the monitor's best
 * rate).
 *
 * Not yet: several screens at once, HDMI with info frames and audio, any
 * kind of acceleration.
 */

#include "drivers/acpi/acpi.h"
#include "drivers/console/kmsg.h"
#include "drivers/bus/pci/pci.h"
#include "drivers/core/module.h"
#include "drivers/graphics/atom.h"
#include "drivers/graphics/display.h"
#include "drivers/graphics/dp_aux.h"
#include "drivers/graphics/edid.h"

#include "core/arch.h"
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
#define DP_LINK_CNTL(i)       (0x15720 + 0x400u * (uint32_t)(i)) /* bit 4: training is complete */
#define DP_CONFIG(i)          (0x1572C + 0x400u * (uint32_t)(i)) /* lanes - 1 */
#define DP_PHY_INTERNAL(i)    (0x1573C + 0x400u * (uint32_t)(i)) /* panel mode: 0 for a monitor */
#define DP_LINK_FRAMING(i)    (0x1574C + 0x400u * (uint32_t)(i)) /* 17:0 idle interval, bit 24 no VB-ID, 28 enhanced */
#define DP_PHY_CNTL(i)        (0x1575C + 0x400u * (uint32_t)(i)) /* bit 16 bypass */
#define DP_PHY_PATTERN(i)     (0x15760 + 0x400u * (uint32_t)(i)) /* training pattern 1-4 as 0-3 */
#define DP_PHY_PRBS(i)        (0x15774 + 0x400u * (uint32_t)(i)) /* bit 0: test pattern generator on */
#define DP_PHY_SCRAMBLER(i)   (0x15778 + 0x400u * (uint32_t)(i)) /* bit 4 advance, 17:8 scrambler reset interval */
#define DP_VID_STREAM_CNTL(i) (0x15730 + 0x400u * (uint32_t)(i)) /* bit 0 on, 9:8 when to stop, bit 16 sending */
#define DP_STEER_FIFO(i)      (0x15734 + 0x400u * (uint32_t)(i)) /* bit 0 reset */
#define DP_VID_TIMING(i)      (0x15740 + 0x400u * (uint32_t)(i)) /* bit 8: M is measured by the hardware */
#define DP_VID_N(i)           (0x15744 + 0x400u * (uint32_t)(i))
#define DP_VID_M(i)           (0x15748 + 0x400u * (uint32_t)(i))
#define DP_MSA_TIMING(i, k)   (0x15830 + 0x400u * (uint32_t)(i) + 4u * (uint32_t)(k)) /* what the monitor is told: 1-4 */
#define DIG_CLOCK_PATTERN(i)  (0x154AC + 0x400u * (uint32_t)(i)) /* TMDS: the pattern on the clock lane (0x1F) */
#define DP_PIXEL_FORMAT(i)    (0x15724 + 0x400u * (uint32_t)(i)) /* bits 26:24 colour depth (1: 8 bits) */
#define DP_MSA_COLORIMETRY(i) (0x15728 + 0x400u * (uint32_t)(i)) /* bits 31:24: what the monitor is told about it */
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

/* The pads of a connector's AUX/DDC pair: either the AUX channel (DisplayPort) or an I2C line (HDMI, DVI) */
#define DDC_PAD_MASK(n)       (0x17640 + 0x10u * (uint32_t)(n)) /* bit 16: the pads are an AUX channel */
#define PHY_AUX_CNTL          0x176FC                           /* bits 11:10, 13:12, ...: receiver of pad n (1: I2C) */
#define AUX_PAD_CTRL          0x17774                           /* bit 12 + n: pad n is in I2C mode */

/* The PHY of connector n (RDPCSTX_PHY_CNTL6): bit 19 gives it the reference clock for DisplayPort */
#define PHY_CNTL6(n)          (0x17818 + 0x360u * (uint32_t)(n))
#define PHY_DP_REF_CLK_EN     (1u << 19)

/* Hot plug pins: one block per pin */
#define HPD_STATUS(n)         (0x14F50 + 0x20u * (uint32_t)(n)) /* bit 1: something is plugged in */
#define HPD_COUNT             6

/* The I2C engine for the DDC lines of HDMI and DVI connectors (one engine, the line is selected) */
#define I2C_CONTROL           0x14D60 /* bit 0 go, bit 1 reset, bit 3 status reset, 10:8 line, 21:20 transactions - 1 */
#define I2C_ARBITRATION       0x14D64 /* bit 20 software wants it, bit 21 software is done */
#define I2C_SW_STATUS         0x14D6C /* 1:0 state (1: busy), bit 2 done, 4 aborted, 5 timeout, 8 no answer */
#define I2C_SPEED(line)       (0x14D88 + 8u * (uint32_t)(line)) /* prescale << 16 */
#define I2C_SETUP(line)       (0x14D8C + 8u * (uint32_t)(line)) /* bit 6 on, 31:24 time limit */
#define I2C_TRANSACTION(k)    (0x14DB8 + 4u * (uint32_t)(k))    /* bit 0 read, 8 stop on NACK, 12 start, 13 stop, count << 16 */
#define I2C_DATA              0x14DC8 /* a byte << 8; bit 31 with an index << 16: where in the buffer; bit 0 read */
#define I2C_LINES             6

/* The video BIOS's list of connectors */
#define ATOM_DATA_DISPLAY_OBJECTS 22

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

#define TMDS_MAX_KHZ          340000  /* HDMI without scrambling */
#define SMU_SET_DISPCLK       0x4     /* messages: argument and answer in MHz */
#define SMU_SET_DPPCLK        0x7

/* The video BIOS's command table that switches a port's transmitter (PHY), and what it is told */
#define ATOM_TRANSMITTER_CONTROL 76
#define ATOM_SET_PIXEL_CLOCK  12      /* the PLL of a PHY as pixel clock for a timing generator (HDMI, DVI) */
#define ATOM_PHY_PLL0         20      /* the PLL of PHY 0; the others follow */
#define ATOM_MODE_DVI         2       /* kinds of signal: 0 DisplayPort, 2 DVI, 3 HDMI */
#define CONNECTOR_HDMI        0x0C
#define TRANSMITTER_DISABLE   0
#define TRANSMITTER_ENABLE    1
#define TRANSMITTER_INIT      7       /* once for a PHY the firmware did not use */
#define TRANSMITTER_LEVELS    11      /* voltage swing and pre-emphasis */
#define CONNECTOR_DISPLAYPORT 0x13

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

    uint32_t          sink_khz;                /* the fastest link the monitor takes */
    atom_t            atom;                    /* the video BIOS's programs */
    bool              atom_ready;              /* the transmitter can be switched through them */
    bool              connected;
    int               edid_blocks;
    bool              pixel_clock_table;       /* the video BIOS can set a PHY's PLL (for HDMI modes) */
    bool              phy_on;                  /* the transmitter of the connector in use is switched on */
    bool              dead;                    /* the display engine's registers do not answer any more */
    bool              trace;                   /* amdgpu=...,trace: log every register access of the video BIOS */
    uint32_t          traced;                  /* accesses logged in the current run */
    uint32_t          trace_seen[2][2];        /* the last two reads logged (register, value) */
    uint32_t          trace_repeats;           /* reads since then that were one of these again */
    bool              lit;                     /* `current` is on the screen (not while a connector is brought up) */
    uint8_t           kinds[HPD_COUNT];        /* the board's connectors by number: KIND_* */
    uint32_t          atom_bad_register;       /* a register beyond the BAR a table asked for (0: none) */
} amdgpu_t;

enum { KIND_NONE, KIND_DISPLAYPORT, KIND_TMDS };

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
 * The two pads of connector n as an AUX channel or as an I2C line. The firmware sets this for the connector it
 * lights; a DisplayPort connector it did not use may be left as I2C, and its monitor then never answers.
 */
static void pad_mode(amdgpu_t *g, uint32_t n, bool aux)
{
    uint32_t mask = rd(g, DDC_PAD_MASK(n)), control = rd(g, AUX_PAD_CTRL), phy = rd(g, PHY_AUX_CNTL);

    klog_debug("amdgpu: pads of connector %u: 0x%x, 0x%x, 0x%x -> %s", n, mask, control, phy, aux ? "AUX" : "I2C");
    if (aux) {
        wr(g, DDC_PAD_MASK(n), mask | 1u << 16);
        wr(g, AUX_PAD_CTRL, control & ~(1u << (12 + n)));
    } else {
        wr(g, DDC_PAD_MASK(n), mask & ~(1u << 16));
        wr(g, AUX_PAD_CTRL, control | 1u << (12 + n));
        wr(g, PHY_AUX_CNTL, (phy & ~(3u << (10 + 2 * n))) | 1u << (10 + 2 * n));
    }
    sleep_ms(3); /* the pads settle */
}

/* An AUX engine the firmware did not use is off: on, through its reset, listening to connector i's hot plug pin. */
static void aux_enable(amdgpu_t *g, uint32_t i)
{
    uint32_t control = rd(g, AUX_CONTROL(i));

    pad_mode(g, i, true);
    if (control & 1u)
        return;
    control = (control & ~(7u << 20)) | i << 20 | 1u << 8 | 1u;
    wr(g, AUX_CONTROL(i), control | 1u << 4);
    wait_bits(g, AUX_CONTROL(i), 1u << 5, 1u << 5, 12);
    wr(g, AUX_CONTROL(i), control);
    wait_bits(g, AUX_CONTROL(i), 1u << 5, 0, 12);
}

static bool ddc_read_block(amdgpu_t *g, uint32_t line, uint8_t offset, uint8_t *block);

/* The EDID of a monitor on an HDMI or DVI connector, over its DDC line: returns the number of valid blocks. */
static int ddc_edid(amdgpu_t *g, uint32_t line, uint8_t *edid)
{
    for (int tries = 0; tries < 3; tries++) {
        if (ddc_read_block(g, line, 0, edid) && edid_header_ok(edid) && edid_block_ok(edid)) {
            if (edid[126] && ddc_read_block(g, line, 128, edid + 128) && edid_block_ok(edid + 128))
                return 2;
            return 1;
        }
        sleep_ms(20);
    }
    return 0;
}

static uint32_t measure_refresh(amdgpu_t *g);

static void log_modes(amdgpu_t *g, const char *too_fast)
{
    display_timing_sort(g->modes, g->mode_count);
    for (uint32_t i = 0; i < g->mode_count; i++) {
        const display_timing_t *t = &g->modes[i];
        uint32_t hz = display_timing_hz100(t);
        klog_info("amdgpu: monitor mode %ux%u at %u.%02u Hz, pixel clock %u kHz (total %ux%u)%s", t->ha, t->va, hz / 100,
                  hz % 100, t->khz, t->ht, t->vt, g->limit_khz && t->khz > g->limit_khz ? too_fast : "");
    }
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
    if (!g->displayport && g->encoder >= 0) {
        /* HDMI or DVI: the EDID over the DDC line. The pixel clock is not in a register: it is the one of the
         * monitor's mode that has the timing on the screen. */
        g->limit_khz = TMDS_MAX_KHZ;
        g->edid_blocks = ddc_edid(g, (uint32_t)g->encoder, g->edid);
        if (!g->edid_blocks) {
            klog_warn("amdgpu: the monitor's data (EDID) cannot be read over DDC line %d", g->encoder);
            return;
        }
        g->mode_count = edid_collect_timings(g->edid, g->edid_blocks, g->modes, 0, MAX_MODES);
        /* Two modes can have the same timing and differ only in the pixel clock (60 and 100 Hz with the same
         * totals): of those with the timing on the screen, the one nearest to the measured refresh rate. */
        uint32_t measured = measure_refresh(g), nearest = 0xFFFFFFFF;
        for (uint32_t i = 0; i < g->mode_count; i++) {
            const display_timing_t *t = &g->modes[i];
            uint32_t rate = display_timing_mhz(t), distance = rate > measured ? rate - measured : measured - rate;
            if (t->ha == g->current.ha && t->va == g->current.va && t->ht == g->current.ht && t->vt == g->current.vt &&
                distance < nearest) {
                nearest = distance;
                g->current = *t;
                g->refresh_mhz = rate;
            }
        }
        klog_info("amdgpu: DDC line %d: EDID of %d block%s; the mode on the screen has a pixel clock of %u kHz", g->encoder,
                  g->edid_blocks, g->edid_blocks == 1 ? "" : "s", g->current.khz);
        log_modes(g, " (too fast for HDMI without scrambling)");
        return;
    }
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
    g->sink_khz = (uint32_t)(caps[1] > 0x14 ? 0x14 : caps[1]) * 27000; /* at most 5.4 Gbit/s: what this port does */
    if (dp_dpcd_read(&aux, DPCD_LINK_BW_SET, link, 2) == 2 && link[0] && (link[1] & 0x1F)) {
        g->link_khz = (uint32_t)link[0] * 27000;
        g->lanes = link[1] & 0x1F;
        /* 24 bits per pixel over 8b/10b lanes */
        g->limit_khz = (uint32_t)((uint64_t)g->link_khz * 8 * g->lanes / 24);
        klog_info("amdgpu: DisplayPort link as trained: %u lane%s at %u.%02u Gbit/s; modes up to %u kHz pixel clock",
                  g->lanes, g->lanes == 1 ? "" : "s", g->link_khz / 100000, g->link_khz / 1000 % 100, g->limit_khz);
    }
    int blocks = g->edid_blocks = dp_edid_read(&aux, g->edid);
    if (!blocks) {
        klog_warn("amdgpu: the monitor's data (EDID) cannot be read");
        return;
    }
    g->mode_count = edid_collect_timings(g->edid, blocks, g->modes, 0, MAX_MODES);
    log_modes(g, " (needs a faster link than the firmware's)");
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
        { 0x154A0, 0x15880 }, /* encoder 0: HDMI and DisplayPort parts */
        { 0x0E7A0, 0x0E7C0 }, /* where the timing generators start */
        { 0x13400, 0x13540 }, /* output formatter and buffer 0 */
        { 0x13E20, 0x13F20 }, /* inputs of the timing generators */
    };

    uint32_t ranges_start = 0;

    klog_info("amdgpu: clocks: display %u kHz, DPP %u kHz, DisplayPort reference %u kHz, memory hub %u kHz; SMU 0x%x",
              rd(g, CLK_DISPCLK) * 100, rd(g, CLK_DPPCLK) * 100, rd(g, CLK_DPREFCLK) * 100, rd(g, CLK_DCFCLK) * 100,
              rd(g, SMU_RESPONSE));
    for (size_t r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++) {
        /* Blocks of another pipe than 0 lie at the same distance as their first registers. */
        uint32_t shift = r == 2 ? HUBP(g->hubp) : r == 3 ? DPP(g->hubp) : r == 5 ? OTG(g->otg)
                         : r == 6 && g->encoder > 0 ? 0x400u * (uint32_t)g->encoder : 0;
        if (r == 6 && !g->displayport) {
            /* Not in bulk: the HDMI part of an encoder has packet data ports, and a monitor on it went dark
             * for seconds after they were read. The registers that say how the encoder is set up: */
            uint32_t e = 0x400u * (uint32_t)(g->encoder > 0 ? g->encoder : 0);
            klog_debug("amdgpu: encoder: front 0x%x, clock pattern 0x%x, HDMI control 0x%x, back 0x%x, on 0x%x, TMDS 0x%x",
                       rd(g, 0x154A0 + e), rd(g, 0x154AC + e), rd(g, 0x154C4 + e), rd(g, 0x155BC + e),
                       rd(g, 0x155C0 + e), rd(g, 0x1564C + e));
            continue;
        }
        if (r == 6)
            ranges_start = 0x15720; /* the DisplayPort part only */
        for (uint32_t reg = (r == 6 ? ranges_start : ranges[r][0]) + shift; reg < ranges[r][1] + shift; reg += 32)
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

static display_ops_t amdgpu_ops;

/* A panic: the first framebuffer, where the console draws, without the pointer. Register writes only. */
static void amdgpu_panic(display_t *display)
{
    amdgpu_t *g = amdgpu_of(display);

    if (amdgpu_ops.flip)
        surface_show(g, g->framebuffers[0]);
    if (amdgpu_ops.cursor_move)
        amdgpu_cursor_move(display, 0, 0, false);
}

/* --- The video BIOS and its programs ----------------------------------------------------- */

static void log_connectors(amdgpu_t *g);

/*
 * "amdgpu=...,trace": every register access of the video BIOS's tables in the log, for finding out what a table
 * does and where it gives up. A table that waits for something reads the same one or two registers hundreds
 * of times; those are counted, not listed.
 */
static void trace_flush(amdgpu_t *g)
{
    if (g->trace_repeats)
        klog_debug("amdgpu: bios   (the last reads %u times more)", g->trace_repeats);
    g->trace_repeats = 0;
}

static void trace_start(amdgpu_t *g)
{
    g->traced = 0;
    g->trace_repeats = 0;
    memset(g->trace_seen, 0xFF, sizeof(g->trace_seen));
}

static void trace_access(amdgpu_t *g, bool write, uint32_t address, uint32_t value)
{
    if (!write) {
        for (int i = 0; i < 2; i++) {
            if (g->trace_seen[i][0] == address && g->trace_seen[i][1] == value) {
                g->trace_repeats++;
                return;
            }
        }
        g->trace_seen[0][0] = g->trace_seen[1][0];
        g->trace_seen[0][1] = g->trace_seen[1][1];
        g->trace_seen[1][0] = address;
        g->trace_seen[1][1] = value;
    } else {
        memset(g->trace_seen, 0xFF, sizeof(g->trace_seen));
    }
    trace_flush(g);
    if (g->traced++ < 400)
        klog_debug("amdgpu: bios %c %05x %08x", write ? 'w' : 'r', address, value);
}

static uint32_t atom_reg_read(void *context, uint32_t reg)
{
    amdgpu_t *g = context;

    if ((uint64_t)reg * 4 + 4 > g->regs_size) {
        g->atom_bad_register = reg;
        return 0;
    }
    uint32_t value = rd(g, reg * 4);
    if (g->trace)
        trace_access(g, false, reg * 4, value);
    return value;
}

static void atom_reg_write(void *context, uint32_t reg, uint32_t value)
{
    amdgpu_t *g = context;

    if ((uint64_t)reg * 4 + 4 > g->regs_size) {
        g->atom_bad_register = reg;
        return;
    }
    if (g->trace)
        trace_access(g, true, reg * 4, value);
    wr(g, reg * 4, value);
}

static void atom_delay(void *context, uint32_t us)
{
    (void)context;
    if (us >= 1000) {
        sleep_ms((us + 999) / 1000);
        return;
    }
    /*
     * The clock ticks in milliseconds, and the tables wait in loops of a few microseconds each (for a PLL to
     * lock, say): sleeping a millisecond every time made a mode switch take many seconds. Shorter waits are
     * busy loops of writes to the unused diagnostic port 0x80; how many of them make a millisecond is measured
     * once against the clock (assuming a microsecond each was too short on this hardware: a PLL did not lock).
     */
    static uint32_t writes_per_ms;
    if (!writes_per_ms) {
        uint64_t tick = clock_monotonic_ns() / 1000000, count = 0;
        while (clock_monotonic_ns() / 1000000 == tick)
            ; /* to the start of a millisecond */
        tick = clock_monotonic_ns() / 1000000;
        while (clock_monotonic_ns() / 1000000 == tick && count < 100000000) {
            arch_io_write8(0x80, 0);
            count++;
        }
        writes_per_ms = count < 100 ? 100 : (uint32_t)count; /* (the loop above also reads the clock: an upper bound for the wait) */
        klog_debug("amdgpu: %u port writes in a millisecond", writes_per_ms);
    }
    for (uint64_t i = 0, writes = (uint64_t)us * writes_per_ms / 1000 + 1; i < writes; i++)
        arch_io_write8(0x80, 0);
}

/*
 * The video BIOS of an integrated GPU is not in a ROM of its own: the firmware hands it over in the ACPI table
 * "VFCT", a list of images each marked with the PCI device it belongs to.
 */
static void atom_load(amdgpu_t *g, const device_t *device)
{
    static uint32_t scratch[4096]; /* the tables' own memory: 16 KiB */
    const acpi_header_t *table = acpi_find_table("VFCT", 0);
    const uint8_t *bytes = (const uint8_t *)table;
    const atom_io_t io = { g, atom_reg_read, atom_reg_write, atom_delay };
    uint8_t format = 0, content = 0;

    if (!table || table->length < 0x40) {
        klog_info("amdgpu: no video BIOS (ACPI table VFCT): the link stays as the firmware trained it");
        return;
    }
    /* After the ACPI header and a UUID: the offset of the first image. Each image: a header of 28 bytes
     * (PCI bus, device, function as dwords; vendor, device, subsystem IDs; revision; length), then the BIOS. */
    uint32_t offset = *(const uint32_t *)(bytes + 36 + 16);
    while ((uint64_t)offset + 28 <= table->length) {
        const uint8_t *header = bytes + offset;
        uint32_t length = *(const uint32_t *)(header + 24);
        uint16_t vendor = *(const uint16_t *)(header + 12), id = *(const uint16_t *)(header + 14);
        offset += 28;
        if ((uint64_t)offset + length > table->length)
            break;
        if (length && vendor == device->id.vendor && id == device->id.device) {
            if (!atom_init(&g->atom, bytes + offset, length, &io, scratch, sizeof(scratch))) {
                klog_warn("amdgpu: the video BIOS image in VFCT is not an AtomBIOS");
                return;
            }
            if (!atom_table_revision(&g->atom, ATOM_TRANSMITTER_CONTROL, &format, &content) || content != 6) {
                klog_info("amdgpu: video BIOS of %u KiB, but its transmitter control is revision %u.%u (6 is known here)",
                          length >> 10, format, content);
                return;
            }
            g->atom_ready = true;
            klog_info("amdgpu: video BIOS of %u KiB from ACPI; transmitter control revision %u.%u", length >> 10, format,
                      content);
            g->pixel_clock_table = atom_table_revision(&g->atom, ATOM_SET_PIXEL_CLOCK, &format, &content) && content == 7;
            if (!g->pixel_clock_table)
                klog_info("amdgpu: the video BIOS's pixel clock table is revision %u.%u (7 is known here): no HDMI modes",
                          format, content);
            /* For HDMI modes and other connectors (still to come): the tables that would be needed. */
            for (uint32_t index = 0; index < 80; index++) {
                if ((index == 4 || index == 12 || index == 13 || index == 35 || index == 46 || index == 49 ||
                     index == 78) && atom_table_revision(&g->atom, index, &format, &content))
                    klog_debug("amdgpu: video BIOS command table %u: revision %u.%u", index, format, content);
            }
            log_connectors(g);
            return;
        }
        offset += length;
    }
    klog_info("amdgpu: the ACPI table VFCT has no video BIOS for this device");
}

/* What a run of a video BIOS table did, and what went wrong. */
static bool atom_report(amdgpu_t *g, const char *what, bool ok)
{
    trace_flush(g);
    klog_debug("amdgpu: %s: %u tables, %u reads, %u writes%s", what, g->atom.calls, g->atom.reads, g->atom.writes,
               ok ? "" : ", failed");
    if (!ok)
        klog_warn("amdgpu: the video BIOS's %s stopped: %s (at 0x%x)", what, g->atom.error, g->atom.error_at);
    if (g->atom_bad_register)
        klog_warn("amdgpu: the video BIOS asked for register 0x%x, which is beyond the register BAR", g->atom_bad_register);
    return ok;
}

/*
 * Run the transmitter control table for the connector in use. `value`: the lanes' levels for TRANSMITTER_LEVELS,
 * else unused. khz: the link's symbol clock (DisplayPort) or the pixel clock (HDMI, DVI).
 */
static bool transmitter(amdgpu_t *g, uint8_t action, uint8_t value, uint32_t khz)
{
    uint32_t parameters[ATOM_PARAMETERS] = { 0 }, e = (uint32_t)g->encoder;
    uint32_t mode = action == TRANSMITTER_LEVELS ? value : g->displayport ? 0 : ATOM_MODE_DVI;
    uint32_t lanes = g->displayport ? g->lanes : 4;

    /* phy, action, kind of signal or levels, lanes; clock in 10 kHz; hot plug pin (from 1), front end, connector */
    parameters[0] = e | (uint32_t)action << 8 | mode << 16 | lanes << 24;
    parameters[1] = khz / 10;
    parameters[2] = (e + 1) | (1u << e) << 8 | (uint32_t)(g->displayport ? CONNECTOR_DISPLAYPORT : CONNECTOR_HDMI) << 16;
    g->atom_bad_register = 0;
    trace_start(g);
    klog_debug("amdgpu: transmitter control: action %u, 0x%x, %u kHz (parameters 0x%x 0x%x 0x%x)", action, value, khz,
               parameters[0], parameters[1], parameters[2]);
    bool ok = atom_report(g, "transmitter control", atom_execute(&g->atom, ATOM_TRANSMITTER_CONTROL, parameters));
    if (action == TRANSMITTER_ENABLE)
        g->phy_on = ok;
    if (action == TRANSMITTER_DISABLE)
        g->phy_on = false;
    return ok;
}

/* The PLL of the connector's PHY as the pixel clock of our timing generator (HDMI, DVI). */
static bool set_pixel_clock(amdgpu_t *g, uint32_t khz)
{
    uint32_t parameters[ATOM_PARAMETERS] = { 0 }, e = (uint32_t)g->encoder;
    uint32_t encoder_object = e < 2 ? 0x1E : e < 4 ? 0x20 : 0x21; /* PHYs come in pairs: A/B, C/D, E/F */

    /* clock in 100 Hz; PLL, encoder, kind of signal, flags; timing generator, colour depth (0: 8 bits) */
    parameters[0] = khz * 10;
    parameters[1] = (ATOM_PHY_PLL0 + e) | encoder_object << 8 | ATOM_MODE_DVI << 16;
    parameters[2] = (uint32_t)g->otg;
    g->atom_bad_register = 0;
    trace_start(g);
    klog_debug("amdgpu: pixel clock: %u kHz from the PLL of PHY %u", khz, e);
    return atom_report(g, "pixel clock table", atom_execute(&g->atom, ATOM_SET_PIXEL_CLOCK, parameters));
}

/* A message to the system management unit; returns its answer, 0 if it does not answer. */
static uint32_t smu_message(amdgpu_t *g, uint32_t message, uint32_t argument)
{
    for (int tries = 0; tries < 2000 && rd(g, SMU_RESPONSE) == 0; tries++)
        sleep_ms(1); /* something else is still being worked on */
    wr(g, SMU_RESPONSE, 0);
    wr(g, SMU_ARGUMENT, argument);
    wr(g, SMU_MESSAGE, message);
    for (int tries = 0; tries < 2000 && rd(g, SMU_RESPONSE) == 0; tries++)
        sleep_ms(1);
    if (rd(g, SMU_RESPONSE) != 1) {
        klog_warn("amdgpu: the system management unit answers 0x%x to message %u (%u)", rd(g, SMU_RESPONSE), message,
                  argument);
        return 0;
    }
    return rd(g, SMU_ARGUMENT);
}

/* The display clock must run at least as fast as the pixel clock (with some room). */
static void display_clock_for(amdgpu_t *g, uint32_t pixel_khz)
{
    uint32_t now_khz = rd(g, CLK_DISPCLK) * 100, wanted_mhz = (pixel_khz + pixel_khz / 10 + 999) / 1000;

    if (now_khz >= wanted_mhz * 1000)
        return;
    uint32_t got = smu_message(g, SMU_SET_DISPCLK, wanted_mhz);
    smu_message(g, SMU_SET_DPPCLK, got ? got : wanted_mhz);
    sleep_ms(2);
    klog_info("amdgpu: display clock from %u to %u kHz (asked for %u MHz, the unit says %u)", now_khz,
              rd(g, CLK_DISPCLK) * 100, wanted_mhz, got);
}

/* The source's side of link training (dp_aux.h) */
static void source_pattern(void *context, int pattern)
{
    amdgpu_t *g = context;
    uint32_t e = (uint32_t)g->encoder;

    if (pattern) {
        wr(g, DP_PHY_PATTERN(e), (uint32_t)pattern - 1);
        wr(g, DP_LINK_CNTL(e), rd(g, DP_LINK_CNTL(e)) & ~(1u << 4));
    } else {
        /* Normal operation: framing as a monitor expects it, scrambler reset interval, training complete. */
        wr(g, DP_PHY_INTERNAL(e), 0);
        wr(g, DP_LINK_FRAMING(e), (rd(g, DP_LINK_FRAMING(e)) & ~(0x3FFFFu | 1u << 24)) | 0x2000 | 1u << 28);
        wr(g, DP_PHY_SCRAMBLER(e), (rd(g, DP_PHY_SCRAMBLER(e)) & ~(0x3FFu << 8)) | 0x1FFu << 8);
        wr(g, DP_LINK_CNTL(e), rd(g, DP_LINK_CNTL(e)) | 1u << 4);
    }
    wr(g, DP_PHY_CNTL(e), rd(g, DP_PHY_CNTL(e)) & ~(1u << 16));
    wr(g, DP_PHY_PRBS(e), rd(g, DP_PHY_PRBS(e)) & ~1u);
}

static void source_levels(void *context, uint8_t swing, uint8_t emphasis)
{
    amdgpu_t *g = context;

    transmitter(g, TRANSMITTER_LEVELS, (uint8_t)(swing | emphasis << 3), g->link_khz);
}

/*
 * Is the transmitter of connector e on? Asked of the PHY itself: a PHY that is off (never used, or switched
 * off by the video BIOS) has all four lanes in power state 3, one that sends has them in state 0.
 *
 * This matters more than it looks. The stream side of an encoder (its "front end": DIG_FE_CNTL and the
 * registers around it) is clocked by the PHY. Reading it while its back end is connected to a PHY that does
 * not run does not just give nonsense: no register of the display engine answers any more afterwards. So
 * the front end of an encoder is only touched after this said yes.
 */
static bool transmitter_runs(amdgpu_t *g, uint32_t e)
{
    uint32_t phy = rd(g, PHY_CNTL6(e));

    if (phy == 0xFFFFFFFF)
        return false;
    for (uint32_t lane = 0; lane < 4; lane++) {
        if (((phy >> (4 * lane)) & 3u) != 3)
            return true;
    }
    return false;
}

/* The transmitter off and on again at link_khz, and the link trained. The pipe must be stopped. */
static bool link_bring_up(amdgpu_t *g, uint32_t link_khz)
{
    aux_engine_t where = { g, g->aux_engine };
    dp_aux_t aux = { &where, aux_once };
    dp_source_t source = { g, source_pattern, source_levels, 3 };
    uint32_t e = (uint32_t)g->encoder;
    dp_training_t training;

    /*
     * Off first, to change the rate. Not for a PHY that is off already: switching a never used one off took the
     * registers of its AUX channel and encoder away, and it could not be switched on any more.
     */
    if (g->phy_on) {
        transmitter(g, TRANSMITTER_DISABLE, 0, g->link_khz);
        if (!g->lit)
            kmsg_logfile_write();
    }
    wr(g, DP_PHY_PATTERN(e), 0);
    wr(g, DP_LINK_CNTL(e), rd(g, DP_LINK_CNTL(e)) & ~(1u << 4));
    wr(g, DP_PHY_INTERNAL(e), 0);
    wr(g, DP_CONFIG(e), g->lanes - 1);
    wr(g, DP_PHY_SCRAMBLER(e), rd(g, DP_PHY_SCRAMBLER(e)) | 1u << 4);
    g->link_khz = link_khz;
    /*
     * The PHY's reference clock for DisplayPort. The firmware switches it on for the connector it lights only;
     * without it the video BIOS's table gives up at once, and the PHY of another connector never comes on.
     */
    uint32_t phy = rd(g, PHY_CNTL6(e));
    if (!(phy & PHY_DP_REF_CLK_EN)) {
        wr(g, PHY_CNTL6(e), phy | PHY_DP_REF_CLK_EN);
        sleep_ms(1);
    }
    if (!g->lit) {
        klog_info("amdgpu: PHY %u: control 0x%x -> 0x%x", e, phy, rd(g, PHY_CNTL6(e)));
        kmsg_logfile_write();
    }
    if (!transmitter(g, TRANSMITTER_ENABLE, 0, link_khz))
        return false;
    if (!g->lit) {
        uint32_t on = rd(g, DIG_BE_EN_CNTL(e)), back = rd(g, DIG_BE_CNTL(e));
        klog_info("amdgpu: after switching on: PHY 0x%x, back end 0x%x (on 0x%x)", rd(g, PHY_CNTL6(e)), back, on);
        kmsg_logfile_write();
    }
    if (!transmitter_runs(g, e)) {
        klog_warn("amdgpu: the transmitter of connector %u did not come on", e);
        g->phy_on = false;
        return false;
    }
    bool ok = dp_link_train(&aux, &source, link_khz, g->lanes, false, &training);
    if (ok)
        klog_info("amdgpu: link trained: %u lane%s at %u.%02u Gbit/s, voltage swing %u, pre-emphasis %u", g->lanes,
                  g->lanes == 1 ? "" : "s", link_khz / 100000, link_khz / 1000 % 100, training.swing, training.emphasis);
    else
        klog_warn("amdgpu: no link at %u.%02u Gbit/s: %s (lane status %02x %02x, alignment %02x)", link_khz / 100000,
                  link_khz / 1000 % 100, training.problem, training.status[0], training.status[1], training.status[2]);
    return ok;
}

/* The pixel clock a link of this rate carries: 24 bits per pixel over 8b/10b lanes. */
static uint32_t link_limit(const amdgpu_t *g, uint32_t link_khz)
{
    return (uint32_t)((uint64_t)link_khz * 8 * g->lanes / 24);
}

/* The fastest pixel clock a mode may have: with the video BIOS's help, what the monitor's fastest link carries. */
static uint32_t pixel_limit(const amdgpu_t *g)
{
    if (!g->displayport)
        return g->limit_khz;
    return g->atom_ready && g->sink_khz > g->link_khz ? link_limit(g, g->sink_khz) : g->limit_khz;
}

/* --- The other connectors: what the board has, what is plugged in ---------------------- */

/* The hot plug pins as a bit mask. */
static uint32_t hpd_pins(amdgpu_t *g)
{
    uint32_t mask = 0;

    for (uint32_t n = 0; n < HPD_COUNT; n++) {
        uint32_t status = rd(g, HPD_STATUS(n));
        if (status != 0xFFFFFFFF && (status & 2u))
            mask |= 1u << n;
    }
    return mask;
}

/*
 * One block of EDID over a DDC line with the hardware I2C engine (after Linux's dce_i2c_hw.c): write the
 * offset to device 0x50, then read 128 bytes, as two transactions of one run. False if nothing answers.
 */
static bool ddc_read_block(amdgpu_t *g, uint32_t line, uint8_t offset, uint8_t *block)
{
    bool ok = false;

    if (AUX_OWNER(rd(g, I2C_ARBITRATION)) == 2)
        return false; /* the display microcontroller has the engine */
    wr(g, I2C_ARBITRATION, rd(g, I2C_ARBITRATION) | 1u << 20);
    /* 100 kHz from the 48 MHz reference: the firmware's time base registers say how it is divided. */
    uint32_t base = rd(g, 0x004EC), reference = (base & 0x7F) ? (base & 0x7F) * 1000 : 48000;
    uint32_t divider = (base >> 8) & 0x7F ? (base >> 8) & 0x7F : 2;
    wr(g, I2C_SPEED(line), (reference / divider / 100) << 16 | 2u << 8 | 2u);
    wr(g, I2C_CONTROL, (rd(g, I2C_CONTROL) & ~(0xFu | 7u << 8 | 3u << 20)) | 1u << 3 | line << 8);
    wr(g, I2C_SETUP(line), (rd(g, I2C_SETUP(line)) & 0x00FFFFFF) | 3u << 24 | 1u << 2 | 1u << 6);
    wr(g, I2C_ARBITRATION, rd(g, I2C_ARBITRATION) & ~(1u << 4));

    wr(g, I2C_TRANSACTION(0), 1u << 8 | 1u << 12 | 1u << 16);                        /* write one byte, no stop */
    wr(g, I2C_DATA, 1u << 31 | 0xA0u << 8);                                          /* device 0x50, writing */
    wr(g, I2C_DATA, (uint32_t)offset << 8);
    wr(g, I2C_TRANSACTION(1), 1u << 0 | 1u << 8 | 1u << 12 | 1u << 13 | 128u << 16); /* read 128 bytes, stop */
    wr(g, I2C_DATA, 0xA1u << 8);                                                     /* device 0x50, reading */

    wr(g, I2C_SETUP(line), rd(g, I2C_SETUP(line)) & ~(1u << 0 | 1u << 1 | 1u << 7 | 0xFFFFu << 8));
    wr(g, I2C_CONTROL, (rd(g, I2C_CONTROL) & ~(0xFu | 3u << 20)) | 1u << 20);
    wr(g, I2C_CONTROL, rd(g, I2C_CONTROL) | 1u);
    for (int ms = 0; ms < 60; ms++) {
        uint32_t status = rd(g, I2C_SW_STATUS);
        if ((status & 3u) != 1) {
            ok = (status & (1u << 2)) && !(status & (1u << 4 | 1u << 5 | 1u << 8));
            break;
        }
        sleep_ms(1);
    }
    if (ok) {
        wr(g, I2C_DATA, 1u << 31 | 3u << 16 | 1u); /* the reply follows the three bytes sent */
        for (int i = 0; i < 128; i++)
            block[i] = (uint8_t)(rd(g, I2C_DATA) >> 8);
    }
    wr(g, I2C_CONTROL, rd(g, I2C_CONTROL) | 1u << 1 | 1u << 3);
    wr(g, I2C_CONTROL, rd(g, I2C_CONTROL) & ~(1u << 1 | 1u << 3));
    wr(g, I2C_SETUP(line), rd(g, I2C_SETUP(line)) & ~(1u << 6));
    wr(g, I2C_ARBITRATION, (rd(g, I2C_ARBITRATION) & ~(1u << 20)) | 1u << 21);
    return ok;
}

/* The connectors of the board as the video BIOS lists them: kind, encoder, DDC line, hot plug pin. */
static void log_connectors(amdgpu_t *g)
{
    const uint8_t *bios = g->atom.bios;
    uint32_t size = 0, table = atom_data_table(&g->atom, ATOM_DATA_DISPLAY_OBJECTS, &size);

    if (!table || table + size > g->atom.size || size < 8) {
        klog_info("amdgpu: the video BIOS has no list of connectors");
        return;
    }
    uint32_t paths = bios[table + 6];
    klog_info("amdgpu: video BIOS: connector list revision %u.%u with %u entries", bios[table + 2], bios[table + 3], paths);
    if (bios[table + 3] != 4)
        return; /* another layout of the entries */
    for (uint32_t i = 0; i < paths && table + 8 + (i + 1) * 16 <= table + size; i++) {
        const uint8_t *path = bios + table + 8 + i * 16;
        uint32_t connector = path[0] | (uint32_t)path[1] << 8, encoder = path[4] | (uint32_t)path[5] << 8;
        uint32_t records = table + (path[2] | (uint32_t)path[3] << 8);
        int i2c = -1, hpd = -1;
        /* Records: type, size, data; 1: DDC (I2C id), 2: hot plug pin; 0xFF ends the list. */
        for (int guard = 0; guard < 16 && records + 4 <= g->atom.size && bios[records] != 0xFF && bios[records + 1]; guard++) {
            if (bios[records] == 1)
                i2c = bios[records + 2];
            if (bios[records] == 2)
                hpd = bios[records + 2];
            records += bios[records + 1];
        }
        uint32_t kind = connector & 0xFF;
        /* Connector n is the one with hot plug pin n (ids count from 1). */
        if (hpd >= 1 && hpd <= HPD_COUNT && (connector >> 12) == 3)
            g->kinds[hpd - 1] = kind == 0x13 ? KIND_DISPLAYPORT
                                : kind == 0x0C || kind == 0x0D || (kind >= 0x02 && kind <= 0x04) ? KIND_TMDS : KIND_NONE;
        klog_info("amdgpu: connector %u: %s (0x%04x), encoder 0x%04x, DDC id 0x%x, hot plug pin id %d", i,
                  kind == 0x13 ? "DisplayPort" : kind == 0x0C || kind == 0x0D ? "HDMI" : kind == 0x14 ? "eDP"
                  : kind == 0x02 || kind == 0x03 || kind == 0x04 ? "DVI" : "other", connector, encoder, i2c, hpd);
    }
}

/* --- Switching modes ------------------------------------------------------------------- */

/* The stream to the monitor, the HUBP and the timing generator off, in that order. */
static void pipe_stop(amdgpu_t *g)
{
    uint32_t e = (uint32_t)g->encoder;
    /* (With the transmitter off there is no stream to stop, and the encoder's stream side must be left alone.) */
    bool stream_side = g->displayport && g->phy_on && transmitter_runs(g, e);
    uint32_t stream = stream_side ? rd(g, DP_VID_STREAM_CNTL(e)) : 0;

    if (stream == 0xFFFFFFFF)
        stream = 0;
    if (stream & 1u) {
        /* Stop at the start of the next vertical blank (2), then wait until nothing is sent any more. */
        wr(g, DP_VID_STREAM_CNTL(e), (stream & ~(3u << 8)) | 2u << 8);
        wr(g, DP_VID_STREAM_CNTL(e), ((stream & ~(3u << 8)) | 2u << 8) & ~1u);
        if (!wait_bits(g, DP_VID_STREAM_CNTL(e), 1u << 16, 0, 120))
            klog_warn("amdgpu: the video stream does not stop (0x%x)", rd(g, DP_VID_STREAM_CNTL(e)));
    }
    if (stream_side)
        wr(g, DP_STEER_FIFO(e), rd(g, DP_STEER_FIFO(e)) | 1u);
    wr(g, HUBP_CNTL(g->hubp), rd(g, HUBP_CNTL(g->hubp)) | HUBP_BLANK | HUBP_NO_URGENCY);
    wait_bits(g, HUBP_CNTL(g->hubp), HUBP_IDLE, HUBP_IDLE, 3);
    wr(g, OTG_CONTROL(g->otg), (rd(g, OTG_CONTROL(g->otg)) | 3u << 8) & ~1u);
    wr(g, VTG_CONTROL(g->otg), rd(g, VTG_CONTROL(g->otg)) & ~(1u << 31));
    if (!wait_bits(g, OTG_CLOCK_CONTROL(g->otg), 1u << 16, 0, 120))
        klog_warn("amdgpu: the timing generator does not stop (0x%x)", rd(g, OTG_CONTROL(g->otg)));
    /* HDMI: the timing generator runs on the PHY's PLL, so the transmitter goes off after it (the PLL is about
     * to change). */
    if (!g->displayport && g->phy_on && g->atom_ready)
        transmitter(g, TRANSMITTER_DISABLE, 0, g->current.khz);
}

/*
 * Encoder e's back end takes its own front end. The video BIOS does this when it switches the transmitter on
 * (and undoes it when it switches it off); this only makes sure. The transmitter must be on.
 */
static void connect_front_end(amdgpu_t *g, uint32_t e)
{
    uint32_t back = rd(g, DIG_BE_CNTL(e));

    if (((back >> 8) & 0x7F) != (1u << e))
        wr(g, DIG_BE_CNTL(e), (back & ~(0x7Fu << 8)) | (1u << e) << 8);
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
    if (!g->displayport) {
        /* HDMI, DVI: the transmitter on at the pixel clock; the encoder passes on what the timing generator makes. */
        transmitter(g, TRANSMITTER_ENABLE, 0, t->khz);
        if (!transmitter_runs(g, e)) {
            klog_warn("amdgpu: the transmitter of connector %u did not come on (PHY 0x%x, back end 0x%x, on 0x%x)", e,
                      rd(g, PHY_CNTL6(e)), rd(g, DIG_BE_CNTL(e)), rd(g, DIG_BE_EN_CNTL(e)));
            g->phy_on = false;
            return;
        }
        connect_front_end(g, e);
        wr(g, DIG_FE_CNTL(e), (rd(g, DIG_FE_CNTL(e)) & ~7u) | (uint32_t)g->otg);
        wr(g, DIG_CLOCK_PATTERN(e), 0x1F);
        return;
    }
    if (!transmitter_runs(g, e)) {
        klog_warn("amdgpu: the transmitter of connector %u is not on: no stream", e);
        return;
    }
    /*
     * The stream side of the encoder: fed by our timing generator, 8 bits per colour, and the monitor is told
     * so. These registers are clocked by the PHY and may only be touched while the transmitter is on (see
     * transmitter_runs()), which is why they are written here and not when a connector is taken into use.
     */
    connect_front_end(g, e);
    wr(g, DIG_FE_CNTL(e), (rd(g, DIG_FE_CNTL(e)) & ~7u) | (uint32_t)g->otg);
    wr(g, DP_PIXEL_FORMAT(e), 1u << 24);
    wr(g, DP_MSA_COLORIMETRY(e), 0x20u << 24);

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

    /* Pixel clock (DisplayPort: the DTO; HDMI: the PHY's PLL, set through the video BIOS in pipe_apply()) */
    if (g->displayport)
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

    if (!g->displayport)
        return count;
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
    if (!g->displayport)
        set_pixel_clock(g, t->khz);
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
        klog_warn("amdgpu: %u frames in 250 ms%s (timing generator input 0x%x, plane 0x%x)", frames,
                  starved ? ", and the pipe ran out of data" : "", input, plane);
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
    display_clock_for(g, t->khz);
    if (!g->displayport && t->khz > g->limit_khz)
        return STATUS_NOT_SUPPORTED;
    if (g->displayport && t->khz > g->limit_khz) {
        /* The link the firmware trained is too slow for this mode: the slowest rate that carries it. */
        static const uint32_t rates[] = { 162000, 270000, 540000 };
        uint32_t before = g->link_khz, rate = 0;
        for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]) && !rate; i++) {
            if (rates[i] <= g->sink_khz && link_limit(g, rates[i]) >= t->khz)
                rate = rates[i];
        }
        if (!rate || !g->atom_ready)
            return STATUS_NOT_SUPPORTED;
        klog_info("amdgpu: the mode needs a faster link: from %u.%02u to %u.%02u Gbit/s", before / 100000,
                  before / 1000 % 100, rate / 100000, rate / 1000 % 100);
        pipe_stop(g);
        if (!link_bring_up(g, rate)) {
            if (!link_bring_up(g, before))
                klog_warn("amdgpu: the old link does not come back either: the screen stays dark until a restart");
            pipe_start(g, &old);
            return STATUS_DEVICE_ERROR;
        }
        g->limit_khz = link_limit(g, rate);
    }
    if (!pipe_apply(g, list, count, undo, t)) {
        if (!g->lit) {
            /* A connector that was just brought up: there is no mode before on it. */
            klog_warn("amdgpu: %ux%u does not come up on this connector", t->ha, t->va);
            g->current = *t; /* what the registers hold now */
            return STATUS_DEVICE_ERROR;
        }
        klog_warn("amdgpu: %ux%u does not come up: back to the mode before", t->ha, t->va);
        g->current = *t; /* the registers hold it: what the way back is planned from and switches off */
        pipe_apply(g, undo, count, NULL, &old);
        g->current = old;
        return STATUS_DEVICE_ERROR;
    }
    g->lit = true;
    g->current = *t;
    g->refresh_mhz = display_timing_mhz(t);
    *pitch = display->info.pitch; /* the framebuffers keep their line length; the picture is their top left part */
    return STATUS_SUCCESS;
}

/*
 * The list of modes for the display layer: the monitor's modes that the link carries and that fit into the
 * framebuffers, with the one on the screen among them. Entry i there is g->modes[i] here.
 */
static void publish_modes(amdgpu_t *g, display_t *d, bool switchable, bool keep_current)
{
    jelly_display_mode_t list[MAX_MODES];
    uint32_t kept = 0, current = DISPLAY_NO_MODE;
    bool known = g->lit && g->current.khz && g->current.ha == d->info.width && g->current.va == d->info.height;

    if (known && keep_current) /* the firmware's mode works with the firmware's monitor, whatever its EDID says */
        g->mode_count = display_timing_add(g->modes, g->mode_count, MAX_MODES, &g->current);
    for (uint32_t i = 0; i < g->mode_count && switchable; i++) {
        const display_timing_t *t = &g->modes[i];
        bool shown = known && display_timing_same(t, &g->current);
        bool fits = t->ha * 4 <= d->info.pitch && (uint64_t)t->va * d->info.pitch <= d->info.size;
        if (shown || (fits && g->limit_khz && t->khz <= pixel_limit(g)))
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

/* --- Hot plug --------------------------------------------------------------------------- */

/* The link at `rate` or, if that cannot be trained, at a slower rate; the picture as it was. True if a link is up. */
static bool link_restore(amdgpu_t *g, uint32_t rate)
{
    static const uint32_t rates[] = { 540000, 270000, 162000 };
    bool ok = false;

    pipe_stop(g);
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]) && !ok; i++) {
        if (rates[i] <= rate)
            ok = link_bring_up(g, rates[i]);
    }
    if (ok)
        g->limit_khz = link_limit(g, g->link_khz);
    pipe_start(g, &g->current);
    return ok;
}

static bool mode_listed(const amdgpu_t *g, const display_timing_t *t)
{
    for (uint32_t i = 0; i < g->mode_count; i++) {
        if (display_timing_same(&g->modes[i], t))
            return true;
    }
    return false;
}

/*
 * A monitor is on connector c, which is not the one in use (or could not be brought up before). Switch the
 * old connector off, connect encoder c to the timing generator and bring its transmitter up, up to the point
 * where a mode can be set. Afterwards the list of modes is the new monitor's and nothing is on the screen.
 */
static bool connector_move(amdgpu_t *g, int c, uint8_t *edid)
{
    static const uint32_t rates[] = { 540000, 270000, 162000 };
    aux_engine_t where = { g, c };
    dp_aux_t aux = { &where, aux_once };
    bool dp = g->kinds[c] == KIND_DISPLAYPORT;
    uint32_t e = (uint32_t)c, m = (uint32_t)g->otg;
    uint8_t caps[16] = { 0 };
    int blocks;

    if (dp) {
        aux_enable(g, e);
        if (dp_dpcd_read(&aux, DPCD_REVISION, caps, 16) != 16 || !caps[1] || !(caps[2] & 0x1F)) {
            klog_info("amdgpu: connector %d: the hot plug pin shows a monitor, but it does not answer on the AUX channel "
                      "(control 0x%x, status 0x%x)", c, rd(g, AUX_CONTROL(e)), rd(g, AUX_SW_STATUS(e)));
            return false;
        }
        blocks = dp_edid_read(&aux, edid);
    } else {
        pad_mode(g, e, false);
        blocks = ddc_edid(g, e, edid);
    }
    if (!blocks) {
        klog_info("amdgpu: connector %d: the hot plug pin shows a monitor, but its EDID cannot be read", c);
        return false;
    }
    klog_info("amdgpu: a monitor on connector %d (%s): the picture moves there", c, dp ? "DisplayPort" : "HDMI");
    kmsg_logfile_write(); /* from here on the screen is dark until it has worked: each step is on record first */

    /*
     * The old connector off: stream and timing generator, then the transmitter. The video BIOS's table takes
     * the encoder apart with it (back end off, front end disconnected); nothing of that is done by hand.
     */
    pipe_stop(g);
    if (g->encoder >= 0 && g->phy_on) /* (for HDMI pipe_stop() has done it) */
        transmitter(g, TRANSMITTER_DISABLE, 0, g->displayport ? g->link_khz : g->current.khz);
    g->lit = false;
    g->encoder = c;
    g->displayport = dp;
    g->aux_engine = dp ? c : -1;
    klog_info("amdgpu: the old connector is off");
    kmsg_logfile_write();

    /*
     * Encoder c: the connector's kind of signal and its hot plug pin. Not more: the back end is switched on and
     * connected to its front end by the video BIOS's table when the transmitter goes on. (Marking it as on
     * here made that table return at once, "it is on already", and connecting the front end to a PHY that
     * does not run is what makes its registers deadly.)
     */
    uint32_t back = rd(g, DIG_BE_CNTL(e)), on = rd(g, DIG_BE_EN_CNTL(e));
    klog_info("amdgpu: encoder %d as found: back end 0x%x (on 0x%x), PHY 0x%x", c, back, on, rd(g, PHY_CNTL6(e)));
    g->phy_on = transmitter_runs(g, e);
    if (g->phy_on) {
        /* Left on by an earlier attempt: off, to start from what the table expects. */
        transmitter(g, TRANSMITTER_DISABLE, 0, dp ? 162000 : 148500);
        back = rd(g, DIG_BE_CNTL(e));
    }
    wr(g, DIG_BE_CNTL(e), (back & ~(7u << 16 | 7u << 28)) | (dp ? 0u : (uint32_t)ATOM_MODE_DVI) << 16 | e << 28);
    memcpy(g->edid, edid, 256);
    g->edid_blocks = blocks;
    g->mode_count = edid_collect_timings(g->edid, blocks, g->modes, 0, MAX_MODES);
    kmsg_logfile_write();

    if (dp) {
        /* The pixel clock from the DisplayPort DTO. */
        wr(g, PIXEL_RATE_CNTL(m), (rd(g, PIXEL_RATE_CNTL(m)) & ~3u) | 1u << 4);
        g->lanes = (caps[2] & 0x1F) >= 4 ? 4 : (caps[2] & 0x1F) >= 2 ? 2 : 1;
        g->sink_khz = (uint32_t)(caps[1] > 0x14 ? 0x14 : caps[1]) * 27000;
        g->link_khz = g->sink_khz;
        bool trained = false;
        for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]) && !trained; i++) {
            if (rates[i] <= g->sink_khz) {
                klog_info("amdgpu: bringing the link up at %u kHz", rates[i]);
                kmsg_logfile_write();
                trained = link_bring_up(g, rates[i]);
                kmsg_logfile_write();
            }
        }
        if (!trained) {
            klog_warn("amdgpu: connector %d: no DisplayPort link at any rate", c);
            return false;
        }
        g->limit_khz = link_limit(g, g->link_khz);
    } else {
        /* The pixel clock from the PHY's PLL (set with each mode), not from the DTO. */
        wr(g, PIXEL_RATE_CNTL(m), (rd(g, PIXEL_RATE_CNTL(m)) & ~(3u | 1u << 4)) | (e & 3u));
        g->limit_khz = TMDS_MAX_KHZ;
    }
    log_modes(g, dp ? " (too fast for this link)" : " (too fast for HDMI without scrambling)");
    return g->mode_count > 0;
}

static void hotplug_thread(void *argument)
{
    static uint8_t edid[256];
    amdgpu_t *g = argument;
    display_t *d = display_get(0);
    uint32_t absent = 0, edid_tries = 0, retrains = 0, pause = 0, move_pause = 0, pins = hpd_pins(g);
    bool watch_link = false, first = true, was_present = false, was_link_ok = false;

    for (;;) {
        bool lost = false, back = false, new_monitor = false, switch_mode = false, present, link_ok = true;
        aux_engine_t where = { g, g->encoder };
        dp_aux_t aux = { &where, aux_once };
        uint8_t status[6], caps[16];

        sleep_ms(1000);
        if (g->dead)
            continue;
        display_lock(d);
        if (rd(g, HPD_STATUS(0)) == 0xFFFFFFFF && rd(g, OTG_CONTROL(g->otg)) == 0xFFFFFFFF) {
            g->dead = true;
            klog_error("amdgpu: the display engine's registers do not answer any more: nothing further is tried");
            display_unlock(d);
            kmsg_logfile_write();
            continue;
        }
        if (hpd_pins(g) != pins) {
            pins = hpd_pins(g);
            klog_info("amdgpu: hot plug pins now 0x%x", pins);
        }
        if (g->displayport) {
            present = dp_dpcd_read(&aux, DPCD_LANE_STATUS, status, 6) == 6;
            link_ok = present && dp_link_good(status, g->lanes);
        } else {
            present = pins & (1u << g->encoder);
        }
        if (first || present != was_present || link_ok != was_link_ok) {
            /* What the watcher sees, whenever it changes. */
            klog_info("amdgpu: watching connector %d (%s): monitor %s, link %s", g->encoder,
                      g->displayport ? "DisplayPort" : "HDMI", present ? "there" : "not there", link_ok ? "good" : "not good");
            was_present = present;
            was_link_ok = link_ok;
        }
        if (first) {
            /* A monitor that calls a link bad on which it shows a picture is not asked again. */
            first = false;
            watch_link = g->displayport && link_ok;
            if (g->displayport && present && !link_ok)
                klog_info("amdgpu: the monitor reports lane status %02x %02x for a working link: the link is not watched",
                          status[0], status[1]);
        }

        if (!present || !g->lit) {
            if (g->connected && ++absent >= 2) {
                g->connected = false;
                lost = true;
            }
            /* Nothing on our connector (or it could not be brought up): a monitor on another one gets the picture. */
            int other = -1;
            for (int c = 0; c < HPD_COUNT && other < 0 && !g->connected && g->atom_ready; c++) {
                bool usable = g->kinds[c] == KIND_DISPLAYPORT || (g->kinds[c] == KIND_TMDS && g->pixel_clock_table);
                if ((pins & (1u << c)) && usable && (c != g->encoder || !g->lit))
                    other = c;
            }
            if (move_pause) {
                move_pause--;
            } else if (other >= 0) {
                if (connector_move(g, other, edid)) {
                    absent = retrains = pause = 0;
                    watch_link = g->displayport;
                    was_present = was_link_ok = true;
                    g->connected = true;
                    back = new_monitor = switch_mode = true;
                } else {
                    move_pause = 3; /* a monitor that was just plugged in may need a moment */
                }
            }
        } else if (!g->connected) {
            absent = 0;
            int blocks = g->displayport ? dp_edid_read(&aux, edid) : ddc_edid(g, (uint32_t)g->encoder, edid);
            if (blocks || ++edid_tries >= 5) { /* a monitor that just woke up may need a moment for its EDID */
                uint32_t rate = g->link_khz;
                edid_tries = 0;
                if (g->displayport && dp_dpcd_read(&aux, DPCD_REVISION, caps, 16) == 16 && caps[1]) {
                    g->sink_khz = (uint32_t)(caps[1] > 0x14 ? 0x14 : caps[1]) * 27000;
                    if (rate > g->sink_khz)
                        rate = g->sink_khz; /* this monitor does not take the rate the last one had */
                }
                if (blocks && (blocks != g->edid_blocks || memcmp(edid, g->edid, (size_t)blocks * 128) != 0)) {
                    memcpy(g->edid, edid, sizeof(edid));
                    g->edid_blocks = blocks;
                    g->mode_count = edid_collect_timings(g->edid, blocks, g->modes, 0, MAX_MODES);
                    new_monitor = true;
                }
                if (g->displayport && g->atom_ready) {
                    link_restore(g, rate);
                    watch_link = true;
                    retrains = pause = 0;
                }
                g->connected = true;
                back = true;
                switch_mode = new_monitor && g->mode_count && !mode_listed(g, &g->current);
            }
        } else {
            absent = 0;
            if (link_ok) {
                retrains = pause = 0;
            } else if (watch_link && pause) {
                pause--;
            } else if (watch_link && g->atom_ready) {
                klog_info("amdgpu: the monitor lost the DisplayPort link: training it again");
                link_restore(g, g->link_khz);
                if (++retrains >= 3)
                    pause = 10; /* it does not hold: try again every ten seconds only */
            }
        }
        display_unlock(d);

        if (lost)
            display_set_connected(0, false);
        if (back) {
            display_set_connected(0, true);
            if (new_monitor)
                publish_modes(g, d, true, false);
            if (switch_mode) {
                klog_info("amdgpu: another monitor: switching to its best mode");
                kmsg_logfile_write();
                status_t result = display_set_mode(0, 0);
                klog_info("amdgpu: the switch %s", STATUS_IS_ERROR(result) ? "failed" : "is done");
                kmsg_logfile_write();
            }
        }
    }
}

static display_ops_t amdgpu_ops; /* filled with what works on this machine */

/* --- Reading the firmware's state ------------------------------------------------------ */

/* Is  somewhere in ? */
static bool contains(const char *text, const char *word)
{
    size_t length = strlen(word);

    for (; *text; text++) {
        if (strncmp(text, word, length) == 0)
            return true;
    }
    return false;
}

static const char *connection_name(amdgpu_t *g)
{
    static const char *const modes[8] = { "DisplayPort", "LVDS", "DVI", "HDMI", "?", "DisplayPort (MST)", "?", "?" };

    g->encoder = -1;
    for (int i = 0; i < DIG_COUNT; i++) {
        uint32_t back = rd(g, DIG_BE_CNTL(i));
        /* The back end names its front ends as a bit mask; a back end that is on has a mode and a front end.
         * (The front end is asked last, and only of an encoder that is on: see transmitter_runs().) */
        if ((rd(g, DIG_BE_EN_CNTL(i)) & 1u) && ((back >> 8) & 0x7F & (1u << i)) &&
            (rd(g, DIG_FE_CNTL(i)) & 7) == (uint32_t)g->otg) {
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
    /*
     * With the DisplayPort DTO the phase is the pixel clock in Hz: the refresh rate follows exactly. Without it
     * (HDMI) the clock is the PHY's PLL and is found later, in the monitor's EDID.
     */
    if (h_total > 1 && v_total > 1) {
        uint32_t h_sync = rd(g, OTG_H_SYNC(m)), v_sync = rd(g, OTG_V_SYNC(m));
        bool dto = rd(g, PIXEL_RATE_CNTL(m)) & (1u << 4);
        display_timing_t *now = &g->current;
        if (dto)
            g->refresh_mhz = (uint32_t)((uint64_t)rd(g, DP_DTO_PHASE(m)) * 1000 / ((uint64_t)h_total * v_total));
        /* The timing generator counts from the start of the sync pulse; blanking ends where the picture begins. */
        now->khz = dto ? rd(g, DP_DTO_PHASE(m)) / 1000 : 0;
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
        uint32_t back = rd(g, DIG_BE_CNTL(i)), on = rd(g, DIG_BE_EN_CNTL(i));
        if (on != 0xFFFFFFFF && (on & 1u))
            klog_info("amdgpu: encoder %d: front 0x%x, back 0x%x (on 0x%x), link 0x%x, stream 0x%x, video timing 0x%x, "
                      "N %u, M %u", i, rd(g, DIG_FE_CNTL(i)), back, on, rd(g, DP_LINK_CNTL(i)), rd(g, DP_VID_STREAM_CNTL(i)),
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
    char option[48];

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
    if (g->regs_size > (1u << 20))
        g->regs_size = 1u << 20;
    g->regs = (volatile uint8_t *)vmm_map_mmio(pci->bars[5].phys, g->regs_size, VM_UNCACHED);
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
    atom_load(g, device);
    /* amdgpu=...,noddc / noflip / nopointer / novblank leave one part out: for finding what a problem comes from. */
    bool has_option = cmdline_value("amdgpu", option, sizeof(option));
    bool no_ddc = has_option && contains(option, "noddc"), no_flip = has_option && contains(option, "noflip");
    bool no_pointer = has_option && contains(option, "nopointer"), no_vblank = has_option && contains(option, "novblank");
    g->trace = has_option && contains(option, "trace");
    g->phy_on = true; /* the firmware's connector shows a picture */
    klog_debug("amdgpu: PHY control: 0x%x, 0x%x, 0x%x", rd(g, PHY_CNTL6(0)), rd(g, PHY_CNTL6(1)), rd(g, PHY_CNTL6(2)));
    klog_info("amdgpu: hot plug pins with something plugged in: 0x%x", hpd_pins(g));
    (void)no_ddc;
    if (!g->trace) /* (with the trace the log is needed for other things) */
        dump_state(g);
    else
        klog_info("amdgpu: the register accesses of the video BIOS's tables are logged");
    uint32_t measured = measure_refresh(g), refresh = g->refresh_mhz ? g->refresh_mhz : measured;
    klog_info("amdgpu: %u.%02u frames per second (%s; the frame counter says about %u)", refresh / 1000,
              refresh % 1000 / 10, g->refresh_mhz ? "from the pixel clock" : "measured", measured / 1000);
    bool native = has_option && strncmp(option, "native", 6) == 0;
    if (!native && (!has_option || strncmp(option, "on", 2) != 0)) {
        klog_info("amdgpu: nothing changed; boot with amdgpu=native (or amdgpu=on to keep the firmware's mode)");
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

    if (measured >= 10000 && !no_vblank) {
        amdgpu_ops.wait_vblank = amdgpu_wait_vblank;
        void *second = room && !no_flip ? (void *)vmm_map_mmio(g->aperture + g->framebuffers[1], d->info.size, VM_WRITE_COMBINING)
                            : NULL;
        if (second) {
            memset(second, 0, (size_t)d->info.size);
            amdgpu_ops.flip = amdgpu_flip;
        }
    } else if (!no_vblank) {
        klog_warn("amdgpu: the frame counter does not run: no vertical blank timing");
    }
    if (room && !no_pointer)
        g->cursor = (volatile uint32_t *)vmm_map_mmio(g->aperture + g->cursor_offset, PAGE_SIZE * 4, VM_WRITE_COMBINING);
    if (g->cursor) {
        for (uint32_t i = 0; i < JELLY_CURSOR_SIZE * JELLY_CURSOR_SIZE; i++)
            g->cursor[i] = 0; /* transparent until the display server sets an image */
        amdgpu_ops.cursor_image = amdgpu_cursor_image;
        amdgpu_ops.cursor_move = amdgpu_cursor_move;
    }
    /*
     * Modes can be switched on a DisplayPort link whose speed is known (pixel clock from the DTO), and on HDMI
     * or DVI if the pixel clock on the screen was found in the EDID and the video BIOS can set the PHY's PLL.
     */
    bool switchable = g->encoder >= 0 && g->current.khz && measured >= 10000 &&
                      (g->displayport ? g->link_khz && (rd(g, PIXEL_RATE_CNTL(g->otg)) & (1u << 4))
                                      : g->atom_ready && g->pixel_clock_table);
    g->lit = true;
    if (switchable)
        amdgpu_ops.set_mode = amdgpu_set_mode;
    else
        klog_info("amdgpu: the mode cannot be switched here (%s)",
                  g->displayport ? "the link or the pixel clock is not known"
                                 : "HDMI: the pixel clock on the screen or the video BIOS's table for it is not known");
    amdgpu_ops.panic = amdgpu_panic;
    status = display_set_driver(0, &amdgpu_ops, g, amdgpu_ops.flip ? g->aperture + g->framebuffers[1] : 0);
    if (!STATUS_IS_ERROR(status))
        publish_modes(g, d, switchable, true);
    (void)refresh;
    /* From now on the connection is watched. */
    if (switchable && (g->aux_engine >= 0 || !g->displayport)) {
        thread_t *watcher;
        g->connected = true;
        if (STATUS_IS_ERROR(thread_create_kernel("amdgpu-hotplug", hotplug_thread, g, THREAD_PRIORITY_KERNEL, &watcher))) {
            klog_warn("amdgpu: no thread to watch the connection");
        } else {
            thread_start(watcher);
            object_release(&watcher->object);
        }
    }
    /* amdgpu=native: the monitor's best mode (the first of the list) right away, if it is not the firmware's. */
    if (native && switchable && g->mode_count && !display_timing_same(&g->modes[0], &g->current)) {
        const display_timing_t *best = &g->modes[0];
        uint32_t hz = display_timing_hz100(best);
        klog_info("amdgpu: amdgpu=native: the monitor's best mode is %ux%u at %u.%02u Hz", best->ha, best->va, hz / 100,
                  hz % 100);
        if (STATUS_IS_ERROR(display_set_mode(0, 0)))
            klog_warn("amdgpu: the best mode does not come up: the firmware's mode stays");
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
