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

/* --- Link training -------------------------------------------------------------------- */

#define DPCD_TRAINING_PATTERN 0x102 /* then: 0x103-0x106 the lanes' levels */
#define DPCD_TRAINING_LANE0   0x103
#define DPCD_DOWNSPREAD_CTRL  0x107
#define DPCD_SET_POWER        0x600

/* Do all lanes report these bits (1: clock recovered, 2: equalized, 4: symbols locked)? */
static bool lanes_report(const uint8_t *status, uint32_t lanes, uint8_t bits)
{
    for (uint32_t i = 0; i < lanes; i++) {
        if (((status[i / 2] >> (4 * (i % 2))) & bits) != bits)
            return false;
    }
    return true;
}

bool dp_link_good(const uint8_t *status, uint32_t lanes)
{
    return lanes_report(status, lanes, 7) && (status[2] & 1); /* and the lanes are aligned with each other */
}

/* What the monitor asks for: the highest request of all lanes (one setting for all), within what the source has. */
static void requested_levels(const dp_source_t *source, const uint8_t *status, uint32_t lanes, uint8_t *swing,
                             uint8_t *emphasis)
{
    uint8_t s = 0, e = 0;

    for (uint32_t i = 0; i < lanes; i++) {
        uint8_t request = (uint8_t)(status[4 + i / 2] >> (4 * (i % 2)));
        if ((request & 3) > s)
            s = request & 3;
        if (((request >> 2) & 3) > e)
            e = (request >> 2) & 3;
    }
    if (s > source->max_swing)
        s = source->max_swing;
    if (s > 3 - e)
        s = (uint8_t)(3 - e);
    *swing = s;
    *emphasis = e;
}

/* The levels as the monitor is told them: with flags for "this is the most the source can do". */
static uint8_t lane_setting(const dp_source_t *source, uint8_t swing, uint8_t emphasis)
{
    return (uint8_t)(swing | (swing >= source->max_swing ? 0x04 : 0) | emphasis << 3 | (emphasis == 3 ? 0x20 : 0));
}

static void apply_levels(const dp_aux_t *aux, const dp_source_t *source, uint32_t lanes, uint8_t swing, uint8_t emphasis)
{
    uint8_t lane = lane_setting(source, swing, emphasis), set[4] = { lane, lane, lane, lane };

    source->levels(source->context, swing, emphasis);
    dp_dpcd_write(aux, DPCD_TRAINING_LANE0, set, (int)lanes);
}

bool dp_link_train(const dp_aux_t *aux, const dp_source_t *source, uint32_t link_khz, uint32_t lanes, bool spread,
                   dp_training_t *result)
{
    uint8_t caps[16], set[5], power = 1, rate = (uint8_t)(link_khz / 27000);
    dp_training_t r = { 0 };
    bool equalized = false;

    *result = r;
    /* The monitor may sleep (D3): wake it and give it a moment. */
    for (int tries = 0; tries < 3 && !dp_dpcd_write(aux, DPCD_SET_POWER, &power, 1); tries++)
        sleep_ms(1);
    sleep_ms(1);
    if (dp_dpcd_read(aux, DPCD_REVISION, caps, 16) != 16) {
        result->problem = "the monitor does not answer";
        return false;
    }
    if (caps[1] < rate || (caps[2] & 0x1Fu) < lanes) {
        result->problem = "the monitor does not take this rate or number of lanes";
        return false;
    }
    bool enhanced = caps[2] & 0x80, pattern3 = caps[2] & 0x40;
    uint32_t settle = (caps[14] & 0x7Fu) * 4; /* how long the monitor wants between adjustments, ms (0: under 1) */
    if (settle < 1 || settle > 16)
        settle = 1;

    /* The monitor: rate, lanes, framing, clock spreading, 8b/10b coding. */
    set[0] = rate;
    set[1] = (uint8_t)(lanes | (enhanced ? 0x80 : 0));
    dp_dpcd_write(aux, DPCD_LINK_BW_SET, set, 2);
    set[0] = spread ? 0x10 : 0;
    set[1] = 0x01;
    dp_dpcd_write(aux, DPCD_DOWNSPREAD_CTRL, set, 2);

    /* Clock recovery: pattern 1 at the lowest levels, scrambling off. */
    source->pattern(source->context, 1);
    source->levels(source->context, 0, 0);
    set[0] = 0x21;
    set[1] = set[2] = set[3] = set[4] = lane_setting(source, 0, 0);
    dp_dpcd_write(aux, DPCD_TRAINING_PATTERN, set, 1 + (int)lanes);
    for (int tries = 0, same = 0; tries < 20; tries++) {
        uint8_t swing, emphasis;
        sleep_ms(1);
        if (dp_dpcd_read(aux, DPCD_LANE_STATUS, r.status, 6) != 6)
            break;
        if (lanes_report(r.status, lanes, 1)) {
            r.clock_recovered = true;
            break;
        }
        requested_levels(source, r.status, lanes, &swing, &emphasis);
        same = swing == r.swing ? same + 1 : 0;
        if (same >= 5)
            break; /* the same level five times: it will not get better */
        r.swing = swing;
        r.emphasis = emphasis;
        apply_levels(aux, source, lanes, swing, emphasis);
    }

    /* Channel equalization: pattern 3 if the monitor knows it, otherwise 2. */
    if (r.clock_recovered) {
        source->pattern(source->context, pattern3 ? 3 : 2);
        set[0] = (uint8_t)((pattern3 ? 3 : 2) | 0x20);
        set[1] = set[2] = set[3] = set[4] = lane_setting(source, r.swing, r.emphasis);
        dp_dpcd_write(aux, DPCD_TRAINING_PATTERN, set, 1 + (int)lanes);
        for (int tries = 0; tries < 6; tries++) {
            sleep_ms(settle);
            if (dp_dpcd_read(aux, DPCD_LANE_STATUS, r.status, 6) != 6 || !lanes_report(r.status, lanes, 1))
                break; /* the clock was lost again */
            if (dp_link_good(r.status, lanes)) {
                equalized = true;
                break;
            }
            requested_levels(source, r.status, lanes, &r.swing, &r.emphasis);
            apply_levels(aux, source, lanes, r.swing, r.emphasis);
        }
    }

    /* Training off in the monitor, the source back to normal operation. */
    set[0] = 0;
    dp_dpcd_write(aux, DPCD_TRAINING_PATTERN, set, 1);
    source->pattern(source->context, 0);
    if (!equalized)
        r.problem = r.clock_recovered ? "channel equalization failed" : "clock recovery failed";
    *result = r;
    return equalized;
}
