/*
 * Protocol between applications and the audio server (service "audio").
 *
 * A connection carries at most one stream. The samples do not travel
 * through the channel: the server answers AUDIO_OPEN with a shared-memory
 * ring (audio_ring_t) that one side fills and the other empties.
 *
 *   playback: the client writes frames and advances `write`; the server's
 *             mixer takes them at the pace of the sound card and advances
 *             `read`. A stream that has gone idle is started again with
 *             AUDIO_START.
 *   capture:  the server writes recorded frames, the client reads.
 *
 * The side that finds the ring full (or empty) sets `waiting` and sleeps
 * on a futex on the other side's counter; the other side wakes it after
 * moving the counter.
 *
 * Applications use the client library (audio.h), not this file.
 */

#ifndef AUDIO_CLIENT_PROTOCOL_H
#define AUDIO_CLIENT_PROTOCOL_H

#include <stdint.h>

#define AUDIO_SERVICE_NAME "audio"

#define AUDIO_PLAYBACK 1
#define AUDIO_CAPTURE  2

#define AUDIO_RATE_MIN     4000
#define AUDIO_RATE_MAX     192000
#define AUDIO_CHANNELS_MAX 2

/* Requests (client to server); every request except AUDIO_START gets one reply of the same type. */
#define AUDIO_OPEN       1 /* a = direction, b = rate, c = channels; reply: a = ring frames, d = bytes of the ring's
                              memory, which comes with the reply as a handle */
#define AUDIO_START      2 /* playback: there is data again (no reply) */
#define AUDIO_SET_VOLUME 3 /* a = percent (this stream) */
#define AUDIO_DRAIN      4 /* the reply comes when everything written has been played */
#define AUDIO_GET_MASTER 5 /* reply: a = percent, b = muted */
#define AUDIO_SET_MASTER 6 /* a = percent, b = muted; reply as AUDIO_GET_MASTER */
#define AUDIO_GET_INFO   7 /* reply: a = device rate, b = device channels, c = streams, d = underruns, name */

typedef struct {
    uint32_t type;
    int32_t  status;   /* replies: a status_t */
    uint32_t a, b, c, d;
    char     name[32];
} audio_message_t;

/* The shared ring. `read` and `write` count frames and run freely; write - read is the fill level. */
typedef struct {
    uint32_t capacity; /* frames */
    uint32_t rate;
    uint32_t channels;
    uint32_t direction;
    uint32_t write;    /* advanced by the producer */
    uint32_t read;     /* advanced by the consumer */
    uint32_t waiting;  /* the client sleeps on the server's counter */
    uint32_t active;   /* playback: the server is mixing this stream */
    uint32_t closed;   /* the server is gone or has dropped the stream */
    uint32_t reserved[7];
    int16_t  samples[];
} audio_ring_t;

#endif
