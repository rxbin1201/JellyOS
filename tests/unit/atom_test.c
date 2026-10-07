/*
 * Host unit tests for drivers/graphics/atom.c, the interpreter for the
 * programs in the video BIOS of AMD graphics.
 *
 * A real video BIOS is not at hand here (and QEMU has no such GPU), so the
 * test builds a small image by hand: the headers the interpreter looks for,
 * three command tables, a data table and a pair of indirect register
 * access methods. The registers are an array.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "drivers/graphics/atom.h"

static int failures, checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("unit: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                        \
    } while (0)

/* --- The machine: registers and time ------------------------------------------------ */

static uint32_t registers[0x100], delayed_us;

static uint32_t reg_read(void *context, uint32_t reg)
{
    (void)context;
    return reg < 0x100 ? registers[reg] : 0xFFFFFFFF;
}

static void reg_write(void *context, uint32_t reg, uint32_t value)
{
    (void)context;
    if (reg < 0x100)
        registers[reg] = value;
}

static void delay_us(void *context, uint32_t us)
{
    (void)context;
    delayed_us += us;
}

/* --- The image -------------------------------------------------------------------------- */

#define ROM_TABLE     0x60
#define COMMAND_TABLE 0x90
#define DATA_TABLE    0xC0
#define TABLE_0       0x100
#define TABLE_1       0x200
#define TABLE_BAD     0x240 /* an instruction that does not exist */
#define TABLE_LOOP    0x260 /* jumps to itself forever */
#define DATA_BLOCK    0x300
#define IIO_TABLE     0x340

static uint8_t image[0x400];
static uint32_t at;

static void put16(uint32_t where, uint32_t value)
{
    image[where] = (uint8_t)value;
    image[where + 1] = (uint8_t)(value >> 8);
}

/* Append bytes at `at`; the list ends with -1. */
static void emit(int first, ...)
{
    va_list list;
    va_start(list, first);
    for (int byte = first; byte != -1; byte = va_arg(list, int))
        image[at++] = (uint8_t)byte;
    va_end(list);
}

#define IMM32(v) (v) & 0xFF, ((v) >> 8) & 0xFF, ((v) >> 16) & 0xFF, ((v) >> 24) & 0xFF
#define IMM16(v) (v) & 0xFF, ((v) >> 8) & 0xFF

/* Opcodes: six in a row per operation, by destination (register, parameters, work space, ...) */
enum { TO_REG, TO_PS, TO_WS };
#define MOVE(d)       (1 + (d))
#define SHIFT_LEFT(d) (19 + (d))
#define DIV(d)        (37 + (d))
#define ADD(d)        (43 + (d))
#define COMPARE(d)    (60 + (d))
#define CLEAR(d)      (84 + (d))
#define MASK(d)       (92 + (d))
#define XOR(d)        (103 + (d))
#define SETPORT       55
#define SWITCH        66
#define JUMP          67
#define JUMP_EQUAL    68
#define DELAY_MS      80
#define CALLTABLE     82
#define EOT           91
#define SETDATABLOCK  102
/* Attribute: kind of source, and which part of the values (0: all 32 bits) */
enum { FROM_REG, FROM_PS, FROM_WS, FROM_FB, FROM_ID, FROM_IMM };
#define BYTE0_TO_BYTE1 (4 << 3 | 1 << 6) /* an 8-bit source into bits 15:8 of the destination */
#define BYTE0          (4 << 3)

static void table_header(uint32_t where, uint32_t ws_bytes, uint32_t ps_bytes)
{
    at = where;
    emit(0, 0, 1, 1, (int)ws_bytes, (int)ps_bytes, -1);
}

