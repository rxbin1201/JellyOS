/*
 * play: play WAV files (PCM, 8 or 16 bits, mono or stereo, any sample rate).
 *
 *   play [-v PERCENT] FILE...
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "audio/client/audio.h"
#include "wav.h"

#define CHUNK_FRAMES 2048

static int play(const char *path, int volume)
{
    static uint8_t raw[CHUNK_FRAMES * 4];
    static int16_t frames[CHUNK_FRAMES * 2];
    wav_format_t format;
    audio_stream_t *stream;

    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "play: %s: cannot open the file\n", path);
        return 1;
    }
    if (wav_read_header(file, &format)) {
        fprintf(stderr, "play: %s: not a PCM WAV file (8 or 16 bits, mono or stereo)\n", path);
        fclose(file);
        return 1;
    }
    status_t status = audio_open(AUDIO_PLAYBACK, format.rate, format.channels, &stream);
    if (STATUS_IS_ERROR(status)) {
        fprintf(stderr, "play: no audio output: %s\n", status_name(status));
        fclose(file);
        return 1;
    }
    audio_set_volume(stream, (uint32_t)volume);

    size_t frame = format.channels * format.bits / 8, left = format.data_bytes / frame, total = left;
    int result = 0;
    while (left) {
        size_t n = fread(raw, frame, left < CHUNK_FRAMES ? left : CHUNK_FRAMES, file);
        if (!n)
            break; /* shorter than the header says */
        size_t samples = n * format.channels;
        if (format.bits == 8) {
            for (size_t i = 0; i < samples; i++)
                frames[i] = (int16_t)((raw[i] - 128) << 8);
        } else {
            memcpy(frames, raw, samples * 2);
        }
        if (audio_write(stream, frames, n) != n) {
            fprintf(stderr, "play: the audio server went away\n");
            result = 1;
            break;
        }
        left -= n;
    }
    audio_drain(stream);
    audio_close(stream);
    fclose(file);
    if (!result)
        printf("play: %s: %u Hz, %u channel%s, %lu ms\n", path, format.rate, format.channels,
               format.channels == 1 ? "" : "s", (unsigned long)((total - left) * 1000 / format.rate));
    return result;
}

int main(int argc, char **argv)
{
    int volume = 100, option, result = 0;

    while ((option = getopt(argc, argv, "v:")) != -1) {
        if (option != 'v') {
            fprintf(stderr, "usage: play [-v percent] file.wav...\n");
            return 2;
        }
        volume = atoi(optarg);
    }
    if (optind >= argc) {
        fprintf(stderr, "usage: play [-v percent] file.wav...\n");
        return 2;
    }
    for (int i = optind; i < argc; i++)
        result |= play(argv[i], volume < 0 ? 0 : volume);
    return result;
}
