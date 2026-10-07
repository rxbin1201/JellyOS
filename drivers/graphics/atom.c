/*
 * AtomBIOS byte code interpreter. See atom.h.
 *
 * An instruction is an opcode byte, mostly followed by an attribute byte
 * and operands. The opcode says what to do and what kind of destination it
 * has (register, parameter space, work space, ...); the attribute says what
 * kind of source, and which part of the 32-bit values takes part (all of
 * it, a word or a byte at some position).
 */

#include "drivers/graphics/atom.h"

/* Layout of the image */
#define BIOS_MAGIC         0xAA55
#define ATI_MAGIC_AT       0x30
#define ROM_TABLE_POINTER  0x48
#define ROM_MAGIC_AT       4      /* "ATOM" in the ROM table */
#define ROM_COMMANDS_AT    0x1E
#define ROM_DATA_AT        0x20
#define DATA_IIO_AT        0x32
#define TABLE_WS_AT        4      /* a command table: size, revisions, work space bytes, parameter bytes, code */
#define TABLE_PS_AT        5
#define TABLE_CODE_AT      6

#define OP_COUNT           127
#define OP_EOT             91
#define CASE_MAGIC         0x63
#define CASE_END           0x5A5A

enum { ARG_REG, ARG_PS, ARG_WS, ARG_FB, ARG_ID, ARG_IMM, ARG_PLL, ARG_MC };
enum { SRC_DWORD, SRC_WORD0, SRC_WORD8, SRC_WORD16, SRC_BYTE0, SRC_BYTE8, SRC_BYTE16, SRC_BYTE24 };
enum { COND_ABOVE, COND_ABOVE_OR_EQUAL, COND_ALWAYS, COND_BELOW, COND_BELOW_OR_EQUAL, COND_EQUAL, COND_NOT_EQUAL };

/* Work space entries with a meaning of their own */
#define WS_QUOTIENT   0x40
#define WS_REMAINDER  0x41
#define WS_DATAPTR    0x42
#define WS_SHIFT      0x43
#define WS_OR_MASK    0x44
#define WS_AND_MASK   0x45
#define WS_FB_WINDOW  0x46
#define WS_ATTRIBUTES 0x47
#define WS_REGPTR     0x48

/* Indirect register access: a small program per method */
enum { IIO_NOP, IIO_START, IIO_READ, IIO_WRITE, IIO_CLEAR, IIO_SET, IIO_MOVE_INDEX, IIO_MOVE_ATTR, IIO_MOVE_DATA, IIO_END };

#define IO_MM    0
#define IO_PCI   1
#define IO_SYSIO 2
#define IO_IIO   0x80

#define MAX_DEPTH 8
#define MAX_STEPS 4000000u /* a table that runs longer hangs */

typedef struct {
    atom_t   *atom;
    uint32_t *ps;        /* this table's parameters */
    uint32_t  ps_left;   /* dwords of parameter space from ps on */
    uint32_t  ps_shift;  /* where the parameters of a called table begin */
    uint32_t  ws[64];
    uint32_t  start;
    bool      abort;
} run_t;

typedef void (*op_t)(run_t *run, uint32_t *ptr, int arg);

static const uint32_t arg_mask[8] = { 0xFFFFFFFF, 0xFFFF, 0xFFFF00, 0xFFFF0000, 0xFF, 0xFF00, 0xFF0000, 0xFF000000 };
static const int arg_shift[8] = { 0, 0, 8, 16, 0, 8, 16, 24 };
/* From the destination's alignment field (by the source's alignment) to the same encoding as a source's. */
static const int dst_to_src[8][4] = {
    { 0, 0, 0, 0 }, { 1, 2, 3, 0 }, { 1, 2, 3, 0 }, { 1, 2, 3, 0 },
    { 4, 5, 6, 7 }, { 4, 5, 6, 7 }, { 4, 5, 6, 7 }, { 4, 5, 6, 7 },
};
static const int default_dst[8] = { 0, 0, 1, 2, 0, 1, 2, 3 };

