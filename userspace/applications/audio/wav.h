/*
 * WAV files (RIFF/WAVE, uncompressed PCM) for play and record.
 */

#ifndef APPLICATIONS_AUDIO_WAV_H
#define APPLICATIONS_AUDIO_WAV_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t rate;
    uint32_t channels;
    uint32_t bits;        /* 8 (unsigned) or 16 (signed, little endian) */
    uint32_t data_bytes;
} wav_format_t;

/* Read the header up to the sample data. 0, or -1 if this is not a PCM WAV file we can play. */
int wav_read_header(FILE *file, wav_format_t *format);

/* Write a 16-bit PCM header; data_bytes may be 0 and fixed later with wav_finish(). */
int wav_write_header(FILE *file, const wav_format_t *format);
int wav_finish(FILE *file, uint32_t data_bytes);

#endif
