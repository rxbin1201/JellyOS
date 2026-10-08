/*
 * Programs for the execution units of Intel graphics, generation 9: copying
 * and blending blocks of pixels, for putting a desktop together on the GPU.
 *
 * The render engine starts one hardware thread per block (GPGPU_WALKER).
 * A thread finds its block's number in register r0 (across in dword 1,
 * down in dword 6) and the piece's corners in r1 (the constants), reads its
 * block of the source and, for blending, of the destination with "media
 * block read" messages to the data port, computes, and writes the result
 * with "media block write". The surfaces are seen as 8-bit surfaces, so x
 * counts bytes: a pixel is four of them.
 *
 * The programs are made by a small assembler: the instruction format of
 * generation 8 and 9 (128 bits per instruction), "align1" only, registers
 * addressed directly. It makes the fill program of Intel's test suite IGT
 * bit for bit, which the unit test checks.
 *
 * Ported from the previous JellyOS (minikernel, Kernel/drivers/gpu/
 * igd_rcs.c), where these programs composed the desktop on a Core i5-8400T
 * with UHD Graphics 630. Instruction encoding after Intel's Programmer's
 * Reference Manual for Skylake and Linux's IGT.
 */

#include "drivers/graphics/intel_kernels.h"

/* --- The assembler --------------------------------------------------------------------------- */

enum { FILE_ARF = 0, FILE_GRF = 1, FILE_IMM = 3 };                                     /* register file */
enum { T_UD = 0, T_D = 1, T_UW = 2, T_W = 3, T_UB = 4, T_B = 5, T_F = 7 };            /* data type */
enum { OP_MOV = 0x01, OP_OR = 0x06, OP_XOR = 0x07, OP_SHR = 0x08, OP_SHL = 0x09, OP_ADD = 0x40, OP_MUL = 0x41,
       OP_SEND = 0x31 };
enum { SFID_SPAWNER = 0x7, SFID_DATA_PORT1 = 0xC };                                  /* where a send goes */

/* An operand: file, type, register and byte in it, region <vs;w,hs> as encoded (0 = 0 or 1, 1 = 1, 2 = 2, 3 = 4, ...) */
typedef struct {
    int      file, type, reg, sub;
    int      vs, w, hs;
    uint32_t imm;
} operand_t;

static operand_t reg(int type, int r, int sub_bytes, int vs, int w, int hs)
{
    return (operand_t){ FILE_GRF, type, r, sub_bytes, vs, w, hs, 0 };
}

/* A destination: stride 1 */
static operand_t dst(int type, int r, int sub_bytes)
{
    return (operand_t){ FILE_GRF, type, r, sub_bytes, 0, 0, 1, 0 };
}

static operand_t imm(int type, uint32_t value)
{
    if (type == T_UW || type == T_W) /* word immediates are in both halves */
        value = (value & 0xFFFF) | (value & 0xFFFF) << 16;
    return (operand_t){ FILE_IMM, type, 0, 0, 0, 0, 0, value };
}

static operand_t null_reg(void)
{
    return (operand_t){ FILE_ARF, T_UW, 0, 0, 0, 0, 1, 0 };
}

static operand_t none(void)
{
    return (operand_t){ 0, 0, 0, 0, 0, 0, 0, 0 };
}

#define SCALAR 0, 0, 0 /* <0;1,0> */

/*
 * One instruction: opcode, execution size (log2: 0 is one channel ... 4 is sixteen), bits 27:24 of the first dword
 * (the shared function of a send), destination, source 0, source 1 (an immediate or a send's message descriptor is
 * the last dword).
 */
static void inst(uint32_t *o, int op, int exec, int control, operand_t d, operand_t s0, operand_t s1)
{
    o[0] = (uint32_t)op | (uint32_t)exec << 21 | (uint32_t)control << 24;
    o[1] = (uint32_t)d.file << 3 | (uint32_t)d.type << 5 | (uint32_t)s0.file << 9 | (uint32_t)s0.type << 11 |
           (uint32_t)d.sub << 16 | (uint32_t)d.reg << 21 | (uint32_t)d.hs << 29;
    if (s0.file == FILE_IMM) { /* source 0 immediate: in the last dword */
        o[2] = 0;
        o[3] = s0.imm;
        return;
    }
    o[2] = (uint32_t)s0.sub | (uint32_t)s0.reg << 5 | (uint32_t)s0.hs << 16 | (uint32_t)s0.w << 18 |
           (uint32_t)s0.vs << 21 | (uint32_t)s1.file << 25 | (uint32_t)s1.type << 27;
    if (s1.file == FILE_GRF) /* source 1 a register: the same fields as source 0, in the last dword */
        o[3] = (uint32_t)s1.sub | (uint32_t)s1.reg << 5 | (uint32_t)s1.hs << 16 | (uint32_t)s1.w << 18 | (uint32_t)s1.vs << 21;
    else
        o[3] = s1.imm;
}