static bool execute(atom_t *atom, uint32_t index, uint32_t *ps, uint32_t ps_left);

static void fail(run_t *run, const char *why, uint32_t at)
{
    if (!run->atom->error) {
        run->atom->error = why;
        run->atom->error_at = at;
    }
    run->abort = true;
}

/* --- Reading the image --------------------------------------------------------------- */

static uint32_t image8(const atom_t *atom, uint32_t at)
{
    return at < atom->size ? atom->bios[at] : 0;
}

static uint32_t image16(const atom_t *atom, uint32_t at)
{
    return image8(atom, at) | image8(atom, at + 1) << 8;
}

static uint32_t image32(const atom_t *atom, uint32_t at)
{
    return image16(atom, at) | image16(atom, at + 2) << 16;
}

static uint32_t code8(run_t *run, uint32_t *ptr)
{
    if (*ptr >= run->atom->size)
        fail(run, "the code runs past the end of the image", *ptr);
    return image8(run->atom, (*ptr)++);
}

static uint32_t code16(run_t *run, uint32_t *ptr)
{
    uint32_t low = code8(run, ptr);
    return low | code8(run, ptr) << 8;
}

static uint32_t code32(run_t *run, uint32_t *ptr)
{
    uint32_t low = code16(run, ptr);
    return low | code16(run, ptr) << 16;
}

/* --- Registers ------------------------------------------------------------------------ */

static uint32_t reg_read(atom_t *atom, uint32_t reg)
{
    atom->reads++;
    return atom->io.read(atom->io.context, reg);
}

static void reg_write(atom_t *atom, uint32_t reg, uint32_t value)
{
    atom->writes++;
    atom->io.write(atom->io.context, reg, value);
}

static uint32_t bits(uint32_t count)
{
    return count >= 32 ? 0xFFFFFFFFu : count ? 0xFFFFFFFFu >> (32 - count) : 0;
}

/* An indirect access method: a short program that builds an index/data access from `index` and `data`. */
static uint32_t iio_execute(atom_t *atom, uint32_t base, uint32_t index, uint32_t data)
{
    uint32_t value = 0xCDCDCDCD;

    for (int guard = 0; guard < 256; guard++) {
        uint32_t a = image8(atom, base + 1), b = image8(atom, base + 2), c = image8(atom, base + 3);
        switch (image8(atom, base)) {
        case IIO_NOP:
            base++;
            break;
        case IIO_READ:
            value = reg_read(atom, image16(atom, base + 1));
            base += 3;
            break;
        case IIO_WRITE:
            reg_write(atom, image16(atom, base + 1), value);
            base += 3;
            break;
        case IIO_CLEAR:
            value &= ~(bits(a) << (b & 31));
            base += 3;
            break;
        case IIO_SET:
            value |= bits(a) << (b & 31);
            base += 3;
            break;
        case IIO_MOVE_INDEX:
            value = (value & ~(bits(a) << (c & 31))) | ((index >> (b & 31)) & bits(a)) << (c & 31);
            base += 4;
            break;
        case IIO_MOVE_DATA:
            value = (value & ~(bits(a) << (c & 31))) | ((data >> (b & 31)) & bits(a)) << (c & 31);
            base += 4;
            break;
        case IIO_MOVE_ATTR:
            value = (value & ~(bits(a) << (c & 31))) | (((uint32_t)atom->io_attr >> (b & 31)) & bits(a)) << (c & 31);
            base += 4;
            break;
        case IIO_END:
            return value;
        default:
            return 0;
        }
    }
    return 0;
}

/* --- Operands ------------------------------------------------------------------------- */

static uint32_t *ps_slot(run_t *run, uint32_t index, uint32_t at)
{
    static uint32_t nowhere;

    if (index >= run->ps_left) {
        fail(run, "a table reaches beyond its parameter space", at);
        nowhere = 0;
        return &nowhere;
    }
    return &run->ps[index];
}

