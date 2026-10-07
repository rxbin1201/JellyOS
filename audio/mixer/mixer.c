/*
 * Mixer: resampling, volume, summing. See mixer.h.
 */

#include "audio/mixer/mixer.h"

bool mix_resampler_init(mix_resampler_t *r, uint32_t in_rate, uint32_t in_channels, uint32_t out_rate)
{
    __builtin_memset(r, 0, sizeof(*r));
    if (!in_rate || !out_rate || !in_channels || in_channels > 8)
        return false;
    uint64_t step = ((uint64_t)in_rate << 16) / out_rate;
    if (step == 0 || step > (64u << 16)) /* more than 64:1 either way is not audio any more */
        return false;
    r->in_channels = in_channels;
    r->step = (uint32_t)step;
    return true;
}

static void next_frame(mix_resampler_t *r, const int16_t *frame)
{
    r->a[0] = r->b[0];
    r->a[1] = r->b[1];
    r->b[0] = frame[0];
    r->b[1] = r->in_channels > 1 ? frame[1] : frame[0];
}

size_t mix_resample(mix_resampler_t *r, const int16_t *in, size_t in_frames, size_t *consumed, int16_t *out,
                    size_t out_frames)
{
    size_t used = 0, made = 0;

    /* Interpolation needs two frames to start with. */
    while (r->primed < 2 && used < in_frames) {
        next_frame(r, in + used++ * r->in_channels);
        r->primed++;
    }
    while (r->primed == 2 && made < out_frames) {
        while (r->fraction >= 65536 && used < in_frames) {
            next_frame(r, in + used++ * r->in_channels);
            r->fraction -= 65536;
        }
        if (r->fraction >= 65536)
            break; /* out of source frames */
        for (int c = 0; c < 2; c++)
            out[made * 2 + c] = (int16_t)(r->a[c] + ((int64_t)(r->b[c] - r->a[c]) * r->fraction) / 65536);
        made++;
        r->fraction += r->step;
    }
    *consumed = used;
    return made;
}

uint32_t mix_gain(uint32_t percent)
{
    if (percent > 100)
        percent = 100;
    return (uint32_t)(((uint64_t)percent * percent * MIX_GAIN_UNITY) / 10000);
}

void mix_add(int32_t *accumulator, const int16_t *samples, size_t count, uint32_t gain)
{
    if (gain == MIX_GAIN_UNITY) {
        for (size_t i = 0; i < count; i++)
            accumulator[i] += samples[i];
        return;
    }
    for (size_t i = 0; i < count; i++)
        accumulator[i] += (int32_t)(((int64_t)samples[i] * gain) / (int64_t)MIX_GAIN_UNITY);
}

size_t mix_output(const int32_t *accumulator, int16_t *out, size_t count, uint32_t gain)
{
    size_t clipped = 0;

    for (size_t i = 0; i < count; i++) {
        int64_t value = ((int64_t)accumulator[i] * gain) / (int64_t)MIX_GAIN_UNITY;
        if (value > INT16_MAX) {
            value = INT16_MAX;
            clipped++;
        } else if (value < INT16_MIN) {
            value = INT16_MIN;
            clipped++;
        }
        out[i] = (int16_t)value;
    }
    return clipped;
}

void mix_stereo_to_mono(const int16_t *stereo, int16_t *mono, size_t frames)
{
    for (size_t i = 0; i < frames; i++)
        mono[i] = (int16_t)(((int32_t)stereo[2 * i] + stereo[2 * i + 1]) / 2);
}
