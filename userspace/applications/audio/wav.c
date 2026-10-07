/*
 * WAV file headers. See wav.h.
 */

#include "wav.h"

#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

int wav_read_header(FILE *file, wav_format_t *format)
{
    uint8_t header[12], chunk[8], fmt[16];
    int have_format = 0;

    if (fread(header, 1, 12, file) != 12 || memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4))
        return -1;
    /* Chunks in any order until "data"; unknown ones are skipped. */
    while (fread(chunk, 1, 8, file) == 8) {
        uint32_t size = le32(chunk + 4);
        if (!memcmp(chunk, "fmt ", 4)) {
            if (size < 16 || fread(fmt, 1, 16, file) != 16)
                return -1;
            uint32_t tag = fmt[0] | fmt[1] << 8;
            format->channels = fmt[2] | fmt[3] << 8;
            format->rate = le32(fmt + 4);
            format->bits = fmt[14] | fmt[15] << 8;
            /* 1 = PCM; 0xFFFE = extensible, which holds PCM for all files we care about */
            if ((tag != 1 && tag != 0xFFFE) || (format->bits != 8 && format->bits != 16) || format->channels < 1 ||
                format->channels > 2 || !format->rate)
                return -1;
            have_format = 1;
            size -= 16;
        } else if (!memcmp(chunk, "data", 4)) {
            format->data_bytes = size;
            return have_format ? 0 : -1;
        }
        if (fseek(file, (long)(size + (size & 1)), SEEK_CUR)) /* chunks are padded to even sizes */
            return -1;
    }
    return -1;
}

int wav_write_header(FILE *file, const wav_format_t *format)
{
    uint8_t h[44];
    uint32_t frame = format->channels * 2;

    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + format->data_bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    h[20] = 1; /* PCM */
    h[21] = 0;
    h[22] = (uint8_t)format->channels;
    h[23] = 0;
    put32(h + 24, format->rate);
    put32(h + 28, format->rate * frame);
    h[32] = (uint8_t)frame;
    h[33] = 0;
    h[34] = 16;
    h[35] = 0;
    memcpy(h + 36, "data", 4);
    put32(h + 40, format->data_bytes);
    return fwrite(h, 1, sizeof(h), file) == sizeof(h) ? 0 : -1;
}

int wav_finish(FILE *file, uint32_t data_bytes)
{
    uint8_t size[4];

    put32(size, 36 + data_bytes);
    if (fseek(file, 4, SEEK_SET) || fwrite(size, 1, 4, file) != 4)
        return -1;
    put32(size, data_bytes);
    if (fseek(file, 40, SEEK_SET) || fwrite(size, 1, 4, file) != 4)
        return -1;
    return fflush(file) ? -1 : 0;
}