/* The value of a source operand. *saved gets the whole 32 bits before the part is cut out. */
static uint32_t get_src(run_t *run, uint8_t attr, uint32_t *ptr, uint32_t *saved)
{
    atom_t *atom = run->atom;
    uint32_t arg = attr & 7, align = (attr >> 3) & 7, index, value = 0xCDCDCDCD;

    switch (arg) {
    case ARG_REG:
        index = code16(run, ptr) + atom->reg_block;
        if (atom->io_mode == IO_MM)
            value = reg_read(atom, index);
        else if ((atom->io_mode & IO_IIO) && atom->iio[atom->io_mode & 0x7F])
            value = iio_execute(atom, atom->iio[atom->io_mode & 0x7F], index, 0);
        else
            fail(run, "a kind of register access that is not there", *ptr);
        break;
    case ARG_PS:
        value = *ps_slot(run, code8(run, ptr), *ptr);
        break;
    case ARG_WS:
        index = code8(run, ptr);
        switch (index) {
        case WS_QUOTIENT:   value = atom->divmul[0]; break;
        case WS_REMAINDER:  value = atom->divmul[1]; break;
        case WS_DATAPTR:    value = atom->data_block; break;
        case WS_SHIFT:      value = atom->shift; break;
        case WS_OR_MASK:    value = 1u << (atom->shift & 31); break;
        case WS_AND_MASK:   value = ~(1u << (atom->shift & 31)); break;
        case WS_FB_WINDOW:  value = atom->fb_base; break;
        case WS_ATTRIBUTES: value = atom->io_attr; break;
        case WS_REGPTR:     value = atom->reg_block; break;
        default:            value = index < 64 ? run->ws[index] : 0; break;
        }
        break;
    case ARG_ID:
        value = image32(atom, code16(run, ptr) + atom->data_block);
        break;
    case ARG_FB:
        index = code8(run, ptr);
        value = atom->fb_base + index * 4 + 4 <= atom->scratch_bytes ? atom->scratch[atom->fb_base / 4 + index] : 0;
        break;
    case ARG_IMM:
        /* An immediate has just the size of the part it is for. */
        if (align == SRC_DWORD)
            return code32(run, ptr);
        if (align <= SRC_WORD16)
            return code16(run, ptr);
        return code8(run, ptr);
    case ARG_PLL:
    case ARG_MC:
        code8(run, ptr); /* clock generator and memory controller registers of old chips: not there */
        value = 0;
        break;
    }
    if (saved)
        *saved = value;
    return (value & arg_mask[align]) >> arg_shift[align];
}

static void skip_src(run_t *run, uint8_t attr, uint32_t *ptr)
{
    uint32_t arg = attr & 7, align = (attr >> 3) & 7;

    if (arg == ARG_REG || arg == ARG_ID)
        *ptr += 2;
    else if (arg == ARG_IMM)
        *ptr += align == SRC_DWORD ? 4 : align <= SRC_WORD16 ? 2 : 1;
    else
        *ptr += 1;
    (void)run;
}

/* An immediate of the size alignment `align` implies. */
static uint32_t get_direct(run_t *run, uint8_t align, uint32_t *ptr)
{
    if (align == SRC_DWORD)
        return code32(run, ptr);
    if (align <= SRC_WORD16)
        return code16(run, ptr);
    return code8(run, ptr);
}

static uint8_t dst_attr(int arg, uint8_t attr)
{
    return (uint8_t)(arg | dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3] << 3);
}

static uint32_t get_dst(run_t *run, int arg, uint8_t attr, uint32_t *ptr, uint32_t *saved)
{
    return get_src(run, dst_attr(arg, attr), ptr, saved);
}

