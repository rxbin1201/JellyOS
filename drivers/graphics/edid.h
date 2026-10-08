/*
 * Monitor timings and EDID, shared by the graphics drivers.
 *
 * A monitor describes itself in its EDID: blocks of 128 bytes. Getting the
 * blocks from the monitor is the driver's business (DDC or the DisplayPort
 * AUX channel, see dp_aux.h); what is in them is the same for every GPU.
 *
 * A monitor names its modes in four ways, and all four are read here:
 *
 *   detailed timings      every number of the timing; the modes the monitor
 *                         prefers, the first one above all
 *   CTA video codes       in a CTA-861 extension block (televisions, HDMI
 *                         monitors): a number that stands for a timing of
 *                         that standard, "16" for 1920x1080 at 60 Hz
 *   standard timings      width, aspect ratio and refresh rate; the timing
 *                         is the one VESA's list of monitor timings (DMT)
 *                         has for them
 *   established timings   one bit each for a handful of old modes, again
 *                         with the timing of VESA's list
 *
 * For the last three the numbers come from tables in edid.c. A mode a table
 * does not have is left out: no timing is made up by formula.
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
 * The monitor's modes (progressive, at least 640x400, square pixels) are added to `list`, which has `count` of
 * `max` entries: the detailed timings of the base block and of CTA extension blocks first, then CTA video
 * codes, standard and established timings. Of those three kinds a mode is left out if the list already has
 * its size at its refresh rate (within 1 Hz): the monitor's own numbers win. Returns the new count.
 */
uint32_t edid_collect_timings(const uint8_t *edid, int blocks, display_timing_t *list, uint32_t count, uint32_t max);

/* Refresh rate in hundredths of a hertz, and in frames per 1000 seconds. */
uint32_t display_timing_hz100(const display_timing_t *timing);
uint32_t display_timing_mhz(const display_timing_t *timing);
/* The same mode: size, totals and (within 1 %) the pixel clock agree. The same size at another rate is another mode. */
bool     display_timing_same(const display_timing_t *a, const display_timing_t *b);
/* Add a timing unless the list has it or is full. Returns the new count. */
uint32_t display_timing_add(display_timing_t *list, uint32_t count, uint32_t max, const display_timing_t *timing);
/* Does the list have this size at this refresh rate (within 1 Hz), with whatever timing? */
bool     display_timing_listed(const display_timing_t *list, uint32_t count, const display_timing_t *timing);
/*
 * Larger first, at the same size the faster one; but every mode of 48 Hz and more before the slower ones
 * (3840x2160 at 30 Hz is not what a desktop wants when 1920x1080 at 60 Hz is there).
 */
bool     display_timing_better(const display_timing_t *a, const display_timing_t *b);
void     display_timing_sort(display_timing_t *list, uint32_t count);

#endif