static void build_image(void)
{
    uint32_t patch_equal, patch_case, patch_done;

    memset(image, 0, sizeof(image));
    put16(0, 0xAA55);
    memcpy(image + 0x30, " 761295520", 10);
    put16(0x48, ROM_TABLE);
    memcpy(image + ROM_TABLE + 4, "ATOM", 4);
    put16(ROM_TABLE + 0x1E, COMMAND_TABLE);
    put16(ROM_TABLE + 0x20, DATA_TABLE);
    put16(COMMAND_TABLE + 4 + 2 * 0, TABLE_0);
    put16(COMMAND_TABLE + 4 + 2 * 1, TABLE_1);
    put16(COMMAND_TABLE + 4 + 2 * 2, TABLE_BAD);
    put16(COMMAND_TABLE + 4 + 2 * 3, TABLE_LOOP);
    put16(DATA_TABLE + 4 + 2 * 1, DATA_BLOCK);
    put16(DATA_TABLE + 0x32, IIO_TABLE);
    image[DATA_BLOCK + 2] = 0xEF; /* a dword at offset 2 of the data block */
    image[DATA_BLOCK + 3] = 0xBE;
    image[DATA_BLOCK + 4] = 0xAD;
    image[DATA_BLOCK + 5] = 0xDE;

    /* Table 0: two dwords of parameters of its own; what follows them belongs to the tables it calls. */
    table_header(TABLE_0, 16, 8);
    emit(MOVE(TO_WS), FROM_IMM, 0, IMM32(5), -1);                    /* WS[0] = 5 */
    emit(ADD(TO_WS), FROM_PS, 0, 0, -1);                             /* WS[0] += PS[0] */
    emit(MOVE(TO_REG), FROM_WS, IMM16(0x10), 0, -1);                 /* REG[0x10] = WS[0] */
    emit(MOVE(TO_REG), FROM_IMM | BYTE0_TO_BYTE1, IMM16(0x11), 0xAB, -1); /* bits 15:8 of REG[0x11] = 0xAB */
    emit(COMPARE(TO_WS), FROM_IMM, 0, IMM32(12), -1);                /* WS[0] == 12? */
    emit(JUMP_EQUAL, -1);
    patch_equal = at;
    emit(IMM16(0), -1);
    emit(MOVE(TO_REG), FROM_IMM, IMM16(0x12), IMM32(0xBAD), -1);     /* skipped when equal */
    put16(patch_equal, at - TABLE_0);
    emit(SHIFT_LEFT(TO_WS), 0, 0, 4, -1);                            /* WS[0] <<= 4 */
    emit(MASK(TO_REG), FROM_IMM, IMM16(0x13), IMM32(0xFF), IMM32(0xC00), -1); /* REG[0x13] = (it & 0xFF) | 0xC00 */
    emit(CALLTABLE, 1, -1);
    emit(SETDATABLOCK, 1, -1);
    emit(MOVE(TO_WS), FROM_ID, 1, IMM16(2), -1);                     /* WS[1] = the dword at data block + 2 */
    emit(MOVE(TO_REG), FROM_WS, IMM16(0x15), 1, -1);
    emit(SWITCH, FROM_WS, 0, -1);                                    /* on WS[0] */
    emit(0x63, IMM32(1), IMM16(0), -1);                              /* case 1: (never) */
    emit(0x63, IMM32(0xC0), -1);                                     /* case 0xC0: */
    patch_case = at;
    emit(IMM16(0), 0x5A, 0x5A, -1);
    emit(MOVE(TO_REG), FROM_IMM, IMM16(0x16), IMM32(0xBAD), -1);     /* no case matched */
    emit(JUMP, -1);
    patch_done = at;
    emit(IMM16(0), -1);
    put16(patch_case, at - TABLE_0);
    emit(MOVE(TO_REG), FROM_IMM, IMM16(0x16), IMM32(0x600D), -1);
    put16(patch_done, at - TABLE_0);
    emit(DIV(TO_WS), FROM_IMM, 0, IMM32(7), -1);                     /* quotient and remainder of WS[0] / 7 */
    emit(MOVE(TO_REG), FROM_WS, IMM16(0x17), 0x40, -1);
    emit(MOVE(TO_REG), FROM_WS, IMM16(0x18), 0x41, -1);
    emit(SETPORT, IMM16(1), -1);                                     /* registers through access method 1 */
    emit(MOVE(TO_REG), FROM_IMM, IMM16(0x20), IMM32(0x1234), -1);    /* "register 0x20" = 0x1234 */
    emit(MOVE(TO_WS), FROM_REG, 2, IMM16(0x21), -1);                 /* WS[2] = "register 0x21" */
    emit(SETPORT, IMM16(0), -1);                                     /* plain registers again */
    emit(MOVE(TO_REG), FROM_WS, IMM16(0x19), 2, -1);
    emit(DELAY_MS, 2, -1);
    emit(XOR(TO_WS), FROM_IMM, 0, IMM32(0xFF), -1);                  /* WS[0] ^= 0xFF */
    emit(MOVE(TO_REG), FROM_WS, IMM16(0x1A), 0, -1);
    emit(CLEAR(TO_REG), BYTE0, IMM16(0x11), -1);                     /* bits 7:0 of REG[0x11] = 0 */
    emit(EOT, -1);

    /* Table 1: its parameters are the caller's from the third dword on. */
    table_header(TABLE_1, 0, 8);
    emit(MOVE(TO_REG), FROM_PS, IMM16(0x14), 0, -1);                 /* REG[0x14] = PS[0] */
    emit(ADD(TO_REG), FROM_IMM, IMM16(0x14), IMM32(1), -1);          /* REG[0x14] += 1 */
    emit(MOVE(TO_PS), FROM_IMM, 1, IMM32(0x77), -1);                 /* PS[1] = 0x77: a result for the caller */
    emit(EOT, -1);

    table_header(TABLE_BAD, 0, 0);
    emit(MOVE(TO_REG), FROM_IMM, IMM16(0x1B), IMM32(1), 0x7F, EOT, -1);

    table_header(TABLE_LOOP, 0, 0);
    emit(JUMP, IMM16(6), -1);                                        /* to the first instruction: itself */

    /*
     * Indirect access: method 1 reads, method 0x81 writes. Both put the register number into an index
     * register (0x30) and then use a data register (0x31).
     */
    at = IIO_TABLE + 4;
    emit(1, 0x01, -1);       /* START, method 1 */
    emit(6, 32, 0, 0, -1);   /* MOVE_INDEX: the index into the value */
    emit(3, IMM16(0x30), -1); /* WRITE the value to register 0x30 */
    emit(2, IMM16(0x31), -1); /* READ register 0x31 */
    emit(9, 0, 0, -1);       /* END */
    emit(1, 0x81, -1);       /* START, method 0x81 */
    emit(6, 32, 0, 0, -1);
    emit(3, IMM16(0x30), -1);
    emit(8, 32, 0, 0, -1);   /* MOVE_DATA: all of the data into the value */
    emit(3, IMM16(0x31), -1);
    emit(9, 0, 0, -1);
}