/* send: the message from register `from` on to a shared function, with descriptor desc */
static void send(uint32_t *o, int exec, int sfid, operand_t d, int from, uint32_t desc)
{
    operand_t s0 = { FILE_GRF, T_D, from, 0, 0, 0, 0, 0 };
    inst(o, OP_SEND, exec, sfid, d, s0, imm(T_D, desc));
}

/* "Media block write" to the data port: message length in registers, with a header, binding table entry */
static uint32_t block_write(int length, int surface)
{
    return (uint32_t)length << 25 | 1u << 19 | 0xAu << 14 | (uint32_t)surface;
}

/* "Media block read": a header of one register, the answer `length` registers */
static uint32_t block_read(int length, int surface)
{
    return 1u << 25 | (uint32_t)length << 20 | 1u << 19 | 0x4u << 14 | (uint32_t)surface;
}

#define END_OF_THREAD 0x82000010u /* to the thread spawner: one register, end of thread */

/* --- The programs ---------------------------------------------------------------------------- */

uint32_t intel_kernel_igt_fill(uint32_t (*k)[4])
{
    uint32_t n = 0;

    inst(k[n++], OP_MOV, 2, 0, (operand_t){ FILE_GRF, T_UB, 1, 0, 0, 0, 1, 0 }, reg(T_UB, 1, 0, SCALAR), none());
    inst(k[n++], OP_MUL, 0, 0, dst(T_UD, 2, 0), (operand_t){ FILE_GRF, T_UD, 0, 4, 0, 0, 0, 0 }, imm(T_UD, 16));
    inst(k[n++], OP_MOV, 0, 0, dst(T_UD, 2, 4), (operand_t){ FILE_GRF, T_UD, 0, 24, 0, 0, 0, 0 }, none());
    inst(k[n++], OP_MOV, 3, 0, dst(T_UD, 4, 0), reg(T_UD, 0, 0, 4, 3, 1), none());
    inst(k[n++], OP_MOV, 1, 0, dst(T_UD, 4, 0), reg(T_UD, 2, 0, 2, 1, 1), none());
    inst(k[n++], OP_MOV, 0, 0, dst(T_UD, 4, 8), imm(T_UD, 0xF), none());
    inst(k[n++], OP_MOV, 4, 0, dst(T_UD, 5, 0), reg(T_UD, 1, 0, SCALAR), none());
    send(k[n++], 4, SFID_DATA_PORT1, (operand_t){ FILE_ARF, T_UW, 32, 0, 0, 0, 1, 0 }, 4, block_write(3, 0));
    inst(k[n++], OP_MOV, 3, 0, dst(T_UD, 112, 0), reg(T_UD, 0, 0, 4, 3, 1), none());
    send(k[n++], 4, SFID_SPAWNER, null_reg(), 112, END_OF_THREAD);
    return n;
}

static const uint8_t shapes[INTEL_SHAPES][2] = { { 32, 8 }, { 4, 8 }, { 32, 1 }, { 4, 1 } }; /* bytes x rows */

void intel_shape_size(uint32_t shape, uint32_t *width, uint32_t *height)
{
    *width = shapes[shape & 3][0] / 4;
    *height = shapes[shape & 3][1];
}

/* Registers a block fills (small blocks are packed: always whole pixels). */
static int block_registers(int bw, int bh)
{
    return (bw * bh + 31) / 32;
}

/* The header of a block message: r0, the block's corner from register `pos`, its size */
static uint32_t block_header(uint32_t (*k)[4], uint32_t n, int header, int pos, int bw, int bh)
{
    inst(k[n++], OP_MOV, 3, 0, dst(T_UD, header, 0), reg(T_UD, 0, 0, 4, 3, 1), none());
    inst(k[n++], OP_MOV, 1, 0, dst(T_UD, header, 0), reg(T_UD, pos, 0, 2, 1, 1), none());
    inst(k[n++], OP_MOV, 0, 0, dst(T_UD, header, 8), imm(T_UD, (uint32_t)(bh - 1) << 16 | (uint32_t)(bw - 1)), none());
    return n;
}

