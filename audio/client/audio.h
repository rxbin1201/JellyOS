/*
 * Audio API for applications (README section 38).
 *
 *     audio_stream_t *out;
 *     audio_open(AUDIO_PLAYBACK, 44100, 2, &out);    // any rate; mono or stereo
 *     audio_write(out, frames, count);               // blocks while the buffer is full
 *     audio_drain(out);                              // until the last frame was played
 *     audio_close(out);
 *
 * Frames are interleaved signed 16-bit samples. The audio server converts
 * the format, mixes all streams and applies the stream and master volumes.
 */

#ifndef AUDIO_CLIENT_AUDIO_H
#define AUDIO_CLIENT_AUDIO_H

#include "audio/client/protocol.h"

#include <jelly/status.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct audio_stream audio_stream_t;

typedef struct {
    uint32_t rate, channels; /* of the sound card */
    uint32_t streams;        /* open streams of all applications */
    uint32_t underruns;
    char     name[32];
} audio_info_t;

/* direction: AUDIO_PLAYBACK or AUDIO_CAPTURE. NOT_FOUND without an audio server (no sound card). */
status_t audio_open(uint32_t direction, uint32_t rate, uint32_t channels, audio_stream_t **stream);
void     audio_close(audio_stream_t *stream);

/* Play `count` frames; blocks until all are queued. Returns the frames written (less only on errors). */
size_t   audio_write(audio_stream_t *stream, const int16_t *frames, size_t count);
/* Record: blocks until `count` frames are there. Returns the frames read (less only on errors). */
size_t   audio_read(audio_stream_t *stream, int16_t *frames, size_t count);

/* Frames that can be written (playback) or read (capture) right now without blocking. */
size_t   audio_available(audio_stream_t *stream);

/* Wait until everything written has been played. */
status_t audio_drain(audio_stream_t *stream);

/* Volume of this stream, 0-100. */
status_t audio_set_volume(audio_stream_t *stream, uint32_t percent);

/* The system volume; these work without a stream. */
status_t audio_get_master(uint32_t *percent, bool *muted);
status_t audio_set_master(uint32_t percent, bool muted);
status_t audio_get_info(audio_info_t *info);

#endif
