/*
 * DisplayPort AUX channel: retries, DPCD and EDID. See dp_aux.h.
 */

#include "drivers/graphics/dp_aux.h"

#include "drivers/graphics/edid.h"

#include "core/string.h"
#include "scheduler/thread.h"

static void sleep_ms(uint32_t ms)
{
    thread_sleep((uint64_t)ms * 1000000);
}

int dp_aux_transfer(const dp_aux_t *aux, const uint8_t *tx, int tx_length, uint8_t *rx, int rx_max, bool i2c)
{
    int timeouts = 0;

    for (int tries = 0; tries < 50; tries++) {
        int n = aux->once(aux->context, tx, tx_length, rx, rx_max);
        if (n == DP_AUX_NO_ANSWER) {
            if (++timeouts >= 3)
                return DP_AUX_NO_ANSWER;
            continue;
        }
        if (n == DP_AUX_STUCK)
            return DP_AUX_STUCK;
        if (n < 0) {
            sleep_ms(1);
            continue;
        }
        uint8_t native = rx[0] & 0x30, i2c_reply = rx[0] & 0xC0;
        if (native == 0x10 || (i2c && i2c_reply == 0x40))
            return DP_AUX_NACK;
        if (native == 0x20 || (i2c && i2c_reply == 0x80)) {
            sleep_ms(1); /* DEFER */
            continue;
        }
        return n;
    }
    return DP_AUX_ERROR;
}

int dp_dpcd_read(const dp_aux_t *aux, uint32_t address, uint8_t *buffer, int length)
{
    uint8_t tx[4] = { (uint8_t)(0x9 << 4 | ((address >> 16) & 0xF)), (uint8_t)(address >> 8), (uint8_t)address,
                      (uint8_t)(length - 1) };
    uint8_t rx[20];
    int n = dp_aux_transfer(aux, tx, 4, rx, 1 + length, false);

    if (n < 1)
        return n < 0 ? n : DP_AUX_ERROR;
    memcpy(buffer, rx + 1, (size_t)(n - 1));
    return n - 1;
}

bool dp_dpcd_write(const dp_aux_t *aux, uint32_t address, const uint8_t *data, int length)
{
    uint8_t tx[20] = { (uint8_t)(0x8 << 4 | ((address >> 16) & 0xF)), (uint8_t)(address >> 8), (uint8_t)address,
                       (uint8_t)(length - 1) };
    uint8_t rx[4];

    memcpy(tx + 4, data, (size_t)length);
    return dp_aux_transfer(aux, tx, 4 + length, rx, 4, false) >= 1;
}

/* EDID as I2C over AUX (address 0x50): write offset 0, read in pieces of 16 bytes, stop. Returns blocks. */
static int edid_read_once(const dp_aux_t *aux, uint8_t *edid)
{
    uint8_t rx[20];
    uint8_t start[5] = { 0x4 << 4, 0, 0x50, 0, 0 }; /* I2C write, middle of transaction: one byte, offset 0 */
    int want = EDID_BLOCK, got = 0;

    if (dp_aux_transfer(aux, start, 5, rx, 20, true) < 1)
        return 0;
    while (got < want) {
        int length = want - got > 16 ? 16 : want - got;
        uint8_t read[4] = { 0x5 << 4, 0, 0x50, (uint8_t)(length - 1) }; /* I2C read, middle of transaction */
        int n = dp_aux_transfer(aux, read, 4, rx, 1 + length, true);
        if (n < 2)
            break;
        memcpy(edid + got, rx + 1, (size_t)(n - 1));
        got += n - 1;
        if (got == EDID_BLOCK && edid[126] && edid_header_ok(edid))
            want = 2 * EDID_BLOCK;
    }
    uint8_t stop[3] = { 0x1 << 4, 0, 0x50 }; /* address only, without "middle of transaction": stop */
    dp_aux_transfer(aux, stop, 3, rx, 20, true);
    if (got < EDID_BLOCK || !edid_header_ok(edid))
        return 0;
    return got >= 2 * EDID_BLOCK ? 2 : 1;
}

/* Right after power-on a monitor may answer incompletely: up to five attempts. */
int dp_edid_read(const dp_aux_t *aux, uint8_t *edid)
{
    int blocks = 0;

    for (int tries = 0; tries < 5; tries++) {
        blocks = edid_read_once(aux, edid);
        int want = blocks && edid[126] ? 2 : 1;
        if (blocks >= want && edid_block_ok(edid) && (want < 2 || edid_block_ok(edid + EDID_BLOCK)))
            return blocks;
        sleep_ms(20);
    }
    return blocks && edid_block_ok(edid) ? 1 : 0;
}