/* Store `value` into the part of the destination the attribute names; `saved` is the rest of its 32 bits. */
static void put_dst(run_t *run, int arg, uint8_t attr, uint32_t *ptr, uint32_t value, uint32_t saved)
{
    atom_t *atom = run->atom;
    uint32_t align = (uint32_t)dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3], index;

    value = ((value << arg_shift[align]) & arg_mask[align]) | (saved & ~arg_mask[align]);
    switch (arg) {
    case ARG_REG:
        index = code16(run, ptr) + atom->reg_block;
        if (atom->io_mode == IO_MM)
            reg_write(atom, index, index == 0 ? value << 2 : value); /* register 0 is an index register in bytes */
        else if ((atom->io_mode & IO_IIO) && atom->iio[atom->io_mode & 0xFF])
            iio_execute(atom, atom->iio[atom->io_mode & 0xFF], index, value); /* write methods: number | 0x80 */
        else
            fail(run, "a kind of register access that is not there", *ptr);
        break;
    case ARG_PS:
        *ps_slot(run, code8(run, ptr), *ptr) = value;
        break;
    case ARG_WS:
        index = code8(run, ptr);
        switch (index) {
        case WS_QUOTIENT:   atom->divmul[0] = value; break;
        case WS_REMAINDER:  atom->divmul[1] = value; break;
        case WS_DATAPTR:    atom->data_block = (uint16_t)value; break;
        case WS_SHIFT:      atom->shift = (uint8_t)value; break;
        case WS_OR_MASK:
        case WS_AND_MASK:   break;
        case WS_FB_WINDOW:  atom->fb_base = value; break;
        case WS_ATTRIBUTES: atom->io_attr = (uint16_t)value; break;
        case WS_REGPTR:     atom->reg_block = (uint16_t)value; break;
        default:
            if (index < 64)
                run->ws[index] = value;
            break;
        }
        break;
    case ARG_FB:
        index = code8(run, ptr);
        if (atom->fb_base + index * 4 + 4 <= atom->scratch_bytes)
            atom->scratch[atom->fb_base / 4 + index] = value;
        break;
    case ARG_PLL:
    case ARG_MC:
        code8(run, ptr);
        break;
    }
}

/* --- Instructions --------------------------------------------------------------------- */

/* dst = dst <operation> src, for the operations that all look alike */
static void op_binary(run_t *run, uint32_t *ptr, int arg, int operation)
{
    uint8_t attr = (uint8_t)code8(run, ptr);
    uint32_t dptr = *ptr, saved, dst = get_dst(run, arg, attr, ptr, &saved), src = get_src(run, attr, ptr, NULL);

    switch (operation) {
    case '+': dst += src; break;
    case '-': dst -= src; break;
    case '&': dst &= src; break;
    case '|': dst |= src; break;
    case '^': dst ^= src; break;
    }
    put_dst(run, arg, attr, &dptr, dst, saved);
}

static void op_add(run_t *run, uint32_t *ptr, int arg) { op_binary(run, ptr, arg, '+'); }
static void op_sub(run_t *run, uint32_t *ptr, int arg) { op_binary(run, ptr, arg, '-'); }
static void op_and(run_t *run, uint32_t *ptr, int arg) { op_binary(run, ptr, arg, '&'); }
static void op_or(run_t *run, uint32_t *ptr, int arg)  { op_binary(run, ptr, arg, '|'); }
static void op_xor(run_t *run, uint32_t *ptr, int arg) { op_binary(run, ptr, arg, '^'); }

static void op_move(run_t *run, uint32_t *ptr, int arg)
{
    uint8_t attr = (uint8_t)code8(run, ptr);
    uint32_t dptr = *ptr, saved = 0xCDCDCDCD;

    /* Only a partial move needs the old value (and with a register, reading it may have effects). */
    if (((attr >> 3) & 7) != SRC_DWORD)
        get_dst(run, arg, attr, ptr, &saved);
    else
        skip_src(run, dst_attr(arg, attr), ptr);
    put_dst(run, arg, attr, &dptr, get_src(run, attr, ptr, NULL), saved);
}

static void op_clear(run_t *run, uint32_t *ptr, int arg)
{
    uint8_t attr = (uint8_t)(code8(run, ptr) & 0x38);
    uint32_t dptr = *ptr, saved;

    attr |= (uint8_t)(default_dst[attr >> 3] << 6);
    get_dst(run, arg, attr, ptr, &saved);
    put_dst(run, arg, attr, &dptr, 0, saved);
}

