/*
 * HDMI, the part that is the same for every GPU.
 *
 * An HDMI connector can be driven like a DVI one: pixels and nothing else.
 * That works with every monitor and is how the firmware does it. HDMI
 * proper adds, between the pixels, packets with data about them, and since
 * HDMI 2.0 a faster signal:
 *
 *   the sink            what the monitor says about itself in the CTA
 *                       extension of its EDID: that it understands HDMI at
 *                       all, how fast a signal it takes, whether it has
 *                       status and control registers (SCDC)
 *   the AVI info frame  a packet in every frame that says what the pixels
 *                       are: RGB, which of the standard's video timings,
 *                       full range, picture shape
 *   scrambling          above 340 MHz (HDMI 2.0, up to 600 MHz) the data is
 *                       scrambled and the clock lane runs at a quarter of
 *                       its rate. The monitor must be told beforehand, in
 *                       its SCDC registers, which are reached over the DDC
 *                       lines like the EDID
 *
 * A driver brings what touches its hardware: reading and writing I2C on the
 * DDC lines, putting a packet into its encoder, switching scrambling on.
 */

#ifndef DRIVERS_GRAPHICS_HDMI_H
#define DRIVERS_GRAPHICS_HDMI_H

#include "drivers/graphics/edid.h"

#define HDMI_DVI_MAX_KHZ    340000 /* fastest signal without scrambling (HDMI 1.4b; single link DVI ends at 165000) */
#define HDMI_INFOFRAME_SIZE 17     /* an AVI info frame: 3 bytes of header, the checksum, 13 bytes */
#define HDMI_SCDC_ADDRESS   0x54   /* the monitor's status and control registers as an I2C device */

/* What the monitor's EDID says about its HDMI input. */
typedef struct {
    bool     hdmi;                    /* it has the HDMI vendor block: it understands packets, not only pixels */
    uint32_t max_tmds_khz;            /* the fastest signal that block names, 0 if it names none */
    bool     scdc;                    /* HDMI 2.0: it has status and control registers */
    uint32_t max_character_khz;       /* HDMI 2.0: the fastest signal it takes, 0 if not said */
    bool     quantization_selectable; /* it follows the range (full or limited) the AVI info frame names */
    bool     basic_audio;
} hdmi_sink_t;

void     hdmi_sink_read(const uint8_t *edid, int blocks, hdmi_sink_t *sink);

/*
 * The fastest pixel clock (8 bits per colour) for a mode on this monitor from a source that can make
 * source_khz: beyond HDMI_DVI_MAX_KHZ only where scrambling can be agreed on.
 */
uint32_t hdmi_pixel_limit(const hdmi_sink_t *sink, uint32_t source_khz);

/*
 * The AVI info frame for timing t as full range RGB: header (type, version, length), checksum, 13 data bytes.
 * `hdmi2`: the signal is an HDMI 2.0 one (video codes above 64 may be named).
 */
void     hdmi_avi_infoframe(const display_timing_t *t, bool hdmi2, uint8_t frame[HDMI_INFOFRAME_SIZE]);

/* I2C on the monitor's DDC lines, as the driver's hardware does it. */
typedef struct {
    void *context;
    /* Write `length` bytes to the 7-bit address. */
    bool (*write)(void *context, uint8_t address, const uint8_t *data, int length);
    /* Write the offset, then read `length` bytes. */
    bool (*read)(void *context, uint8_t address, uint8_t offset, uint8_t *buffer, int length);
} hdmi_ddc_t;

/*
 * Tell the monitor what signal comes next: scrambled with the clock lane at a quarter of its rate (above
 * HDMI_DVI_MAX_KHZ), or plain. Before the signal changes. False if the monitor does not take it.
 */
bool     hdmi_scdc_configure(const hdmi_ddc_t *ddc, bool scrambled);
/* Does the monitor see a scrambled signal (its scrambler status)? For the log. */
bool     hdmi_scdc_scrambling_seen(const hdmi_ddc_t *ddc);
/*
 * Has the monitor found the clock and locked onto all three data lanes? 1 yes, 0 no, -1 it does not say (its
 * registers cannot be read). The one way to learn that a fast signal really arrives.
 */
int      hdmi_scdc_locked(const hdmi_ddc_t *ddc);

#endif
