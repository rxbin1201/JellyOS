/*
 * Monitor timings and EDID. See edid.h.
 */

#include "drivers/graphics/edid.h"

#include <stddef.h>

bool edid_block_ok(const uint8_t *block)
{
    uint8_t sum = 0;
    for (int i = 0; i < EDID_BLOCK; i++)
        sum = (uint8_t)(sum + block[i]);
    return sum == 0;
}

bool edid_header_ok(const uint8_t *edid)
{
    return edid[0] == 0 && edid[1] == 0xFF && edid[7] == 0;
}

bool edid_timing_parse(const uint8_t *d, display_timing_t *t)
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

uint32_t display_timing_hz100(const display_timing_t *t)
{
    return t->ht && t->vt ? (uint32_t)((uint64_t)t->khz * 100000 / ((uint64_t)t->ht * t->vt)) : 0;
}

uint32_t display_timing_mhz(const display_timing_t *t)
{
    return t->ht && t->vt ? (uint32_t)((uint64_t)t->khz * 1000000 / ((uint64_t)t->ht * t->vt)) : 0;
}

bool display_timing_same(const display_timing_t *a, const display_timing_t *b)
{
    uint32_t difference = a->khz > b->khz ? a->khz - b->khz : b->khz - a->khz;
    return a->ha == b->ha && a->va == b->va && a->ht == b->ht && a->vt == b->vt && difference <= a->khz / 100;
}

uint32_t display_timing_add(display_timing_t *list, uint32_t count, uint32_t max, const display_timing_t *t)
{
    for (uint32_t i = 0; i < count; i++) {
        if (display_timing_same(&list[i], t))
            return count;
    }
    if (count < max)
        list[count++] = *t;
    return count;
}

bool display_timing_listed(const display_timing_t *list, uint32_t count, const display_timing_t *t)
{
    uint32_t hz = display_timing_hz100(t);

    for (uint32_t i = 0; i < count; i++) {
        uint32_t other = display_timing_hz100(&list[i]);
        if (list[i].ha == t->ha && list[i].va == t->va && (hz > other ? hz - other : other - hz) < 100)
            return true;
    }
    return false;
}

/* --- Timings a monitor names without giving their numbers ----------------------------------- */

/* A timing as the standards print it: pixel clock, then active, sync start, sync end and total of both directions. */
typedef struct {
    uint8_t  code;       /* CTA: the video code; VESA: REDUCED for a timing with reduced blanking, else 0 */
    uint32_t khz;
    uint16_t ha, hss, hse, ht, va, vss, vse, vt;
    bool     hpos, vpos; /* sync polarity positive */
} standard_timing_t;

#define REDUCED 1

