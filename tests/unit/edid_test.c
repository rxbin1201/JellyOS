/*
 * Host unit tests for drivers/graphics/edid.c: the modes a monitor names in
 * its EDID.
 *
 * The test builds the EDID of a monitor by hand, with every way of naming a
 * mode in it: detailed timings in the base block and in a CTA extension,
 * CTA video codes, standard timings (also in a descriptor) and established
 * timings; with modes named twice, an interlaced one, one with pixels that
 * are not square and one no table has.
 */

#include <stdio.h>
#include <string.h>

#include "drivers/graphics/edid.h"

static int failures, checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("unit: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                        \
    } while (0)

/* A detailed timing descriptor: 18 bytes. */
static void detailed(uint8_t *d, uint32_t khz, uint32_t ha, uint32_t hblank, uint32_t va, uint32_t vblank, uint32_t hso,
                     uint32_t hsw, uint32_t vso, uint32_t vsw, bool hpos, bool vpos, bool interlaced)
{
    memset(d, 0, 18);
    d[0] = (uint8_t)(khz / 10);
    d[1] = (uint8_t)(khz / 10 >> 8);
    d[2] = (uint8_t)ha;
    d[3] = (uint8_t)hblank;
    d[4] = (uint8_t)((ha >> 8) << 4 | hblank >> 8);
    d[5] = (uint8_t)va;
    d[6] = (uint8_t)vblank;
    d[7] = (uint8_t)((va >> 8) << 4 | vblank >> 8);
    d[8] = (uint8_t)hso;
    d[9] = (uint8_t)hsw;
    d[10] = (uint8_t)((vso & 0xF) << 4 | (vsw & 0xF));
    d[11] = (uint8_t)((hso >> 8) << 6 | (hsw >> 8) << 4 | (vso >> 4) << 2 | vsw >> 4);
    d[17] = (uint8_t)((interlaced ? 0x80 : 0) | 3 << 3 | (vpos ? 4 : 0) | (hpos ? 2 : 0));
}

static void checksum(uint8_t *block)
{
    uint8_t sum = 0;

    for (int i = 0; i < 127; i++)
        sum = (uint8_t)(sum + block[i]);
    block[127] = (uint8_t)(0 - sum);
}

/* A standard timing: width / 8 - 31, then aspect ratio (0: 16:10, 1: 4:3, 2: 5:4, 3: 16:9) and refresh rate - 60. */
static void standard(uint8_t *code, uint32_t width, uint32_t ratio, uint32_t hz)
{
    code[0] = (uint8_t)(width / 8 - 31);
    code[1] = (uint8_t)(ratio << 6 | (hz - 60));
}

static void build(uint8_t *edid)
{
    static const uint8_t header[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    uint8_t *e = edid, *x = edid + EDID_BLOCK;

    memset(edid, 0, 2 * EDID_BLOCK);
    memcpy(e, header, 8);
    e[18] = 1;
    e[19] = 4;    /* EDID 1.4 */
    e[20] = 0x80; /* a digital input */
    /* Established: 720x400 at 70 Hz (no table has it), 640x480 and 800x600 at 60 Hz; 1024x768 at 87 Hz
     * interlaced (not driven) and at 60 Hz, 1280x1024 at 75 Hz. */
    e[35] = 0x80 | 0x20 | 0x01;
    e[36] = 0x10 | 0x08 | 0x01;
    memset(e + 38, 0x01, 16); /* unused standard timings */
    standard(e + 38, 1920, 3, 60);
    standard(e + 40, 1680, 0, 60);
    standard(e + 42, 1280, 2, 60); /* 1280x1024 */
    standard(e + 44, 1360, 3, 60); /* 1360x765: how 1366x768 is said */
    standard(e + 46, 1792, 1, 60); /* 1792x1344: no table has it */
    standard(e + 48, 1280, 1, 60); /* 1280x960 */
    standard(e + 50, 1280, 0, 60); /* 1280x800 */
    detailed(e + 54, 319890, 3440, 160, 1440, 41, 48, 32, 3, 10, true, false, false);
    detailed(e + 72, 148500, 1920, 280, 1080, 45, 88, 44, 4, 5, true, true, false); /* also a standard timing and code 16 */
    e[90 + 3] = 0xFA; /* a descriptor with more standard timings */
    memset(e + 90 + 5, 0x01, 12);
    standard(e + 90 + 5, 1440, 0, 60);
    standard(e + 90 + 7, 1600, 1, 60);
    e[90 + 17] = 0x0A;
    e[108 + 3] = 0xFC; /* the monitor's name */
    memcpy(e + 108 + 5, "Test\\n       ", 13);
    e[126] = 1;
    checksum(e);

    /* The CTA extension: an audio block (skipped), video codes, two detailed timings. */
    static const uint8_t blocks[] = { 0x23, 0x09, 0x07, 0x07,
                                      0x48, 0x90 /* 16, native */, 4, 31, 5 /* 1080i */, 95, 97, 2 /* 720x480 */, 34 };
    x[0] = 0x02;
    x[1] = 3;
    x[2] = (uint8_t)(4 + sizeof(blocks));
    memcpy(x + 4, blocks, sizeof(blocks));
    /* 1280x720 at 60 Hz with the monitor's own numbers, which are not those of code 4; and an interlaced one. */
    detailed(x + x[2], 64000, 1280, 160, 720, 21, 48, 32, 3, 5, true, false, false);
    detailed(x + x[2] + 18, 74250, 1920, 280, 540, 22, 88, 44, 2, 5, true, true, true);
    checksum(x);
}

static const display_timing_t *find(const display_timing_t *list, uint32_t count, uint32_t width, uint32_t height,
                                    uint32_t hz, int *times)
{
    const display_timing_t *found = NULL;

    *times = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t rate = (display_timing_hz100(&list[i]) + 50) / 100;
        if (list[i].ha == width && list[i].va == height && rate == hz) {
            found = &list[i];
            (*times)++;
        }
    }
    return found;
}

int main(void)
{
    static uint8_t edid[2 * EDID_BLOCK];
    display_timing_t list[32];
    const display_timing_t *t;
    int times;

    build(edid);
    CHECK(edid_header_ok(edid) && edid_block_ok(edid) && edid_block_ok(edid + EDID_BLOCK));
    uint32_t count = edid_collect_timings(edid, 2, list, 0, 32);

    /* 3 detailed timings, 4 video codes, 5 standard timings, 2 from the descriptor, 4 established */
    CHECK(count == 18);
    /* The detailed timings come first, in the monitor's order. */
    CHECK(list[0].ha == 3440 && list[0].va == 1440 && list[0].khz == 319890 && list[0].ht == 3600 && list[0].vt == 1481);
    CHECK(list[0].hso == 48 && list[0].hsw == 32 && list[0].vso == 3 && list[0].vsw == 10 && list[0].hpos && !list[0].vpos);
    CHECK(list[1].ha == 1920 && list[2].ha == 1280 && list[2].khz == 64000);

    /* A mode named three times (detailed, standard, video code) is there once. */
    t = find(list, count, 1920, 1080, 60, &times);
    CHECK(t && times == 1 && t->khz == 148500 && t->ht == 2200);
    /* The monitor's own numbers win over the table's: 1280x720 at 60 Hz is not code 4's timing. */
    t = find(list, count, 1280, 720, 60, &times);
    CHECK(t && times == 1 && t->khz == 64000 && t->ht == 1440);

    /* Video codes: every number of the timing from the table. */
    t = find(list, count, 3840, 2160, 60, &times);
    CHECK(t && times == 1 && t->khz == 594000 && t->ht == 4400 && t->vt == 2250 && t->hso == 176 && t->hsw == 88 &&
          t->vso == 8 && t->vsw == 10 && t->hpos && t->vpos);
    t = find(list, count, 1920, 1080, 50, &times);
    CHECK(t && t->khz == 148500 && t->ht == 2640 && t->hso == 528 && t->hsw == 44);
    CHECK(find(list, count, 3840, 2160, 30, &times) && find(list, count, 1920, 1080, 30, &times));
    /* Not: interlaced, pixels that are not square. */
    for (uint32_t i = 0; i < count; i++)
        CHECK(!list[i].interlaced && list[i].va != 540 && !(list[i].ha == 720 && list[i].va == 480));

    /* Standard timings: VESA's numbers; a flat panel gets reduced blanking where there is a choice. */
    t = find(list, count, 1680, 1050, 60, &times);
    CHECK(t && times == 1 && t->khz == 119000 && t->ht == 1840 && t->hpos && !t->vpos);
    t = find(list, count, 1280, 1024, 60, &times);
    CHECK(t && t->khz == 108000 && t->ht == 1688 && t->vt == 1066 && t->hso == 48 && t->hsw == 112 && t->vso == 1 && t->vsw == 3);
    t = find(list, count, 1366, 768, 60, &times);
    CHECK(t && t->khz == 85500 && t->ht == 1792);
    CHECK(find(list, count, 1280, 960, 60, &times) && find(list, count, 1280, 800, 60, &times));
    CHECK(!find(list, count, 1792, 1344, 60, &times) && !find(list, count, 1360, 765, 60, &times));
    /* From the descriptor */
    t = find(list, count, 1440, 900, 60, &times);
    CHECK(t && t->khz == 88750);
    t = find(list, count, 1600, 1200, 60, &times);
    CHECK(t && t->khz == 162000 && t->ht == 2160 && t->vt == 1250);
    /* Established timings */
    t = find(list, count, 640, 480, 60, &times);
    CHECK(t && t->khz == 25175 && t->ht == 800 && t->vt == 525 && !t->hpos && !t->vpos);
    t = find(list, count, 800, 600, 60, &times);
    CHECK(t && t->khz == 40000 && t->ht == 1056);
    t = find(list, count, 1024, 768, 60, &times);
    CHECK(t && t->khz == 65000 && t->ht == 1344);
    t = find(list, count, 1280, 1024, 75, &times);
    CHECK(t && t->khz == 135000);
    CHECK(!find(list, count, 720, 400, 70, &times));

    /* Best first: the largest of 48 Hz and more; the slow ones at the end, whatever their size. */
    display_timing_sort(list, count);
    CHECK(list[0].ha == 3840 && list[0].khz == 594000);
    CHECK(list[1].ha == 3440 && list[2].ha == 1920 && list[2].va == 1200 - 120); /* 1920x1080: 60 before 50 Hz */
    CHECK(list[2].khz == 148500 && list[2].ht == 2200 && list[3].ha == 1920 && list[3].ht == 2640);
    CHECK(list[count - 2].ha == 3840 && list[count - 2].khz == 297000);
    CHECK(list[count - 1].ha == 1920 && list[count - 1].khz == 74250);
    CHECK(list[count - 3].ha == 640);

    /* A list that is full keeps what came first: the detailed timings. */
    count = edid_collect_timings(edid, 2, list, 0, 2);
    CHECK(count == 2 && list[0].ha == 3440 && list[1].ha == 1920);
    /* Without the extension block */
    count = edid_collect_timings(edid, 1, list, 0, 32);
    CHECK(count == 13 && !find(list, count, 3840, 2160, 60, &times) && !find(list, count, 1280, 720, 60, &times));
    CHECK(edid_collect_timings(edid, 0, list, 0, 32) == 0);

    /* A monitor with an analog input gets the classic blanking. */
    edid[20] = 0;
    count = edid_collect_timings(edid, 2, list, 0, 32);
    t = find(list, count, 1680, 1050, 60, &times);
    CHECK(count == 18 && t && t->khz == 146250 && t->ht == 2240 && !t->hpos && t->vpos);
    /* Before EDID 1.3 the first aspect ratio meant 1:1: 1680x1680 and its like are in no table. */
    edid[19] = 2;
    count = edid_collect_timings(edid, 2, list, 0, 32);
    CHECK(count == 15 && !find(list, count, 1680, 1050, 60, &times) && !find(list, count, 1440, 900, 60, &times));

    /* The same size at the same rate, whatever the timing */
    display_timing_t like = { .khz = 138500, .ha = 1920, .ht = 2080, .va = 1080, .vt = 1111 };
    CHECK(display_timing_listed(list, count, &like));
    like.khz = 69250;
    CHECK(!display_timing_listed(list, count, &like) || find(list, count, 1920, 1080, 30, &times));

    printf("unit: edid: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