/* dst = (dst & mask) | src, with the mask an immediate */
static void op_mask(run_t *run, uint32_t *ptr, int arg)
{
    uint8_t attr = (uint8_t)code8(run, ptr);
    uint32_t dptr = *ptr, saved, dst = get_dst(run, arg, attr, ptr, &saved);
    uint32_t mask = get_direct(run, (attr >> 3) & 7, ptr), src = get_src(run, attr, ptr, NULL);

    put_dst(run, arg, attr, &dptr, (dst & mask) | src, saved);
}

static void op_compare(run_t *run, uint32_t *ptr, int arg)
{
    uint8_t attr = (uint8_t)code8(run, ptr);
    uint32_t dst = get_dst(run, arg, attr, ptr, NULL), src = get_src(run, attr, ptr, NULL);

    run->atom->equal = dst == src;
    run->atom->above = dst > src;
}

static void op_test(run_t *run, uint32_t *ptr, int arg)
{
    uint8_t attr = (uint8_t)code8(run, ptr);
    uint32_t dst = get_dst(run, arg, attr, ptr, NULL), src = get_src(run, attr, ptr, NULL);

    run->atom->equal = (dst & src) == 0;
}

static void op_mul(run_t *run, uint32_t *ptr, int arg)
{
    uint8_t attr = (uint8_t)code8(run, ptr);
    uint32_t dst = get_dst(run, arg, attr, ptr, NULL), src = get_src(run, attr, ptr, NULL);

    run->atom->divmul[0] = dst * src;
}

static void op_mul32(run_t *run, uint32_t *ptr, int arg)
{
    uint8_t attr = (uint8_t)code8(run, ptr);
    uint64_t product = (uint64_t)get_dst(run, arg, attr, ptr, NULL);

    product *= get_src(run, attr, ptr, NULL);
    run->atom->divmul[0] = (uint32_t)product;
    run->atom->divmul[1] = (uint32_t)(product >> 32);
}

static void op_div(run_t *run, uint32_t *ptr, int arg)
{
    uint8_t attr = (uint8_t)code8(run, ptr);
    uint32_t dst = get_dst(run, arg, attr, ptr, NULL), src = get_src(run, attr, ptr, NULL);

    run->atom->divmul[0] = src ? dst / src : 0;
    run->atom->divmul[1] = src ? dst % src : 0;
}

/* 64 by 32 bits: the high half of the dividend is the remainder register, the quotient goes to both. */
static void op_div32(run_t *run, uint32_t *ptr, int arg)
{
    uint8_t attr = (uint8_t)code8(run, ptr);
    uint64_t dividend = get_dst(run, arg, attr, ptr, NULL);
    uint32_t src = get_src(run, attr, ptr, NULL);

    dividend |= (uint64_t)run->atom->divmul[1] << 32;
    uint64_t quotient = src ? dividend / src : 0;
    run->atom->divmul[0] = (uint32_t)quotient;
    run->atom->divmul[1] = (uint32_t)(quotient >> 32);
}

/* Shift by an immediate count; the destination's part is the default one for the source alignment. */
static void op_shift(run_t *run, uint32_t *ptr, int arg, bool left)
{
    uint8_t attr = (uint8_t)(code8(run, ptr) & 0x38);
    uint32_t dptr = *ptr, saved;

    attr |= (uint8_t)(default_dst[attr >> 3] << 6);
    uint32_t dst = get_dst(run, arg, attr, ptr, &saved), count = get_direct(run, SRC_BYTE0, ptr);
    dst = count >= 32 ? 0 : left ? dst << count : dst >> count;
    put_dst(run, arg, attr, &dptr, dst, saved);
}

static void op_shift_left(run_t *run, uint32_t *ptr, int arg)  { op_shift(run, ptr, arg, true); }
static void op_shift_right(run_t *run, uint32_t *ptr, int arg) { op_shift(run, ptr, arg, false); }

