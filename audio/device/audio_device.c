/*
 * Audio device layer: the rings between sound drivers and the audio server.
 * See audio_device.h.
 *
 * The rings are filled and emptied from interrupt handlers on one side and
 * system calls on the other, so every access runs with interrupts disabled.
 */

#include "audio/device/audio_device.h"

#include "core/arch.h"
#include "core/export.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"

#define DEVICE_MAX 4

typedef struct {
    object_t        object;
    audio_device_t *device;
} audio_handle_t;

static audio_device_t *devices[DEVICE_MAX];
static uint32_t device_count;

status_t audio_device_register(audio_device_t *device)
{
    if (device_count == DEVICE_MAX)
        return STATUS_LIMIT_EXCEEDED;
    if (!device->period || !device->channels || !device->rate || !device->ops)
        return STATUS_INVALID_ARGUMENT;

    device->ring_frames = device->period * AUDIO_RING_PERIODS;
    size_t bytes = (size_t)device->ring_frames * device->channels * sizeof(int16_t);
    if (device->flags & JELLY_AUDIO_PLAYBACK)
        device->playback = kcalloc(1, bytes);
    if (device->flags & JELLY_AUDIO_CAPTURE)
        device->capture = kcalloc(1, bytes);
    if (((device->flags & JELLY_AUDIO_PLAYBACK) && !device->playback) ||
        ((device->flags & JELLY_AUDIO_CAPTURE) && !device->capture)) {
        kfree(device->playback);
        kfree(device->capture);
        return STATUS_OUT_OF_MEMORY;
    }
    device->index = device_count;
    devices[device_count++] = device;
    klog_info("audio: device %u: %s (%u Hz, %u channels%s%s)", device->index, device->name, device->rate,
              device->channels, (device->flags & JELLY_AUDIO_PLAYBACK) ? ", playback" : "",
              (device->flags & JELLY_AUDIO_CAPTURE) ? ", capture" : "");
    return STATUS_SUCCESS;
}

audio_device_t *audio_device_get(uint32_t index)
{
    return index < device_count ? devices[index] : NULL;
}

uint32_t audio_device_count(void)
{
    return device_count;
}

/* --- Driver side ------------------------------------------------------------------ */

static size_t frame_bytes(const audio_device_t *device)
{
    return device->channels * sizeof(int16_t);
}

void audio_playback_pull(audio_device_t *device, int16_t *frames, uint32_t count)
{
    size_t size = frame_bytes(device);
    uint64_t saved = arch_interrupts_save();
    uint32_t available = device->playback_count < count ? device->playback_count : count;

    for (uint32_t done = 0; done < available;) {
        uint32_t run = device->ring_frames - device->playback_head;
        if (run > available - done)
            run = available - done;
        memcpy((uint8_t *)frames + done * size, (uint8_t *)device->playback + device->playback_head * size, run * size);
        device->playback_head = (device->playback_head + run) % device->ring_frames;
        done += run;
    }
    device->playback_count -= available;
    if (available < count) {
        memset((uint8_t *)frames + available * size, 0, (count - available) * size);
        device->underruns++;
    }
    device->played_frames += count;
    if (device->user)
        object_notify(device->user);
    arch_interrupts_restore(saved);
}

void audio_capture_push(audio_device_t *device, const int16_t *frames, uint32_t count)
{
    size_t size = frame_bytes(device);
    uint64_t saved = arch_interrupts_save();

    if (!device->capturing || count > device->ring_frames) {
        arch_interrupts_restore(saved);
        return;
    }
    /* A reader that fell behind loses the oldest frames. */
    if (device->capture_count + count > device->ring_frames) {
        uint32_t drop = device->capture_count + count - device->ring_frames;
        device->capture_head = (device->capture_head + drop) % device->ring_frames;
        device->capture_count -= drop;
        device->overruns++;
    }
    uint32_t tail = (device->capture_head + device->capture_count) % device->ring_frames;
    for (uint32_t done = 0; done < count;) {
        uint32_t run = device->ring_frames - tail;
        if (run > count - done)
            run = count - done;
        memcpy((uint8_t *)device->capture + tail * size, (const uint8_t *)frames + done * size, run * size);
        tail = (tail + run) % device->ring_frames;
        done += run;
    }
    device->capture_count += count;
    device->captured_frames += count;
    if (device->user)
        object_notify(device->user);
    arch_interrupts_restore(saved);
}

/* --- User side -------------------------------------------------------------------- */

void audio_device_info(audio_device_t *device, jelly_audio_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->index = device->index;
    info->flags = device->flags;
    info->rate = device->rate;
    info->channels = device->channels;
    info->period = device->period;
    info->buffer = device->ring_frames;
    memcpy(info->name, device->name, sizeof(info->name));
    info->name[sizeof(info->name) - 1] = '\0';
    info->played_frames = device->played_frames;
    info->captured_frames = device->captured_frames;
    info->underruns = device->underruns;
    info->overruns = device->overruns;
}

