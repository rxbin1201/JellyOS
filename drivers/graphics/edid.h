/*
 * Monitor timings and EDID, shared by the graphics drivers.
 *
 * A monitor describes itself in its EDID: blocks of 128 bytes with, among
 * other things, "detailed timing descriptors" for the modes it prefers.
 * Getting the blocks from the monitor is the driver's business (DDC or the
 * DisplayPort AUX channel, see dp_aux.h); what is in them is the same for
 * every GPU.
 */

#ifndef DRIVERS_GRAPHICS_EDID_H
#define DRIVERS_GRAPHICS_EDID_H

#include <stdbool.h>
#include <stdint.h>

#define EDID_BLOCK 128

/* One video timing: what a monitor is driven with. */
typedef struct {
    uint32_t khz;                    /* pixel clock */
    uint32_t ha, hso, hsw, ht;       /* active, sync offset, sync width, total */
    uint32_t va, vso, vsw, vt;
    bool     hpos, vpos, interlaced; /* sync polarity positive */
} display_timing_t;

/* The checksum of a block is right. */
bool     edid_block_ok(const uint8_t *block);
/* The first block starts as an EDID does. */
bool     edid_header_ok(const uint8_t *edid);

/* A detailed timing descriptor (18 bytes); false for the other kinds of descriptors. */
bool     edid_timing_parse(const uint8_t *descriptor, display_timing_t *timing);
/*
 * The detailed timings of the base block and of CTA extension blocks (progressive, at least 640x400) are added
 * to `list`, which has `count` of `max` entries; duplicates are left out. Returns the new count.
 */
uint32_t edid_collect_timings(const uint8_t *edid, int blocks, display_timing_t *list, uint32_t count, uint32_t max);

/* Refresh rate in hundredths of a hertz, and in frames per 1000 seconds. */
uint32_t display_timing_hz100(const display_timing_t *timing);
uint32_t display_timing_mhz(const display_timing_t *timing);
/* The same mode: size, totals and (within 1 %) the pixel clock agree. The same size at another rate is another mode. */
bool     display_timing_same(const display_timing_t *a, const display_timing_t *b);
/* Add a timing unless the list has it or is full. Returns the new count. */
uint32_t display_timing_add(display_timing_t *list, uint32_t count, uint32_t max, const display_timing_t *timing);
/* Larger first, at the same size the faster one. */
bool     display_timing_better(const display_timing_t *a, const display_timing_t *b);
void     display_timing_sort(display_timing_t *list, uint32_t count);

#endif