/* Shift the whole 32 bits by an operand and keep the destination's part of the result. */
static void op_shx(run_t *run, uint32_t *ptr, int arg, bool left)
{
    uint8_t attr = (uint8_t)code8(run, ptr);
    uint32_t dptr = *ptr, saved, align = (uint32_t)dst_to_src[(attr >> 3) & 7][(attr >> 6) & 3];

    get_dst(run, arg, attr, ptr, &saved);
    uint32_t count = get_src(run, attr, ptr, NULL);
    uint32_t dst = count >= 32 ? 0 : left ? saved << count : saved >> count;
    put_dst(run, arg, attr, &dptr, (dst & arg_mask[align]) >> arg_shift[align], saved);
}

static void op_shl(run_t *run, uint32_t *ptr, int arg) { op_shx(run, ptr, arg, true); }
static void op_shr(run_t *run, uint32_t *ptr, int arg) { op_shx(run, ptr, arg, false); }

static void op_jump(run_t *run, uint32_t *ptr, int arg)
{
    atom_t *atom = run->atom;
    uint32_t target = code16(run, ptr);
    bool taken = false;

    switch (arg) {
    case COND_ABOVE:          taken = atom->above; break;
    case COND_ABOVE_OR_EQUAL: taken = atom->above || atom->equal; break;
    case COND_ALWAYS:         taken = true; break;
    case COND_BELOW:          taken = !(atom->above || atom->equal); break;
    case COND_BELOW_OR_EQUAL: taken = !atom->above; break;
    case COND_EQUAL:          taken = atom->equal; break;
    case COND_NOT_EQUAL:      taken = !atom->equal; break;
    }
    if (taken)
        *ptr = run->start + target;
}

/* SWITCH src; then pairs of (0x63, value, target) until 0x5A5A */
static void op_switch(run_t *run, uint32_t *ptr, int arg)
{
    uint8_t attr = (uint8_t)code8(run, ptr);
    uint32_t src = get_src(run, attr, ptr, NULL);

    (void)arg;
    while (image16(run->atom, *ptr) != CASE_END && !run->abort) {
        if (image8(run->atom, *ptr) != CASE_MAGIC) {
            fail(run, "a damaged switch", *ptr);
            return;
        }
        (*ptr)++;
        uint32_t value = get_src(run, (uint8_t)((attr & 0x38) | ARG_IMM), ptr, NULL), target = code16(run, ptr);
        if (value == src) {
            *ptr = run->start + target;
            return;
        }
    }
    *ptr += 2;
}

static void op_calltable(run_t *run, uint32_t *ptr, int arg)
{
    uint32_t index = code8(run, ptr);

    (void)arg;
    if (!image16(run->atom, run->atom->command_table + 4 + 2 * index))
        return; /* a table this BIOS does not have: nothing to do */
    if (run->ps_shift > run->ps_left) {
        fail(run, "a table reaches beyond its parameter space", *ptr);
        return;
    }
    if (!execute(run->atom, index, run->ps + run->ps_shift, run->ps_left - run->ps_shift))
        run->abort = true;
}

static void op_delay(run_t *run, uint32_t *ptr, int arg)
{
    uint32_t count = code8(run, ptr);

    if (run->atom->io.delay_us)
        run->atom->io.delay_us(run->atom->io.context, arg ? count * 1000 : count);
}

static void op_setport(run_t *run, uint32_t *ptr, int arg)
{
    if (arg == 0) {
        uint32_t port = code16(run, ptr);
        run->atom->io_mode = port ? (int)(IO_IIO | port) : IO_MM;
    } else {
        code8(run, ptr);
        run->atom->io_mode = arg == 1 ? IO_PCI : IO_SYSIO;
    }
}

static void op_setregblock(run_t *run, uint32_t *ptr, int arg)
{
    (void)arg;
    run->atom->reg_block = (uint16_t)code16(run, ptr);
}

static void op_setfbbase(run_t *run, uint32_t *ptr, int arg)
{
    uint8_t attr = (uint8_t)code8(run, ptr);

    (void)arg;
    run->atom->fb_base = get_src(run, attr, ptr, NULL);
}

