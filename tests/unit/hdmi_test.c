/*
 * Host unit tests for drivers/graphics/hdmi.c: what an EDID says about a
 * monitor's HDMI input, the AVI info frame, and the talk with the monitor's
 * status and control registers (SCDC) before a scrambled signal.
 */

#include <stdio.h>
#include <string.h>

#include "drivers/graphics/hdmi.h"

static int failures, checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("unit: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                        \
    } while (0)

/* An EDID of two blocks whose CTA extension holds these data blocks. */
static void build(uint8_t *edid, const uint8_t *blocks, size_t length, uint8_t flags)
{
    uint8_t *x = edid + EDID_BLOCK;

    memset(edid, 0, 2 * EDID_BLOCK);
    edid[1] = edid[2] = edid[3] = edid[4] = edid[5] = edid[6] = 0xFF;
    edid[126] = 1;
    x[0] = 0x02;
    x[1] = 3;
    x[2] = (uint8_t)(4 + length);
    x[3] = flags;
    memcpy(x + 4, blocks, length);
}

/* --- A monitor's SCDC registers, behind I2C ---------------------------------------------- */

static struct {
    uint8_t registers[256];
    bool    there;
    int     writes, reads;
    uint8_t order[8]; /* the registers written, in order */
} scdc;

static bool fake_write(void *context, uint8_t address, const uint8_t *data, int length)
{
    (void)context;
    if (!scdc.there || address != HDMI_SCDC_ADDRESS || length != 2)
        return false;
    if (scdc.writes < 8)
        scdc.order[scdc.writes] = data[0];
    scdc.writes++;
    scdc.registers[data[0]] = data[1];
    return true;
}

static bool fake_read(void *context, uint8_t address, uint8_t offset, uint8_t *buffer, int length)
{
    (void)context;
    if (!scdc.there || address != HDMI_SCDC_ADDRESS)
        return false;
    scdc.reads++;
    for (int i = 0; i < length; i++)
        buffer[i] = scdc.registers[(uint8_t)(offset + i)];
    return true;
}

int main(void)
{
    static uint8_t edid[2 * EDID_BLOCK];
    uint8_t frame[HDMI_INFOFRAME_SIZE], sum;
    hdmi_sink_t sink;

    /* --- The sink ---------------------------------------------------------------------- */

    /* A DVI monitor: no extension block at all, or one without the HDMI block. */
    memset(edid, 0, sizeof(edid));
    hdmi_sink_read(edid, 1, &sink);
    CHECK(!sink.hdmi && !sink.scdc && hdmi_pixel_limit(&sink, 600000) == HDMI_DVI_MAX_KHZ);
    static const uint8_t audio_only[] = { 0x23, 0x09, 0x07, 0x07 };
    build(edid, audio_only, sizeof(audio_only), 0);
    hdmi_sink_read(edid, 2, &sink);
    CHECK(!sink.hdmi && !sink.basic_audio);

    /* HDMI 1.4: the vendor block of HDMI Licensing, with a fastest signal of 300 MHz. */
    static const uint8_t hdmi14[] = { 0x23, 0x09, 0x07, 0x07, /* audio */
                                      0x67, 0x03, 0x0C, 0x00, 0x10, 0x00, 0x00, 0x3C };
    build(edid, hdmi14, sizeof(hdmi14), 0x40);
    hdmi_sink_read(edid, 2, &sink);
    CHECK(sink.hdmi && sink.max_tmds_khz == 300000 && !sink.scdc && sink.max_character_khz == 0 && sink.basic_audio);
    /* No scrambling to agree on: not beyond what works without, whatever the source could do. */
    CHECK(hdmi_pixel_limit(&sink, 600000) == HDMI_DVI_MAX_KHZ && hdmi_pixel_limit(&sink, 300000) == 300000);

    /* HDMI 2.0: also the block of the HDMI Forum (600 MHz, SCDC) and a video capability block (range selectable). */
    static const uint8_t hdmi20[] = { 0x65, 0x03, 0x0C, 0x00, 0x10, 0x00,             /* a short HDMI block */
                                      0xE2, 0x00, 0x40,                               /* video capability */
                                      0x67, 0xD8, 0x5D, 0xC4, 0x01, 0x78, 0x80, 0x00, /* HDMI Forum */
                                      0x42, 0x10, 0x04 };                             /* video codes */
    build(edid, hdmi20, sizeof(hdmi20), 0);
    hdmi_sink_read(edid, 2, &sink);
    CHECK(sink.hdmi && sink.max_tmds_khz == 0 && sink.scdc && sink.max_character_khz == 600000);
    CHECK(sink.quantization_selectable && !sink.basic_audio);
    CHECK(hdmi_pixel_limit(&sink, 600000) == 600000 && hdmi_pixel_limit(&sink, 300000) == 300000);
    sink.max_character_khz = 450000;
    CHECK(hdmi_pixel_limit(&sink, 600000) == 450000);
    /* Without SCDC the faster rate cannot be asked for. */
    sink.scdc = false;
    CHECK(hdmi_pixel_limit(&sink, 600000) == HDMI_DVI_MAX_KHZ);
    /* A block that claims to be longer than the extension has room: not read beyond. */
    static const uint8_t broken[] = { 0x7F, 0x03, 0x0C, 0x00 };
    build(edid, broken, sizeof(broken), 0);
    hdmi_sink_read(edid, 2, &sink);
    CHECK(!sink.hdmi);

    /* --- The AVI info frame --------------------------------------------------------------- */

    /* 1920x1080 at 60 Hz: video code 16, 16:9. */
    display_timing_t t = { .khz = 148500, .ha = 1920, .hso = 88, .hsw = 44, .ht = 2200,
                           .va = 1080, .vso = 4, .vsw = 5, .vt = 1125, .hpos = true, .vpos = true };
    CHECK(edid_cta_code(&t) == 16);
    hdmi_avi_infoframe(&t, false, true, frame);
    CHECK(frame[0] == 0x82 && frame[1] == 2 && frame[2] == 13);
    sum = 0;
    for (int i = 0; i < HDMI_INFOFRAME_SIZE; i++)
        sum = (uint8_t)(sum + frame[i]);
    CHECK(sum == 0);                                /* the checksum makes it so */
    CHECK((frame[4] >> 5) == 0);                    /* RGB */
    CHECK((frame[4] & 0x10) && (frame[4] & 3) == 2); /* active format valid; no overscan */
    CHECK(((frame[5] >> 4) & 3) == 2 && (frame[5] & 0xF) == 8); /* 16:9; the active format is the picture */
    CHECK((frame[6] & 0x80) && ((frame[6] >> 2) & 3) == 2);     /* IT content, full range */
    CHECK(frame[7] == 16);
    for (int i = 8; i < HDMI_INFOFRAME_SIZE; i++)
        CHECK(frame[i] == 0);
    /* The same at 59.94 Hz is the same code. */
    t.khz = 148352;
    CHECK(edid_cta_code(&t) == 16);

    /* A monitor's own timing is none of the standard's: code 0, and no shape the frame could name. */
    display_timing_t wide = { .khz = 319890, .ha = 3440, .hso = 48, .hsw = 32, .ht = 3600,
                              .va = 1440, .vso = 3, .vsw = 10, .vt = 1481, .hpos = true };
    CHECK(edid_cta_code(&wide) == 0);
    hdmi_avi_infoframe(&wide, true, true, frame);
    CHECK(frame[7] == 0 && ((frame[5] >> 4) & 3) == 0);
    sum = 0;
    for (int i = 0; i < HDMI_INFOFRAME_SIZE; i++)
        sum = (uint8_t)(sum + frame[i]);
    CHECK(sum == 0);
    /* 3840x2160 at 60 Hz is code 97, which only an HDMI 2.0 signal may name. */
    display_timing_t uhd = { .khz = 594000, .ha = 3840, .hso = 176, .hsw = 88, .ht = 4400,
                             .va = 2160, .vso = 8, .vsw = 10, .vt = 2250, .hpos = true, .vpos = true };
    CHECK(edid_cta_code(&uhd) == 97);
    hdmi_avi_infoframe(&uhd, true, true, frame);
    CHECK(frame[7] == 97);
    hdmi_avi_infoframe(&uhd, false, true, frame);
    CHECK(frame[7] == 0);
    /* Limited range is said, too. */
    hdmi_avi_infoframe(&uhd, true, false, frame);
    CHECK(((frame[6] >> 2) & 3) == 1);
    sum = 0;
    for (int i = 0; i < HDMI_INFOFRAME_SIZE; i++)
        sum = (uint8_t)(sum + frame[i]);
    CHECK(sum == 0);
    /* 1024x768 is 4:3. */
    display_timing_t xga = { .khz = 65000, .ha = 1024, .hso = 24, .hsw = 136, .ht = 1344, .va = 768, .vso = 3, .vsw = 6, .vt = 806 };
    hdmi_avi_infoframe(&xga, false, true, frame);
    CHECK(frame[7] == 0 && ((frame[5] >> 4) & 3) == 1);

    /* --- SCDC ------------------------------------------------------------------------------ */

    const hdmi_ddc_t ddc = { NULL, fake_write, fake_read };

    /* A monitor of version 1 is told the source's version, then what signal comes. */
    memset(&scdc, 0, sizeof(scdc));
    scdc.there = true;
    scdc.registers[0x01] = 1;
    CHECK(hdmi_scdc_configure(&ddc, true));
    CHECK(scdc.writes == 2 && scdc.order[0] == 0x02 && scdc.order[1] == 0x20);
    CHECK(scdc.registers[0x02] == 1 && scdc.registers[0x20] == 0x03); /* scrambled, the clock at a quarter */
    CHECK(!hdmi_scdc_scrambling_seen(&ddc));
    scdc.registers[0x21] = 1;
    CHECK(hdmi_scdc_scrambling_seen(&ddc));
    /* Locked: the clock and all three data lanes. */
    CHECK(hdmi_scdc_locked(&ddc) == 0);
    scdc.registers[0x40] = 0x07;
    CHECK(hdmi_scdc_locked(&ddc) == 0);
    scdc.registers[0x40] = 0x0F;
    CHECK(hdmi_scdc_locked(&ddc) == 1);
    /* Back to a plain signal. */
    CHECK(hdmi_scdc_configure(&ddc, false) && scdc.registers[0x20] == 0x00);
    /* A monitor that says no version gets only the configuration. */
    memset(&scdc, 0, sizeof(scdc));
    scdc.there = true;
    CHECK(hdmi_scdc_configure(&ddc, true) && scdc.writes == 1 && scdc.order[0] == 0x20);
    /* Nothing answers: said so. */
    scdc.there = false;
    CHECK(!hdmi_scdc_configure(&ddc, true) && !hdmi_scdc_scrambling_seen(&ddc) && hdmi_scdc_locked(&ddc) == -1);

    /* --- The description of the monitor's sound for an audio codec ------------------------ */

    uint8_t eld[HDMI_ELD_SIZE];
    memset(edid, 0, sizeof(edid));
    edid[8] = 0x10;
    edid[9] = 0xAC; /* "DEL" */
    edid[10] = 0x34;
    edid[11] = 0x12;
    hdmi_eld(edid, false, eld);
    CHECK(eld[0] == 0x10 && HDMI_ELD_SIZE % 4 == 0 && 4 + 4 * eld[2] == HDMI_ELD_SIZE);
    CHECK((eld[4] >> 5) == 3 && (eld[4] & 0x1F) == 0);       /* no name: the sound's kinds follow the header */
    CHECK((eld[5] >> 4) == 1 && ((eld[HDMI_ELD_KIND] >> 2) & 3) == 0); /* one kind of sound, HDMI */
    CHECK(eld[7] == 0x01);
    CHECK(eld[16] == 0x10 && eld[17] == 0xAC && eld[18] == 0x34 && eld[19] == 0x12);
    CHECK(eld[20] == 0x09 && eld[21] == 0x07 && eld[22] == 0x01 && eld[23] == 0);
    hdmi_eld(edid, true, eld);
    CHECK(((eld[HDMI_ELD_KIND] >> 2) & 3) == 1 && (eld[5] >> 4) == 1);

    printf("unit: hdmi: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