/* VESA Display Monitor Timings: the modes that standard and established timings can mean. */
static const standard_timing_t vesa_timings[] = {
    { 0, 25175, 640, 656, 752, 800, 480, 490, 492, 525, false, false },             /* 640x480 at 60 Hz */
    { 0, 31500, 640, 664, 704, 832, 480, 489, 492, 520, false, false },             /* 640x480 at 72 Hz */
    { 0, 31500, 640, 656, 720, 840, 480, 481, 484, 500, false, false },             /* 640x480 at 75 Hz */
    { 0, 40000, 800, 840, 968, 1056, 600, 601, 605, 628, true, true },              /* 800x600 at 60 Hz */
    { 0, 50000, 800, 856, 976, 1040, 600, 637, 643, 666, true, true },              /* 800x600 at 72 Hz */
    { 0, 49500, 800, 816, 896, 1056, 600, 601, 604, 625, true, true },              /* 800x600 at 75 Hz */
    { 0, 65000, 1024, 1048, 1184, 1344, 768, 771, 777, 806, false, false },         /* 1024x768 at 60 Hz */
    { 0, 75000, 1024, 1048, 1184, 1328, 768, 771, 777, 806, false, false },         /* 1024x768 at 70 Hz */
    { 0, 78750, 1024, 1040, 1136, 1312, 768, 769, 772, 800, true, true },           /* 1024x768 at 75 Hz */
    { 0, 108000, 1152, 1216, 1344, 1600, 864, 865, 868, 900, true, true },          /* 1152x864 at 75 Hz */
    { 0, 74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, true, true },           /* 1280x720 at 60 Hz */
    { REDUCED, 68250, 1280, 1328, 1360, 1440, 768, 771, 778, 790, true, false },    /* 1280x768 at 60 Hz */
    { 0, 79500, 1280, 1344, 1472, 1664, 768, 771, 778, 798, false, true },
    { REDUCED, 71000, 1280, 1328, 1360, 1440, 800, 803, 809, 823, true, false },    /* 1280x800 at 60 Hz */
    { 0, 83500, 1280, 1352, 1480, 1680, 800, 803, 809, 831, false, true },
    { 0, 108000, 1280, 1376, 1488, 1800, 960, 961, 964, 1000, true, true },         /* 1280x960 at 60 Hz */
    { 0, 108000, 1280, 1328, 1440, 1688, 1024, 1025, 1028, 1066, true, true },      /* 1280x1024 at 60 Hz */
    { 0, 135000, 1280, 1296, 1440, 1688, 1024, 1025, 1028, 1066, true, true },      /* 1280x1024 at 75 Hz */
    { 0, 85500, 1360, 1424, 1536, 1792, 768, 771, 777, 795, true, true },           /* 1360x768 at 60 Hz */
    { 0, 85500, 1366, 1436, 1579, 1792, 768, 771, 774, 798, true, true },           /* 1366x768 at 60 Hz */
    { REDUCED, 101000, 1400, 1448, 1480, 1560, 1050, 1053, 1057, 1080, true, false }, /* 1400x1050 at 60 Hz */
    { 0, 121750, 1400, 1488, 1632, 1864, 1050, 1053, 1057, 1089, false, true },
    { REDUCED, 88750, 1440, 1488, 1520, 1600, 900, 903, 909, 926, true, false },    /* 1440x900 at 60 Hz */
    { 0, 106500, 1440, 1520, 1672, 1904, 900, 903, 909, 934, false, true },
    { REDUCED, 108000, 1600, 1624, 1704, 1800, 900, 901, 904, 1000, true, true },   /* 1600x900 at 60 Hz */
    { 0, 162000, 1600, 1664, 1856, 2160, 1200, 1201, 1204, 1250, true, true },      /* 1600x1200 at 60 Hz */
    { REDUCED, 119000, 1680, 1728, 1760, 1840, 1050, 1053, 1059, 1080, true, false }, /* 1680x1050 at 60 Hz */
    { 0, 146250, 1680, 1784, 1960, 2240, 1050, 1053, 1059, 1089, false, true },
    { 0, 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, true, true },      /* 1920x1080 at 60 Hz */
    { REDUCED, 154000, 1920, 1968, 2000, 2080, 1200, 1203, 1209, 1235, true, false }, /* 1920x1200 at 60 Hz */
    { 0, 193250, 1920, 2056, 2256, 2592, 1200, 1203, 1209, 1245, false, true },
    { 0, 234000, 1920, 2048, 2256, 2600, 1440, 1441, 1444, 1500, false, true },     /* 1920x1440 at 60 Hz */
    { REDUCED, 162000, 2048, 2074, 2154, 2250, 1152, 1153, 1156, 1200, true, true }, /* 2048x1152 at 60 Hz */
    { REDUCED, 268500, 2560, 2608, 2640, 2720, 1600, 1603, 1609, 1646, true, false }, /* 2560x1600 at 60 Hz */
    { 0, 348500, 2560, 2752, 3032, 3504, 1600, 1603, 1609, 1658, false, true },
};

/*
 * CTA-861 video codes, the progressive ones with square pixels. (720x480 and 720x576 are meant to be stretched
 * to 4:3 or 16:9 and would show a desktop distorted; interlaced timings are not driven.)
 */