/* Which data table ID operands are relative to: none, this command table itself, or one of the master list. */
static void op_setdatablock(run_t *run, uint32_t *ptr, int arg)
{
    uint32_t index = code8(run, ptr);

    (void)arg;
    if (index == 0)
        run->atom->data_block = 0;
    else if (index == 255)
        run->atom->data_block = (uint16_t)run->start;
    else
        run->atom->data_block = (uint16_t)image16(run->atom, run->atom->data_table + 4 + 2 * index);
}

static void op_skip_byte(run_t *run, uint32_t *ptr, int arg)
{
    (void)arg;
    code8(run, ptr); /* POST code or debug marker */
}

/* A block of data inside the code: step over it. */
static void op_processds(run_t *run, uint32_t *ptr, int arg)
{
    (void)arg;
    *ptr += code16(run, ptr);
}

static void op_nothing(run_t *run, uint32_t *ptr, int arg)
{
    (void)run;
    (void)ptr;
    (void)arg;
}

static void op_unsupported(run_t *run, uint32_t *ptr, int arg)
{
    (void)arg;
    fail(run, "an instruction that is not implemented (REPEAT, SAVEREG, RESTOREREG)", *ptr);
}

#define SIX(function) \
    { function, ARG_REG }, { function, ARG_PS }, { function, ARG_WS }, { function, ARG_FB }, { function, ARG_PLL }, \
    { function, ARG_MC }

static const struct {
    op_t function;
    int  arg;
} opcodes[OP_COUNT] = {
    { NULL, 0 },
    SIX(op_move),        /* 1 */
    SIX(op_and),         /* 7 */
    SIX(op_or),          /* 13 */
    SIX(op_shift_left),  /* 19 */
    SIX(op_shift_right), /* 25 */
    SIX(op_mul),         /* 31 */
    SIX(op_div),         /* 37 */
    SIX(op_add),         /* 43 */
    SIX(op_sub),         /* 49 */
    { op_setport, 0 }, { op_setport, 1 }, { op_setport, 2 }, /* 55 */
    { op_setregblock, 0 },                                   /* 58 */
    { op_setfbbase, 0 },                                     /* 59 */
    SIX(op_compare),                                         /* 60 */
    { op_switch, 0 },                                        /* 66 */
    { op_jump, COND_ALWAYS }, { op_jump, COND_EQUAL }, { op_jump, COND_BELOW }, { op_jump, COND_ABOVE }, /* 67 */
    { op_jump, COND_BELOW_OR_EQUAL }, { op_jump, COND_ABOVE_OR_EQUAL }, { op_jump, COND_NOT_EQUAL },
    SIX(op_test),                                            /* 74 */
    { op_delay, 1 }, { op_delay, 0 },                        /* 80: milliseconds, microseconds */
    { op_calltable, 0 },                                     /* 82 */
    { op_unsupported, 0 },                                   /* 83: REPEAT */
    SIX(op_clear),                                           /* 84 */
    { op_nothing, 0 },                                       /* 90: NOP */
    { op_nothing, 0 },                                       /* 91: EOT */
    SIX(op_mask),                                            /* 92 */
    { op_skip_byte, 0 },                                     /* 98: POSTCARD */
    { op_nothing, 0 },                                       /* 99: BEEP */
    { op_unsupported, 0 }, { op_unsupported, 0 },            /* 100: SAVEREG, RESTOREREG */
    { op_setdatablock, 0 },                                  /* 102 */
    SIX(op_xor),                                             /* 103 */
    SIX(op_shl),                                             /* 109 */
    SIX(op_shr),                                             /* 115 */
    { op_skip_byte, 0 },                                     /* 121: DEBUG */
    { op_processds, 0 },                                     /* 122 */
    { op_mul32, ARG_PS }, { op_mul32, ARG_WS },              /* 123 */
    { op_div32, ARG_PS }, { op_div32, ARG_WS },              /* 125 */
};

/* --- Running a table -------------------------------------------------------------------- */

