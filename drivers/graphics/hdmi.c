/*
 * HDMI, the part that is the same for every GPU. See hdmi.h.
 */

#include "drivers/graphics/hdmi.h"

#include <stddef.h>

/* Data blocks of a CTA extension (their tag is the upper three bits of the first byte) */
#define BLOCK_VENDOR   3
#define BLOCK_EXTENDED 7
#define EXTENDED_VIDEO_CAPABILITY 0

/* SCDC registers */
#define SCDC_SINK_VERSION     0x01
#define SCDC_SOURCE_VERSION   0x02
#define SCDC_TMDS_CONFIG      0x20 /* bit 0: scrambling, bit 1: the clock lane at a quarter of its rate */
#define SCDC_SCRAMBLER_STATUS 0x21 /* bit 0: the monitor sees a scrambled signal */
#define SCDC_STATUS_FLAGS     0x40 /* bit 0: clock found, bits 1-3: locked onto data lanes 0-2 */

void hdmi_sink_read(const uint8_t *edid, int blocks, hdmi_sink_t *sink)
{
    *sink = (hdmi_sink_t){ 0 };
    for (int block = 1; block < blocks; block++) {
        const uint8_t *e = edid + EDID_BLOCK * block;
        if (e[0] != 0x02)
            continue; /* not a CTA extension */
        uint32_t end = e[2] >= 4 && e[2] <= 127 ? e[2] : 4;
        if (e[1] >= 2 && (e[3] & 0x40))
            sink->basic_audio = true;
        for (uint32_t i = 4; i < end;) {
            uint32_t tag = e[i] >> 5, length = e[i] & 0x1F;
            const uint8_t *d = e + i + 1;
            if (i + 1 + length > end)
                break;
            if (tag == BLOCK_VENDOR && length >= 5 && d[0] == 0x03 && d[1] == 0x0C && d[2] == 0x00) {
                /* HDMI Licensing (00-0C-03): the block every HDMI sink has */
                sink->hdmi = true;
                if (length >= 7)
                    sink->max_tmds_khz = (uint32_t)d[6] * 5000;
            } else if (tag == BLOCK_VENDOR && length >= 6 && d[0] == 0xD8 && d[1] == 0x5D && d[2] == 0xC4) {
                /* HDMI Forum (C4-5D-D8): HDMI 2.0 and later */
                sink->max_character_khz = (uint32_t)d[4] * 5000;
                sink->scdc = d[5] & 0x80;
            } else if (tag == BLOCK_EXTENDED && length >= 2 && d[0] == EXTENDED_VIDEO_CAPABILITY) {
                sink->quantization_selectable = d[1] & 0x40;
            }
            i += 1 + length;
        }
    }
}

uint32_t hdmi_pixel_limit(const hdmi_sink_t *sink, uint32_t source_khz)
{
    uint32_t limit = HDMI_DVI_MAX_KHZ;

    /* Faster needs scrambling, which needs a monitor that can be told about it. */
    if (sink->hdmi && sink->scdc && sink->max_character_khz > limit)
        limit = sink->max_character_khz;
    return limit < source_khz ? limit : source_khz;
}

void hdmi_avi_infoframe(const display_timing_t *t, bool hdmi2, bool full_range, uint8_t frame[HDMI_INFOFRAME_SIZE])
{
    uint8_t *d = frame + 4, code = edid_cta_code(t), sum = 0;

    for (int i = 0; i < HDMI_INFOFRAME_SIZE; i++)
        frame[i] = 0;
    frame[0] = 0x82; /* AVI */
    frame[1] = 2;    /* version */
    frame[2] = 13;   /* bytes that follow the checksum */
    /* RGB; the next byte's "active format" is valid; a computer's picture has no overscanned border. */
    d[0] = 0x10 | 0x02;
    /* Picture shape, if it is one the frame can name; the active format is the picture. */
    d[1] = (uint8_t)((t->ha * 9 == t->va * 16 ? 2u : t->ha * 3 == t->va * 4 ? 1u : 0u) << 4 | 8);
    /* Made by a computer (IT content), and the range of its values. */
    d[2] = (uint8_t)(0x80 | (full_range ? 2u : 1u) << 2);
    /* Which of the standard's timings this is, if any. The codes above 64 came with HDMI 2.0. */
    d[3] = code <= 64 || hdmi2 ? code : 0;
    for (int i = 0; i < HDMI_INFOFRAME_SIZE; i++)
        sum = (uint8_t)(sum + frame[i]);
    frame[3] = (uint8_t)(0 - sum);
}

bool hdmi_scdc_configure(const hdmi_ddc_t *ddc, bool scrambled)
{
    uint8_t version = 0, set[2];

    /* A monitor of version 1 wants to know the source's version, too. (Not all take the write: not checked.) */
    if (ddc->read(ddc->context, HDMI_SCDC_ADDRESS, SCDC_SINK_VERSION, &version, 1) && version == 1) {
        set[0] = SCDC_SOURCE_VERSION;
        set[1] = 1;
        ddc->write(ddc->context, HDMI_SCDC_ADDRESS, set, 2);
    }
    set[0] = SCDC_TMDS_CONFIG;
    set[1] = scrambled ? 0x03 : 0x00;
    return ddc->write(ddc->context, HDMI_SCDC_ADDRESS, set, 2);
}

int hdmi_scdc_locked(const hdmi_ddc_t *ddc)
{
    uint8_t status = 0;

    if (!ddc->read(ddc->context, HDMI_SCDC_ADDRESS, SCDC_STATUS_FLAGS, &status, 1))
        return -1;
    return (status & 0x0F) == 0x0F;
}

bool hdmi_scdc_scrambling_seen(const hdmi_ddc_t *ddc)
{
    uint8_t status = 0;

    return ddc->read(ddc->context, HDMI_SCDC_ADDRESS, SCDC_SCRAMBLER_STATUS, &status, 1) && (status & 1);
}