static bool handle_signaled(object_t *object)
{
    audio_device_t *device = audio_device_of(object);
    return (device->playing && device->playback_count <= device->period * AUDIO_LOW_WATER_PERIODS) ||
           (device->capturing && device->capture_count >= device->period);
}

static void handle_destroy(object_t *object)
{
    audio_device_t *device = audio_device_of(object);

    audio_device_control(device, JELLY_AUDIO_PLAYBACK_ENABLE, 0, NULL);
    audio_device_control(device, JELLY_AUDIO_CAPTURE_ENABLE, 0, NULL);
    uint64_t saved = arch_interrupts_save();
    device->user = NULL;
    arch_interrupts_restore(saved);
    kfree(container_of(object, audio_handle_t, object));
}

static const object_ops_t handle_ops = {
    .destroy = handle_destroy,
    .signaled = handle_signaled,
};

status_t audio_device_open(uint32_t index, object_t **object)
{
    audio_device_t *device = audio_device_get(index);

    if (!device)
        return STATUS_NOT_FOUND;
    if (device->user)
        return STATUS_BUSY;
    audio_handle_t *handle = kcalloc(1, sizeof(*handle));
    if (!handle)
        return STATUS_OUT_OF_MEMORY;
    object_init(&handle->object, OBJECT_AUDIO, &handle_ops);
    handle->device = device;
    device->user = &handle->object;
    *object = &handle->object;
    return STATUS_SUCCESS;
}

audio_device_t *audio_device_of(object_t *object)
{
    return container_of(object, audio_handle_t, object)->device;
}

size_t audio_device_write(audio_device_t *device, const int16_t *frames, size_t count)
{
    size_t size = frame_bytes(device);

    if (!device->playback)
        return 0;
    uint64_t saved = arch_interrupts_save();
    uint32_t space = device->ring_frames - device->playback_count;
    if (count > space)
        count = space;
    uint32_t tail = (device->playback_head + device->playback_count) % device->ring_frames;
    for (size_t done = 0; done < count;) {
        size_t run = device->ring_frames - tail;
        if (run > count - done)
            run = count - done;
        memcpy((uint8_t *)device->playback + tail * size, (const uint8_t *)frames + done * size, run * size);
        tail = (uint32_t)((tail + run) % device->ring_frames);
        done += run;
    }
    device->playback_count += (uint32_t)count;
    arch_interrupts_restore(saved);
    return count;
}

size_t audio_device_read(audio_device_t *device, int16_t *frames, size_t count)
{
    size_t size = frame_bytes(device);

    if (!device->capture)
        return 0;
    uint64_t saved = arch_interrupts_save();
    if (count > device->capture_count)
        count = device->capture_count;
    for (size_t done = 0; done < count;) {
        size_t run = device->ring_frames - device->capture_head;
        if (run > count - done)
            run = count - done;
        memcpy((uint8_t *)frames + done * size, (uint8_t *)device->capture + device->capture_head * size, run * size);
        device->capture_head = (uint32_t)((device->capture_head + run) % device->ring_frames);
        done += run;
    }
    device->capture_count -= (uint32_t)count;
    arch_interrupts_restore(saved);
    return count;
}

status_t audio_device_control(audio_device_t *device, uint32_t command, uint64_t value, uint64_t *result)
{
    status_t status = STATUS_SUCCESS;
    uint64_t saved;

    switch (command) {
    case JELLY_AUDIO_PLAYBACK_ENABLE:
        if (!(device->flags & JELLY_AUDIO_PLAYBACK))
            return value ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
        if (!!value == device->playing)
            return STATUS_SUCCESS;
        if (value) {
            /* The driver pulls the first periods while it starts. */
            status = device->ops->playback_enable(device, true);
            if (!STATUS_IS_ERROR(status))
                device->playing = true;
        } else {
            device->playing = false;
            status = device->ops->playback_enable(device, false);
            saved = arch_interrupts_save();
            device->playback_head = device->playback_count = 0;
            arch_interrupts_restore(saved);
        }
        return status;
    case JELLY_AUDIO_CAPTURE_ENABLE:
        if (!(device->flags & JELLY_AUDIO_CAPTURE))
            return value ? STATUS_NOT_SUPPORTED : STATUS_SUCCESS;
        if (!!value == device->capturing)
            return STATUS_SUCCESS;
        if (value) {
            device->capturing = true;
            status = device->ops->capture_enable(device, true);
            if (STATUS_IS_ERROR(status))
                device->capturing = false;
        } else {
            status = device->ops->capture_enable(device, false);
            saved = arch_interrupts_save();
            device->capturing = false;
            device->capture_head = device->capture_count = 0;
            arch_interrupts_restore(saved);
        }
        return status;
    case JELLY_AUDIO_PLAYBACK_QUEUED:
        if (result)
            *result = device->playback_count;
        return STATUS_SUCCESS;
    case JELLY_AUDIO_CAPTURE_QUEUED:
        if (result)
            *result = device->capture_count;
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_ARGUMENT;
}

EXPORT_SYMBOL(audio_device_register);
EXPORT_SYMBOL(audio_playback_pull);
EXPORT_SYMBOL(audio_capture_push);