static bool execute(atom_t *atom, uint32_t index, uint32_t *ps, uint32_t ps_left)
{
    uint32_t base = image16(atom, atom->command_table + 4 + 2 * index);
    run_t run = { .atom = atom, .ps = ps, .ps_left = ps_left, .start = base };

    if (!base || base + TABLE_CODE_AT >= atom->size) {
        fail(&run, "no such command table", base);
        return false;
    }
    if (++atom->depth > MAX_DEPTH) {
        atom->depth--;
        fail(&run, "tables call each other too deeply", base);
        return false;
    }
    atom->calls++;
    run.ps_shift = (image8(atom, base + TABLE_PS_AT) & 0x7F) / 4;
    for (uint32_t ptr = base + TABLE_CODE_AT; !run.abort;) {
        uint32_t at = ptr, op = code8(&run, &ptr);
        if (op == 0 || op >= OP_COUNT) {
            fail(&run, "an unknown instruction", at);
            break;
        }
        if (++atom->steps > MAX_STEPS) {
            fail(&run, "a table does not come to an end", at);
            break;
        }
        opcodes[op].function(&run, &ptr, opcodes[op].arg);
        if (op == OP_EOT)
            break;
    }
    atom->depth--;
    return !run.abort;
}

bool atom_execute(atom_t *atom, uint32_t index, uint32_t *parameters)
{
    atom->data_block = 0;
    atom->reg_block = 0;
    atom->fb_base = 0;
    atom->io_mode = IO_MM;
    atom->divmul[0] = atom->divmul[1] = 0;
    atom->depth = 0;
    atom->steps = 0;
    atom->reads = atom->writes = atom->calls = 0;
    atom->error = NULL;
    atom->error_at = 0;
    return execute(atom, index, parameters, ATOM_PARAMETERS);
}

bool atom_table_revision(const atom_t *atom, uint32_t index, uint8_t *format, uint8_t *content)
{
    uint32_t base = image16(atom, atom->command_table + 4 + 2 * index);

    if (!base || base + TABLE_CODE_AT >= atom->size)
        return false;
    if (format)
        *format = (uint8_t)image8(atom, base + 2);
    if (content)
        *content = (uint8_t)image8(atom, base + 3);
    return true;
}

static bool text_at(const atom_t *atom, uint32_t at, const char *text)
{
    for (uint32_t i = 0; text[i]; i++) {
        if (image8(atom, at + i) != (uint8_t)text[i])
            return false;
    }
    return true;
}

bool atom_init(atom_t *atom, const uint8_t *bios, uint32_t size, const atom_io_t *io, uint32_t *scratch,
               uint32_t scratch_bytes)
{
    static const uint8_t iio_length[] = { 1, 2, 3, 3, 3, 3, 4, 4, 4, 3 };

    *atom = (atom_t){ .bios = bios, .size = size, .io = *io, .scratch = scratch, .scratch_bytes = scratch_bytes };
    if (size < 0x100 || image16(atom, 0) != BIOS_MAGIC || !text_at(atom, ATI_MAGIC_AT, " 761295520"))
        return false;
    uint32_t rom = image16(atom, ROM_TABLE_POINTER);
    if (!text_at(atom, rom + ROM_MAGIC_AT, "ATOM"))
        return false;
    atom->command_table = image16(atom, rom + ROM_COMMANDS_AT);
    atom->data_table = image16(atom, rom + ROM_DATA_AT);
    if (!atom->command_table || !atom->data_table || atom->command_table >= size || atom->data_table >= size)
        return false;

    /* The indirect access methods: (START, number, program..., END, two bytes), one after the other. */
    uint32_t iio = image16(atom, atom->data_table + DATA_IIO_AT);
    for (uint32_t base = iio ? iio + 4 : 0; base && image8(atom, base) == IIO_START;) {
        atom->iio[image8(atom, base + 1)] = (uint16_t)(base + 2);
        base += 2;
        while (image8(atom, base) != IIO_END && image8(atom, base) < sizeof(iio_length) && base < size)
            base += iio_length[image8(atom, base)];
        base += 3;
    }
    return true;
}
