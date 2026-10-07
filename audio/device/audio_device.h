/*
 * Audio devices (README section 38).
 *
 *     audio driver  ─▶  audio device  ─▶  audio server  ─▶  mixer  ─▶  applications
 *
 * An audio device is what a driver registers here: a sound card's output
 * and/or input with a fixed format (interleaved signed 16-bit frames). The
 * device layer puts a ring buffer between the driver and its user:
 *
 *   playback: the user writes frames into the ring; the driver pulls one
 *             period at a time from its interrupt handler (silence and an
 *             underrun count when the ring runs dry)
 *   capture:  the driver pushes each recorded period; the user reads
 *
 * One user at a time opens a device (the audio server; root only). The
 * handle is waitable: it is signaled when the playback ring wants more
 * data or recorded frames are waiting, so the server sleeps in between and
 * the hardware sets the pace.
 */

#ifndef AUDIO_DEVICE_AUDIO_DEVICE_H
#define AUDIO_DEVICE_AUDIO_DEVICE_H

#include "core/object.h"

#include <jelly/syscall.h>
#include <stddef.h>

#define AUDIO_RING_PERIODS      8 /* ring size in periods */
#define AUDIO_LOW_WATER_PERIODS 2 /* playback: "wants more data" at or below this */

struct audio_device;

typedef struct {
    /* Start or stop the hardware stream. Thread context. */
    status_t (*playback_enable)(struct audio_device *device, bool enable);
    status_t (*capture_enable)(struct audio_device *device, bool enable);
} audio_device_ops_t;

typedef struct audio_device {
    /* Set by the driver before audio_device_register() */
    char                      name[32];
    uint32_t                  flags;    /* JELLY_AUDIO_PLAYBACK | JELLY_AUDIO_CAPTURE */
    uint32_t                  rate;
    uint32_t                  channels;
    uint32_t                  period;   /* frames per pull/push */
    const audio_device_ops_t *ops;
    void                     *driver_data;

    /* Managed by the device layer */
    uint32_t  index;
    object_t *user;                     /* the open handle's object, NULL while closed */
    bool      playing, capturing;
    int16_t  *playback, *capture;       /* rings of ring_frames frames */
    uint32_t  ring_frames;
    uint32_t  playback_head, playback_count;
    uint32_t  capture_head, capture_count;
    uint64_t  played_frames, captured_frames, underruns, overruns;
} audio_device_t;

/* --- Driver side ------------------------------------------------------------------ */

status_t audio_device_register(audio_device_t *device);

/* The next `frames` frames to play (interrupt context). */
void     audio_playback_pull(audio_device_t *device, int16_t *frames, uint32_t count);
/* `frames` frames were recorded (interrupt context). */
void     audio_capture_push(audio_device_t *device, const int16_t *frames, uint32_t count);

/* --- User side (system calls) ------------------------------------------------------- */

audio_device_t *audio_device_get(uint32_t index);
uint32_t audio_device_count(void);
void     audio_device_info(audio_device_t *device, jelly_audio_info_t *info);

/* Open for exclusive use: BUSY while another handle exists. Releasing the object stops the device. */
status_t audio_device_open(uint32_t index, object_t **object);
audio_device_t *audio_device_of(object_t *object);

/* Queue frames for playback / take recorded frames; never block. Return the frames transferred. */
size_t   audio_device_write(audio_device_t *device, const int16_t *frames, size_t count);
size_t   audio_device_read(audio_device_t *device, int16_t *frames, size_t count);

/* JELLY_AUDIO_* control commands */
status_t audio_device_control(audio_device_t *device, uint32_t command, uint64_t value, uint64_t *result);

#endif