/* r2 = the block's corner in the destination, r3 in the source: block number * block size + the piece's corner */
static uint32_t positions(uint32_t (*k)[4], uint32_t n, int bw, int bh)
{
    inst(k[n++], OP_MUL, 0, 0, dst(T_UD, 2, 0), (operand_t){ FILE_GRF, T_UD, 0, 4, 0, 0, 0, 0 }, imm(T_UD, (uint32_t)bw));
    inst(k[n++], OP_MUL, 0, 0, dst(T_UD, 2, 4), (operand_t){ FILE_GRF, T_UD, 0, 24, 0, 0, 0, 0 }, imm(T_UD, (uint32_t)bh));
    inst(k[n++], OP_ADD, 1, 0, dst(T_UD, 3, 0), reg(T_UD, 2, 0, 2, 1, 1), reg(T_UD, 1, 8, 2, 1, 1));
    inst(k[n++], OP_ADD, 1, 0, dst(T_UD, 2, 0), reg(T_UD, 2, 0, 2, 1, 1), reg(T_UD, 1, 0, 2, 1, 1));
    return n;
}

static uint32_t end_thread(uint32_t (*k)[4], uint32_t n)
{
    inst(k[n++], OP_MOV, 3, 0, dst(T_UD, 112, 0), reg(T_UD, 0, 0, 4, 3, 1), none());
    send(k[n++], 4, SFID_SPAWNER, null_reg(), 112, END_OF_THREAD);
    return n;
}

/* Copy: the source's block into r21 on, written with the header in r20 */
static uint32_t copy_block(uint32_t (*k)[4], int bw, int bh)
{
    int registers = block_registers(bw, bh);
    uint32_t n = positions(k, 0, bw, bh);

    n = block_header(k, n, 4, 3, bw, bh);
    send(k[n++], 4, SFID_DATA_PORT1, dst(T_UD, 21, 0), 4, block_read(registers, 1));
    n = block_header(k, n, 20, 2, bw, bh);
    send(k[n++], 4, SFID_DATA_PORT1, null_reg(), 20, block_write(registers + 1, 0));
    return end_thread(k, n);
}

/*
 * Blend: the source (r21 on) with its alpha (byte 3 of each pixel) times the opacity over the destination (r29 on),
 * the result in r51 on with its header in r50. Per channel x = s * a + d * (255 - a), the result
 * (x + 128 + ((x + 128) >> 8)) >> 8, which is x / 255 rounded. Half a row of a register at a time (16 bytes, four
 * pixels) in words: r40 x, r41 255 - a, r42 and r43 in between, r44 for packing, r45 to r47 the bytes as words.
 */
