/*
 * AtomBIOS: the little programs in the video BIOS of AMD graphics.
 *
 * Much of what is specific to a board (which PHY drives which connector,
 * how its PLL and signal levels are set) is not described to the driver
 * but done for it: the video BIOS holds "command tables", programs in a
 * byte code that read and write the GPU's registers. A driver runs a table
 * with a block of parameters; this file is the interpreter.
 *
 * It knows nothing about the hardware: registers are reached through the
 * functions the driver gives it, which is also what lets the interpreter
 * be tested on the development machine (tests/unit/atom_test.c).
 *
 * After the interpreter of Linux's radeon and amdgpu drivers (atom.c by
 * Stanislaw Skowronek, MIT licence), which defines the byte code in
 * practice.
 */

#ifndef DRIVERS_GRAPHICS_ATOM_H
#define DRIVERS_GRAPHICS_ATOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ATOM_PARAMETERS 32 /* dwords of parameter space a caller provides (tables pass part of it on to others) */

typedef struct {
    void    *context;
    /* Registers are numbered in dwords: register r is at byte 4 * r of the register BAR. */
    uint32_t (*read)(void *context, uint32_t reg);
    void     (*write)(void *context, uint32_t reg, uint32_t value);
    void     (*delay_us)(void *context, uint32_t microseconds);
} atom_io_t;

typedef struct {
    const uint8_t *bios;
    uint32_t       size;
    atom_io_t      io;
    uint32_t       command_table, data_table; /* offsets of the two master tables */
    uint16_t       iio[256];                  /* indirect register access: where each method's program starts */
    uint32_t      *scratch;                   /* the tables' own memory ("frame buffer scratch") */
    uint32_t       scratch_bytes;

    /* State of a run */
    uint32_t       divmul[2];
    uint16_t       data_block, reg_block, io_attr;
    uint32_t       fb_base;
    uint8_t        shift;
    bool           equal, above;
    int            io_mode;
    int            depth;
    uint32_t       steps;
    const char    *error;                     /* why the last run stopped early, NULL if it did not */
    uint32_t       error_at;                  /* offset in the BIOS */

    /* Statistics of the last atom_execute() */
    uint32_t       reads, writes, calls;
} atom_t;

/* Check the image and find its tables. scratch: zeroed memory of scratch_bytes for the tables' own use. */
bool atom_init(atom_t *atom, const uint8_t *bios, uint32_t size, const atom_io_t *io, uint32_t *scratch,
               uint32_t scratch_bytes);

/* Does command table `index` exist? Its format and content revision say which parameters it takes. */
bool atom_table_revision(const atom_t *atom, uint32_t index, uint8_t *format, uint8_t *content);

/* Run command table `index`. parameters: ATOM_PARAMETERS dwords, read and written by the table. */
bool atom_execute(atom_t *atom, uint32_t index, uint32_t *parameters);

#endif
