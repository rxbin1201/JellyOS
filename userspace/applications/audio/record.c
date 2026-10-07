/*
 * record: record from the sound card's input into a WAV file (16 bits).
 *
 *   record [-d SECONDS] [-r RATE] [-c CHANNELS] FILE
 *
 * Defaults: 3 seconds, 48000 Hz, stereo.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "audio/client/audio.h"
#include "wav.h"

#define CHUNK_FRAMES 2048

int main(int argc, char **argv)
{
    static int16_t frames[CHUNK_FRAMES * 2];
    int seconds = 3, rate = 48000, channels = 2, option;
    audio_stream_t *stream;

    while ((option = getopt(argc, argv, "d:r:c:")) != -1) {
        switch (option) {
        case 'd': seconds = atoi(optarg); break;
        case 'r': rate = atoi(optarg); break;
        case 'c': channels = atoi(optarg); break;
        default:  optind = argc; break;
        }
    }
    if (optind + 1 != argc || seconds <= 0 || rate <= 0 || channels < 1 || channels > 2) {
        fprintf(stderr, "usage: record [-d seconds] [-r rate] [-c 1|2] file.wav\n");
        return 2;
    }
    const char *path = argv[optind];
    wav_format_t format = { .rate = (uint32_t)rate, .channels = (uint32_t)channels, .bits = 16 };

    status_t status = audio_open(AUDIO_CAPTURE, format.rate, format.channels, &stream);
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "record: no audio input: %s\n", status_name(status));
        return 1;
    }
    FILE *file = fopen(path, "wb");
    if (!file || wav_write_header(file, &format)) {
        fprintf(stderr, "record: %s: cannot write the file\n", path);
        return 1;
    }

    size_t total = (size_t)rate * (size_t)seconds, done = 0;
    int peak = 0;
    while (done < total) {
        size_t n = total - done < CHUNK_FRAMES ? total - done : CHUNK_FRAMES;
        size_t got = audio_read(stream, frames, n);
        for (size_t i = 0; i < got * format.channels; i++)
            peak = abs(frames[i]) > peak ? abs(frames[i]) : peak;
        if (fwrite(frames, format.channels * 2, got, file) != got) {
            fprintf(stderr, "record: %s: write error\n", path);
            return 1;
        }
        done += got;
        if (got != n) {
            fprintf(stderr, "record: the audio server went away\n");
            break;
        }
    }
    audio_close(stream);
    if (wav_finish(file, (uint32_t)(done * format.channels * 2)) || fclose(file)) {
        fprintf(stderr, "record: %s: write error\n", path);
        return 1;
    }
    printf("record: %s: %lu frames at %d Hz, peak %d%%\n", path, (unsigned long)done, rate, peak * 100 / 32768);
    return done == total ? 0 : 1;
}