static const standard_timing_t cta_timings[] = {
    { 1, 25175, 640, 656, 752, 800, 480, 490, 492, 525, false, false },             /* 640x480 at 60 Hz */
    { 4, 74250, 1280, 1390, 1430, 1650, 720, 725, 730, 750, true, true },           /* 1280x720 at 60 Hz */
    { 16, 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, true, true },     /* 1920x1080 at 60 Hz */
    { 19, 74250, 1280, 1720, 1760, 1980, 720, 725, 730, 750, true, true },          /* 1280x720 at 50 Hz */
    { 31, 148500, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, true, true },     /* 1920x1080 at 50 Hz */
    { 32, 74250, 1920, 2558, 2602, 2750, 1080, 1084, 1089, 1125, true, true },      /* 1920x1080 at 24 Hz */
    { 33, 74250, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, true, true },      /* 1920x1080 at 25 Hz */
    { 34, 74250, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, true, true },      /* 1920x1080 at 30 Hz */
    { 41, 148500, 1280, 1720, 1760, 1980, 720, 725, 730, 750, true, true },         /* 1280x720 at 100 Hz */
    { 47, 148500, 1280, 1390, 1430, 1650, 720, 725, 730, 750, true, true },         /* 1280x720 at 120 Hz */
    { 63, 297000, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, true, true },     /* 1920x1080 at 120 Hz */
    { 64, 297000, 1920, 2448, 2492, 2640, 1080, 1084, 1089, 1125, true, true },     /* 1920x1080 at 100 Hz */
    { 93, 297000, 3840, 5116, 5204, 5500, 2160, 2168, 2178, 2250, true, true },     /* 3840x2160 at 24 Hz */
    { 94, 297000, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, true, true },     /* 3840x2160 at 25 Hz */
    { 95, 297000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, true, true },     /* 3840x2160 at 30 Hz */
    { 96, 594000, 3840, 4896, 4984, 5280, 2160, 2168, 2178, 2250, true, true },     /* 3840x2160 at 50 Hz */
    { 97, 594000, 3840, 4016, 4104, 4400, 2160, 2168, 2178, 2250, true, true },     /* 3840x2160 at 60 Hz */
};

/* Established timings: bit 7 of byte 35 first. Width, height and refresh rate; 0: not a mode of the table. */
static const uint16_t established[17][3] = {
    { 0, 0, 0 },        /* 720x400 at 70 Hz */
    { 0, 0, 0 },        /* 720x400 at 88 Hz */
    { 640, 480, 60 },
    { 0, 0, 0 },        /* 640x480 at 67 Hz (Apple) */
    { 640, 480, 72 },
    { 640, 480, 75 },
    { 0, 0, 0 },        /* 800x600 at 56 Hz */
    { 800, 600, 60 },
    { 800, 600, 72 },
    { 800, 600, 75 },
    { 0, 0, 0 },        /* 832x624 at 75 Hz (Apple) */
    { 0, 0, 0 },        /* 1024x768 at 87 Hz, interlaced */
    { 1024, 768, 60 },
    { 1024, 768, 70 },
    { 1024, 768, 75 },
    { 1280, 1024, 75 },
    { 0, 0, 0 },        /* 1152x870 at 75 Hz (Apple) */
};

static void timing_from(const standard_timing_t *s, display_timing_t *t)
{
    *t = (display_timing_t){ .khz = s->khz, .ha = s->ha, .hso = (uint32_t)(s->hss - s->ha), .hsw = (uint32_t)(s->hse - s->hss),
                             .ht = s->ht, .va = s->va, .vso = (uint32_t)(s->vss - s->va),
                             .vsw = (uint32_t)(s->vse - s->vss), .vt = s->vt, .hpos = s->hpos, .vpos = s->vpos };
}

/* Add a timing that comes from a table, unless the list has the mode already. */
static uint32_t add_named(display_timing_t *list, uint32_t count, uint32_t max, const standard_timing_t *s)
{
    display_timing_t t;

    timing_from(s, &t);
    if (count < max && !display_timing_listed(list, count, &t))
        list[count++] = t;
    return count;
}

/*
 * VESA's timing for width x height at `hz`. Where the list has two, a flat panel (`reduced`) gets the one with
 * reduced blanking, which needs the slower pixel clock; a monitor with an analog input the classic one.
 */
static const standard_timing_t *vesa_timing(uint32_t width, uint32_t height, uint32_t hz, bool reduced)
{
    const standard_timing_t *found = NULL;

    for (uint32_t i = 0; i < sizeof(vesa_timings) / sizeof(vesa_timings[0]); i++) {
        const standard_timing_t *s = &vesa_timings[i];
        uint32_t rate = (uint32_t)(((uint64_t)s->khz * 1000 + (uint64_t)s->ht * s->vt / 2) / ((uint64_t)s->ht * s->vt));
        if (s->ha != width || s->va != height || rate != hz)
            continue;
        if (!found || (s->code == REDUCED) == reduced)
            found = s;
    }
    return found;
}