static uint32_t blend_block(uint32_t (*k)[4], int bw, int bh)
{
    int registers = block_registers(bw, bh);
    uint32_t n = positions(k, 0, bw, bh);

    n = block_header(k, n, 4, 3, bw, bh);
    send(k[n++], 4, SFID_DATA_PORT1, dst(T_UD, 21, 0), 4, block_read(registers, 1));
    n = block_header(k, n, 5, 2, bw, bh);
    send(k[n++], 4, SFID_DATA_PORT1, dst(T_UD, 29, 0), 5, block_read(registers, 0));
    /* Bytes into words first (which a mov does safely), then only words with words. */
    operand_t x = reg(T_UW, 40, 0, 5, 4, 1), inverse = reg(T_UW, 41, 0, 5, 4, 1), t = reg(T_UW, 42, 0, 5, 4, 1);
    operand_t u = reg(T_UW, 43, 0, 5, 4, 1), sw = reg(T_UW, 45, 0, 5, 4, 1), aw = reg(T_UW, 46, 0, 5, 4, 1);
    operand_t dw = reg(T_UW, 47, 0, 5, 4, 1);
    for (int row = 0; row < registers; row++) {
        for (int half = 0; half < 2; half++) {
            int at = half * 16;
            operand_t s = reg(T_UB, 21 + row, at, 5, 4, 1), d = reg(T_UB, 29 + row, at, 5, 4, 1);
            operand_t a = reg(T_UB, 21 + row, at + 3, 3, 2, 0); /* <4;4,0>: each pixel's alpha four times */
            inst(k[n++], OP_MOV, 4, 0, dst(T_UW, 45, 0), s, none());
            inst(k[n++], OP_MOV, 4, 0, dst(T_UW, 46, 0), a, none());
            inst(k[n++], OP_MUL, 4, 0, dst(T_UW, 46, 0), aw, reg(T_UW, 1, 16, SCALAR)); /* a * opacity */
            inst(k[n++], OP_SHR, 4, 0, dst(T_UW, 46, 0), aw, imm(T_UW, 8));            /* / 256 */
            inst(k[n++], OP_MOV, 4, 0, dst(T_UW, 47, 0), d, none());
            inst(k[n++], OP_MUL, 4, 0, dst(T_UW, 40, 0), sw, aw);                      /* x = s * a */
            inst(k[n++], OP_XOR, 4, 0, dst(T_UW, 41, 0), aw, imm(T_UW, 0xFF));         /* 255 - a */
            inst(k[n++], OP_MUL, 4, 0, dst(T_UW, 42, 0), dw, inverse);                 /* d * (255 - a) */
            inst(k[n++], OP_ADD, 4, 0, dst(T_UW, 40, 0), x, t);
            inst(k[n++], OP_ADD, 4, 0, dst(T_UW, 40, 0), x, imm(T_UW, 128));
            inst(k[n++], OP_SHR, 4, 0, dst(T_UW, 43, 0), x, imm(T_UW, 8));
            inst(k[n++], OP_ADD, 4, 0, dst(T_UW, 40, 0), x, u);
            inst(k[n++], OP_SHR, 4, 0, dst(T_UW, 40, 0), x, imm(T_UW, 8));
            /* Four pixels packed: channel c of pixel p is word 4p + c; fetched as a dword, shifted and or-ed. */
            int out = 51 + row;
            inst(k[n++], OP_MOV, 2, 0, dst(T_UD, out, at), reg(T_UW, 40, 0, 3, 0, 0), none());
            for (int c = 1; c < 4; c++) {
                operand_t temp = reg(T_UD, 44, 0, 4, 4, 1);
                inst(k[n++], OP_MOV, 2, 0, dst(T_UD, 44, 0), reg(T_UW, 40, 2 * c, 3, 0, 0), none());
                inst(k[n++], OP_SHL, 2, 0, dst(T_UD, 44, 0), temp, imm(T_UD, 8u * (uint32_t)c));
                inst(k[n++], OP_OR, 2, 0, dst(T_UD, out, at), reg(T_UD, out, at, 4, 4, 1), temp);
            }
        }
    }
    n = block_header(k, n, 50, 2, bw, bh);
    send(k[n++], 4, SFID_DATA_PORT1, null_reg(), 50, block_write(registers + 1, 0));
    return end_thread(k, n);
}

uint32_t intel_kernel(bool blend, uint32_t shape, uint32_t (*code)[4])
{
    int bw = shapes[shape & 3][0], bh = shapes[shape & 3][1];
    return blend ? blend_block(code, bw, bh) : copy_block(code, bw, bh);
}

/* --- Rectangles ------------------------------------------------------------------------------ */

uint32_t intel_pieces(uint32_t to_x, uint32_t to_y, uint32_t from_x, uint32_t from_y, uint32_t width, uint32_t height,
                      intel_piece_t pieces[4])
{
    uint32_t w8 = width & ~7u, h8 = height & ~7u, wr = width & 7, hr = height & 7, count = 0;
    const uint32_t parts[4][5] = { /* x, y, width, height, shape */
        { 0, 0, w8, h8, INTEL_SHAPE_8X8 },
        { w8, 0, wr, h8, INTEL_SHAPE_1X8 },
        { 0, h8, w8, hr, INTEL_SHAPE_8X1 },
        { w8, h8, wr, hr, INTEL_SHAPE_1X1 },
    };

    for (uint32_t i = 0; i < 4; i++) {
        uint32_t x = parts[i][0], y = parts[i][1], w = parts[i][2], h = parts[i][3], shape = parts[i][4], bw, bh;
        if (!w || !h)
            continue;
        intel_shape_size(shape, &bw, &bh);
        pieces[count++] = (intel_piece_t){ .shape = shape, .columns = w / bw, .rows = h / bh,
                                           .to_x = (to_x + x) * 4, .to_y = to_y + y,
                                           .from_x = (from_x + x) * 4, .from_y = from_y + y };
    }
    return count;
}

uint32_t intel_blend_pixel(uint32_t source, uint32_t destination, uint32_t opacity)
{
    uint32_t a = ((source >> 24) * opacity) >> 8, result = 0;

    for (uint32_t c = 0; c < 32; c += 8) {
        uint32_t x = ((source >> c) & 0xFF) * a + ((destination >> c) & 0xFF) * (255 - a) + 128;
        result |= (((x + (x >> 8)) >> 8) & 0xFF) << c;
    }
    return result;
}
