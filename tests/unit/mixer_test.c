/*
 * Host unit tests for audio/mixer: sample-rate and channel conversion,
 * volume, mixing and clipping.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/mixer/mixer.h"

static int failures, checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            printf("unit: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                        \
    } while (0)

/* A triangle wave: exact in integers, and linear interpolation reproduces its slopes. */
static int16_t triangle(size_t i, size_t period, int amplitude)
{
    size_t phase = i % period, half = period / 2;
    int value = phase < half ? (int)(phase * 2 * (size_t)amplitude / half) - amplitude
                             : amplitude - (int)((phase - half) * 2 * (size_t)amplitude / half);
    return (int16_t)value;
}

/* Sign changes from negative to non-negative on the left channel */
static int rising_crossings(const int16_t *stereo, size_t frames)
{
    int count = 0;
    for (size_t i = 1; i < frames; i++)
        count += stereo[2 * (i - 1)] < 0 && stereo[2 * i] >= 0;
    return count;
}

static void test_same_rate(void)
{
    mix_resampler_t r;
    int16_t in[] = { 100, -100, 200, -200, 300, -300, 400, -400 }, out[16];
    size_t used;

    CHECK(mix_resampler_init(&r, 48000, 2, 48000));
    /* The output runs one frame behind the input: the last frame waits for its successor. */
    CHECK(mix_resample(&r, in, 4, &used, out, 8) == 3 && used == 4);
    CHECK(out[0] == 100 && out[1] == -100 && out[2] == 200 && out[3] == -200 && out[4] == 300 && out[5] == -300);
    CHECK(mix_resample(&r, in, 1, &used, out, 8) == 1 && used == 1);
    CHECK(out[0] == 400 && out[1] == -400);
    /* Nothing in, nothing out; the output buffer limits, too */
    CHECK(mix_resample(&r, in, 0, &used, out, 8) == 0 && used == 0);
    CHECK(mix_resample(&r, in, 4, &used, out, 2) == 2 && used == 2);
}

static void test_mono_and_upsampling(void)
{
    mix_resampler_t r;
    int16_t in[] = { 0, 1000, 2000, 3000 }, out[32];
    size_t used;

    CHECK(mix_resampler_init(&r, 24000, 1, 48000));
    size_t made = mix_resample(&r, in, 4, &used, out, 16);
    CHECK(used == 4 && made == 6);
    /* Both channels carry the mono signal; every second frame lies halfway between two source frames */
    CHECK(out[0] == 0 && out[1] == 0);
    CHECK(out[2] == 500 && out[3] == 500);
    CHECK(out[4] == 1000 && out[6] == 1500 && out[8] == 2000 && out[10] == 2500);
}

static void test_downsampling(void)
{
    mix_resampler_t r;
    int16_t in[40], out[40];
    size_t used;

    for (int i = 0; i < 20; i++)
        in[2 * i] = in[2 * i + 1] = (int16_t)(i * 10);
    CHECK(mix_resampler_init(&r, 96000, 2, 48000));
    size_t made = mix_resample(&r, in, 20, &used, out, 20);
    CHECK(used == 20 && made == 10);
    CHECK(out[0] == 0 && out[2] == 20 && out[4] == 40 && out[18] == 180);
}

static void test_rates_keep_the_pitch(void)
{
    /* One second of a 441 Hz triangle at 44100 Hz must be one second of 441 Hz at 48000 Hz. */
    enum { IN = 44100, OUT = 48000 };
    int16_t *in = malloc(IN * sizeof(int16_t)), *out = malloc((OUT + 64) * 2 * sizeof(int16_t));
    int16_t *chunked = malloc((OUT + 64) * 2 * sizeof(int16_t));
    mix_resampler_t r;
    size_t used, made;

    for (size_t i = 0; i < IN; i++)
        in[i] = triangle(i, 100, 20000);
    CHECK(mix_resampler_init(&r, IN, 1, OUT));
    made = mix_resample(&r, in, IN, &used, out, OUT + 64);
    CHECK(used == IN);
    CHECK(made >= OUT - 4 && made <= OUT + 4);
    int crossings = rising_crossings(out, made);
    CHECK(crossings >= 440 && crossings <= 441);
    int peak = 0;
    for (size_t i = 0; i < made; i++)
        peak = abs(out[2 * i]) > peak ? abs(out[2 * i]) : peak;
    CHECK(peak > 19000 && peak <= 20000);

    /* Fed in odd chunks with a small output buffer, the result is the same. */
    CHECK(mix_resampler_init(&r, IN, 1, OUT));
    size_t in_pos = 0, out_pos = 0, got;
    do {
        size_t n = IN - in_pos < 317 ? IN - in_pos : 317;
        got = mix_resample(&r, in + in_pos, n, &used, chunked + 2 * out_pos, 101);
        in_pos += used;
        out_pos += got;
    } while (in_pos < IN || got == 101);
    CHECK(out_pos == made);
    CHECK(memcmp(out, chunked, made * 2 * sizeof(int16_t)) == 0);
    free(in);
    free(out);
    free(chunked);
}

static void test_invalid_formats(void)
{
    mix_resampler_t r;
    CHECK(!mix_resampler_init(&r, 0, 2, 48000));
    CHECK(!mix_resampler_init(&r, 48000, 0, 48000));
    CHECK(!mix_resampler_init(&r, 48000, 2, 0));
    CHECK(!mix_resampler_init(&r, 48000, 9, 48000));
    CHECK(!mix_resampler_init(&r, 1, 2, 48000 * 4));     /* a step of zero would never advance */
    CHECK(!mix_resampler_init(&r, 4000000, 2, 8000));
    CHECK(mix_resampler_init(&r, 8000, 1, 48000));
    CHECK(mix_resampler_init(&r, 192000, 6, 48000));
}

static void test_volume_and_mixing(void)
{
    int32_t sum[4] = { 0 };
    int16_t a[] = { 1000, -1000, 30000, -30000 }, b[] = { 500, 500, 10000, -10000 }, out[4], mono[2];

    CHECK(mix_gain(100) == MIX_GAIN_UNITY && mix_gain(0) == 0 && mix_gain(50) == MIX_GAIN_UNITY / 4);
    CHECK(mix_gain(250) == MIX_GAIN_UNITY);
    CHECK(mix_gain(10) < mix_gain(11));

    mix_add(sum, a, 4, MIX_GAIN_UNITY);
    mix_add(sum, b, 4, MIX_GAIN_UNITY / 2);
    CHECK(sum[0] == 1250 && sum[1] == -750 && sum[2] == 35000 && sum[3] == -35000);

    /* The sum is limited, not wrapped around */
    CHECK(mix_output(sum, out, 4, MIX_GAIN_UNITY) == 2);
    CHECK(out[0] == 1250 && out[1] == -750 && out[2] == 32767 && out[3] == -32768);
    /* With the master volume at half (a quarter of the amplitude) it fits */
    CHECK(mix_output(sum, out, 4, mix_gain(50)) == 0);
    CHECK(out[0] == 312 && out[1] == -187 && out[2] == 8750 && out[3] == -8750);
    CHECK(mix_output(sum, out, 4, 0) == 0 && out[0] == 0 && out[2] == 0);

    mix_stereo_to_mono((int16_t[]){ 100, 300, -32768, -32768 }, mono, 2);
    CHECK(mono[0] == 200 && mono[1] == -32768);
}

int main(void)
{
    test_same_rate();
    test_mono_and_upsampling();
    test_downsampling();
    test_rates_keep_the_pitch();
    test_invalid_formats();
    test_volume_and_mixing();
    printf("unit: mixer %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