/* A standard timing: two bytes for width, aspect ratio and refresh rate. */
static uint32_t add_standard(const uint8_t *edid, const uint8_t *code, display_timing_t *list, uint32_t count,
                             uint32_t max)
{
    if ((code[0] == 0x01 && code[1] == 0x01) || code[0] == 0)
        return count; /* unused */
    uint32_t width = ((uint32_t)code[0] + 31) * 8, hz = (code[1] & 0x3Fu) + 60, height;
    bool old = edid[18] == 1 && edid[19] < 3; /* before EDID 1.3 the first ratio meant 1:1 */

    switch (code[1] >> 6) {
    case 0:
        height = old ? width : width * 10 / 16;
        break;
    case 1:
        height = width * 3 / 4;
        break;
    case 2:
        height = width * 4 / 5;
        break;
    default:
        height = width * 9 / 16;
        break;
    }
    if (width == 1360 && height == 765) {
        /* 1366x768 cannot be said in this form (the width is a multiple of 8): this is how monitors say it. */
        width = 1366;
        height = 768;
    }
    const standard_timing_t *s = vesa_timing(width, height, hz, edid[20] & 0x80);
    return s ? add_named(list, count, max, s) : count;
}

/* The video codes of a CTA extension block: its data blocks lie between byte 4 and the detailed timings. */
static uint32_t add_video_codes(const uint8_t *e, display_timing_t *list, uint32_t count, uint32_t max)
{
    uint32_t end = e[2] >= 4 && e[2] <= 127 ? e[2] : 4;

    for (uint32_t i = 4; i < end;) {
        uint32_t tag = e[i] >> 5, length = e[i] & 0x1F;
        if (i + 1 + length > end)
            break;
        for (uint32_t k = 0; tag == 2 && k < length; k++) {
            uint8_t code = e[i + 1 + k];
            if (code >= 129 && code <= 192)
                code &= 0x7F; /* codes 1 to 64 with the mark "native" */
            for (uint32_t n = 0; n < sizeof(cta_timings) / sizeof(cta_timings[0]); n++) {
                if (cta_timings[n].code == code)
                    count = add_named(list, count, max, &cta_timings[n]);
            }
        }
        i += 1 + length;
    }
    return count;
}

uint32_t edid_collect_timings(const uint8_t *edid, int blocks, display_timing_t *list, uint32_t count, uint32_t max)
{
    if (blocks < 1)
        return count;
    /* The monitor's own numbers first: detailed timings. */
    for (int block = 0; block < blocks; block++) {
        const uint8_t *e = edid + EDID_BLOCK * block;
        uint32_t first = block == 0 ? 54 : e[2], end = block == 0 ? 126 : 127;
        if (block > 0 && (e[0] != 0x02 || first < 4))
            continue; /* not a CTA extension, or one without detailed timings */
        for (uint32_t i = first; i + 18 <= end; i += 18) {
            display_timing_t t;
            if (edid_timing_parse(e + i, &t) && !t.interlaced && t.ha >= 640 && t.va >= 400)
                count = display_timing_add(list, count, max, &t);
        }
    }
    /* Then what it names: video codes (what a television is driven with), standard and established timings. */
    for (int block = 1; block < blocks; block++) {
        if (edid[EDID_BLOCK * block] == 0x02)
            count = add_video_codes(edid + EDID_BLOCK * block, list, count, max);
    }
    for (uint32_t i = 38; i < 54; i += 2)
        count = add_standard(edid, edid + i, list, count, max);
    for (uint32_t i = 54; i + 18 <= 126; i += 18) {
        /* A descriptor that is not a timing can hold six more standard timings (tag 0xFA). */
        const uint8_t *d = edid + i;
        for (uint32_t k = 0; d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == 0xFA && k < 6; k++)
            count = add_standard(edid, d + 5 + 2 * k, list, count, max);
    }
    for (uint32_t bit = 0; bit < 17; bit++) {
        const standard_timing_t *s = NULL;
        if ((edid[35 + bit / 8] & (0x80u >> (bit % 8))) && established[bit][0])
            s = vesa_timing(established[bit][0], established[bit][1], established[bit][2], edid[20] & 0x80);
        if (s)
            count = add_named(list, count, max, s);
    }
    return count;
}

bool display_timing_better(const display_timing_t *a, const display_timing_t *b)
{
    uint64_t area_a = (uint64_t)a->ha * a->va, area_b = (uint64_t)b->ha * b->va;
    bool fluid_a = display_timing_hz100(a) >= 4800, fluid_b = display_timing_hz100(b) >= 4800;

    if (fluid_a != fluid_b)
        return fluid_a;
    return area_a > area_b || (area_a == area_b && display_timing_hz100(a) > display_timing_hz100(b));
}

void display_timing_sort(display_timing_t *list, uint32_t count)
{
    for (uint32_t i = 1; i < count; i++) {
        display_timing_t t = list[i];
        uint32_t k = i;
        for (; k > 0 && display_timing_better(&t, &list[k - 1]); k--)
            list[k] = list[k - 1];
        list[k] = t;
    }
}