int main(void)
{
    static uint32_t scratch[64];
    const atom_io_t io = { NULL, reg_read, reg_write, delay_us };
    uint32_t parameters[ATOM_PARAMETERS] = { 7, 0, 0x40 };
    uint8_t format = 0, content = 0;
    atom_t atom;

    build_image();

    /* An image that is not a video BIOS is refused. */
    CHECK(!atom_init(&atom, image, 0x20, &io, scratch, sizeof(scratch)));
    image[0] = 0;
    CHECK(!atom_init(&atom, image, sizeof(image), &io, scratch, sizeof(scratch)));
    image[0] = 0x55;
    CHECK(atom_init(&atom, image, sizeof(image), &io, scratch, sizeof(scratch)));
    CHECK(atom.command_table == COMMAND_TABLE && atom.data_table == DATA_TABLE);
    CHECK(atom.iio[1] && atom.iio[0x81] && !atom.iio[2]);
    CHECK(atom_table_revision(&atom, 0, &format, &content) && format == 1 && content == 1);
    CHECK(!atom_table_revision(&atom, 9, NULL, NULL));

    registers[0x11] = 0x11223344;
    registers[0x13] = 0x12345678;
    CHECK(atom_execute(&atom, 0, parameters));
    CHECK(atom.error == NULL);
    CHECK(registers[0x10] == 12);          /* 5 + the first parameter */
    CHECK(registers[0x11] == 0x1122AB00);  /* one byte replaced, later the lowest one cleared */
    CHECK(registers[0x12] == 0);           /* the jump was taken */
    CHECK(registers[0x13] == 0xC78);       /* mask and insert */
    CHECK(registers[0x14] == 0x41);        /* the called table saw the third parameter */
    CHECK(parameters[3] == 0x77);          /* and gave something back */
    CHECK(registers[0x15] == 0xDEADBEEF);  /* from the data block */
    CHECK(registers[0x16] == 0x600D);      /* the matching case of the switch */
    CHECK(registers[0x17] == 0xC0 / 7 && registers[0x18] == 0xC0 % 7);
    CHECK(registers[0x30] == 0x21);        /* the index register: last used for the read */
    CHECK(registers[0x31] == 0x1234 && registers[0x19] == 0x1234); /* written and read back indirectly */
    CHECK(registers[0x20] == 0);           /* nothing went to the plain register of that number */
    CHECK(registers[0x1A] == (0xC0 ^ 0xFF));
    CHECK(delayed_us == 2000);
    CHECK(atom.calls == 2 && atom.writes > 10 && atom.reads >= 2);

    /* Running again gives the same result: no state is left over. */
    parameters[0] = 7;
    registers[0x11] = 0x11223344;
    CHECK(atom_execute(&atom, 0, parameters) && registers[0x10] == 12 && registers[0x11] == 0x1122AB00);
    /* Another parameter: the comparison fails and the skipped instruction runs. */
    parameters[0] = 1;
    CHECK(atom_execute(&atom, 0, parameters) && registers[0x10] == 6 && registers[0x12] == 0xBAD);

    /* What must not hang or run wild */
    CHECK(!atom_execute(&atom, 2, parameters) && atom.error != NULL && registers[0x1B] == 1);
    CHECK(!atom_execute(&atom, 3, parameters) && atom.error != NULL);
    CHECK(!atom_execute(&atom, 9, parameters) && atom.error != NULL);

    printf("unit: atom: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
