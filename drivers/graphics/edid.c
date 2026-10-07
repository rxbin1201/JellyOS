/*
 * Monitor timings and EDID. See edid.h.
 */

#include "drivers/graphics/edid.h"

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

uint32_t edid_collect_timings(const uint8_t *edid, int blocks, display_timing_t *list, uint32_t count, uint32_t max)
{
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
    return count;
}

bool display_timing_better(const display_timing_t *a, const display_timing_t *b)
{
    uint64_t area_a = (uint64_t)a->ha * a->va, area_b = (uint64_t)b->ha * b->va;
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
