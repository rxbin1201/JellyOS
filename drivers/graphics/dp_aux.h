/*
 * DisplayPort AUX channel, the part that is the same for every GPU.
 *
 * Over the AUX channel a source reads and writes the monitor's
 * configuration data (DPCD: capabilities, link settings, link status) and
 * talks I2C to it, which is how the EDID is read. A graphics driver brings
 * one function that sends a single AUX message with its hardware; retries,
 * DPCD access and the EDID are done here.
 */

#ifndef DRIVERS_GRAPHICS_DP_AUX_H
#define DRIVERS_GRAPHICS_DP_AUX_H

#include <stdbool.h>
#include <stdint.h>

/* Results of dp_aux_t.once and of the functions below, besides a byte count */
#define DP_AUX_NO_ANSWER (-1)
#define DP_AUX_ERROR     (-2)
#define DP_AUX_STUCK     (-3) /* the channel is busy and stays so */
#define DP_AUX_NACK      (-4)

/* DPCD addresses used by more than one driver */
#define DPCD_REVISION         0x000 /* then: 0x001 highest link rate (units of 0.27 Gbit/s), 0x002 lanes and flags */
#define DPCD_LINK_BW_SET      0x100 /* then: 0x101 lane count */
#define DPCD_LANE_STATUS      0x202

typedef struct dp_aux {
    void *context;
    /*
     * Send tx_length bytes (header and data) and receive the reply into rx: returns the number of bytes
     * received, of which rx[0] is the reply code, or DP_AUX_NO_ANSWER, DP_AUX_ERROR, DP_AUX_STUCK.
     */
    int (*once)(void *context, const uint8_t *tx, int tx_length, uint8_t *rx, int rx_max);
} dp_aux_t;

/* One message with retries for receive errors and DEFER (the monitor may put us off). i2c: an I2C-over-AUX message. */
int  dp_aux_transfer(const dp_aux_t *aux, const uint8_t *tx, int tx_length, uint8_t *rx, int rx_max, bool i2c);

/* Read up to 16 bytes of DPCD: returns the number read or a negative result. */
int  dp_dpcd_read(const dp_aux_t *aux, uint32_t address, uint8_t *buffer, int length);
/* Write up to 16 bytes of DPCD. */
bool dp_dpcd_write(const dp_aux_t *aux, uint32_t address, const uint8_t *data, int length);

/* The monitor's EDID (up to two blocks, 256 bytes), with retries: returns the number of valid blocks. */
int  dp_edid_read(const dp_aux_t *aux, uint8_t *edid);

#endif
