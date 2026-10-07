/*
 * tone: play a sine tone.
 *
 *   tone [-f HERTZ] [-d MILLISECONDS] [-v PERCENT] [-r RATE] [-m]
 *
 * -r chooses the sample rate of the stream (the audio server converts it),
 * -m plays mono. Defaults: 440 Hz, 500 ms, volume 100, 48000 Hz stereo.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/client/audio.h"

#define CHUNK 1024

int main(int argc, char **argv)
{
    int frequency = 440, duration = 500, volume = 100, rate = 48000, channels = 2, option;
    static int16_t frames[CHUNK * 2];
    audio_stream_t *stream;

    while ((option = getopt(argc, argv, "f:d:v:r:m")) != -1) {
        switch (option) {
        case 'f': frequency = atoi(optarg); break;
        case 'd': duration = atoi(optarg); break;
        case 'v': volume = atoi(optarg); break;
        case 'r': rate = atoi(optarg); break;
        case 'm': channels = 1; break;
        default:
            fprintf(stderr, "usage: tone [-f hertz] [-d milliseconds] [-v percent] [-r rate] [-m]\n");
            return 2;
        }
    }
    if (frequency <= 0 || duration <= 0 || rate <= 0 || frequency * 2 > rate) {
        fprintf(stderr, "tone: the frequency must be below half the sample rate\n");
        return 2;
    }
    status_t status = audio_open(AUDIO_PLAYBACK, (uint32_t)rate, (uint32_t)channels, &stream);
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "tone: no audio output: %s\n", status_name(status));
        return 1;
    }
    audio_set_volume(stream, volume < 0 ? 0 : (uint32_t)volume);

    size_t total = (size_t)((long long)rate * duration / 1000), fade = (size_t)rate / 100; /* 10 ms in and out: no clicks */
    double step = 2 * M_PI * frequency / rate;
    for (size_t position = 0; position < total;) {
        size_t n = total - position < CHUNK ? total - position : CHUNK;
        for (size_t i = 0; i < n; i++, position++) {
            double envelope = 1.0;
            if (position < fade)
                envelope = (double)position / fade;
            else if (total - position < fade)
                envelope = (double)(total - position) / fade;
            int16_t sample = (int16_t)(sin(step * position) * 16000.0 * envelope);
            for (int c = 0; c < channels; c++)
                frames[i * channels + c] = sample;
        }
        if (audio_write(stream, frames, n) != n) {
            fprintf(stderr, "tone: the audio server went away\n");
            return 1;
        }
    }
    audio_drain(stream);
    audio_close(stream);
    printf("tone: %d Hz for %d ms\n", frequency, duration);
    return 0;
}
