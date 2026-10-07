/*
 * Mixer (README section 38): sample-rate conversion, volume and the sum of
 * several streams. Everything works on interleaved signed 16-bit stereo
 * frames at the output side.
 *
 * Used by the audio server; without OS dependencies, so the host unit
 * tests (tests/unit/mixer_test.c) compile it directly.
 */

#ifndef AUDIO_MIXER_MIXER_H
#define AUDIO_MIXER_MIXER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIX_GAIN_UNITY 65536u

/* --- Sample-rate and channel conversion ------------------------------------------- */

/*
 * Linear interpolation between neighbouring source frames, with a 16.16
 * fixed-point position. Mono sources are copied to both channels; of more
 * than two channels the first two are used.
 */
typedef struct {
    uint32_t in_channels;
    uint32_t step;       /* source frames per output frame, 16.16 */
    uint32_t fraction;   /* position between `a` and `b`; >= 65536: the next source frame is needed */
    int16_t  a[2], b[2];
    uint32_t primed;     /* source frames seen, up to 2 */
} mix_resampler_t;

/* False for a rate or channel count of 0 or an unusable ratio. */
bool   mix_resampler_init(mix_resampler_t *r, uint32_t in_rate, uint32_t in_channels, uint32_t out_rate);

/*
 * Convert up to `in_frames` source frames into up to `out_frames` stereo
 * frames. Returns the frames written to `out`; *consumed is the number of
 * source frames used. Stops when either side is exhausted, so a ring
 * buffer's two spans are fed one after the other.
 */
size_t mix_resample(mix_resampler_t *r, const int16_t *in, size_t in_frames, size_t *consumed, int16_t *out,
                    size_t out_frames);

/* --- Volume and mixing -------------------------------------------------------------- */

/* Volume in percent (0-100) as a linear gain with a quadratic curve: 100 -> unity, 50 -> a quarter (-12 dB). */
uint32_t mix_gain(uint32_t percent);

/* accumulator[i] += samples[i] * gain */
void   mix_add(int32_t *accumulator, const int16_t *samples, size_t count, uint32_t gain);

/* out[i] = accumulator[i] * gain, limited to the 16-bit range. Returns the number of samples that had to be clipped. */
size_t mix_output(const int32_t *accumulator, int16_t *out, size_t count, uint32_t gain);

/* Stereo frames to mono (the mean of both channels), in place or to another buffer. */
void   mix_stereo_to_mono(const int16_t *stereo, int16_t *mono, size_t frames);

#endif
