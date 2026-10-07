/*
 * System calls of ABI version 7: audio devices (for the audio server).
 */

#include "syscall/internal.h"

#include "audio/device/audio_device.h"
#include "process/process.h"
#include "process/usercopy.h"

#include <jelly/syscall.h>

#define CHUNK_FRAMES 256
#define CHUNK_SAMPLES (CHUNK_FRAMES * 2)

status_t sys_audio_info(const uint64_t *a)
{
    jelly_audio_info_t info;
    audio_device_t *device = audio_device_get((uint32_t)a[0]);

    if (!device)
        return STATUS_NOT_FOUND;
    audio_device_info(device, &info);
    return copy_to_user(a[1], &info, sizeof(info));
}

status_t sys_audio_open(const uint64_t *a)
{
    object_t *object;

    if (process_current()->credentials.uid != UID_ROOT)
        return STATUS_ACCESS_DENIED;
    if (!user_range_ok(a[1], sizeof(jelly_handle_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = audio_device_open((uint32_t)a[0], &object);
    if (STATUS_IS_ERROR(status))
        return status;
    return syscall_give_handle(object, JELLY_RIGHT_READ | JELLY_RIGHT_WRITE | JELLY_RIGHT_WAIT, a[1]);
}

/* Frames move through a small kernel buffer: the rings are only touched with interrupts off. */
static status_t transfer(const uint64_t *a, bool write)
{
    int16_t chunk[CHUNK_SAMPLES];
    object_t *object;
    size_t count = a[2], done = 0;

    if (!user_range_ok(a[3], sizeof(uint64_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = handle_get(syscall_handles(), (handle_t)a[0], OBJECT_AUDIO,
                                 write ? JELLY_RIGHT_WRITE : JELLY_RIGHT_READ, &object, NULL);
    if (STATUS_IS_ERROR(status))
        return status;
    audio_device_t *device = audio_device_of(object);
    size_t frame = device->channels * sizeof(int16_t);
    size_t chunk_frames = sizeof(chunk) / frame;

    while (done < count && chunk_frames) {
        size_t n = count - done < chunk_frames ? count - done : chunk_frames, moved;
        if (write) {
            status = copy_from_user(chunk, a[1] + done * frame, n * frame);
            if (STATUS_IS_ERROR(status))
                break;
            moved = audio_device_write(device, chunk, n);
        } else {
            if (!user_range_ok(a[1] + done * frame, n * frame, true)) {
                status = STATUS_INVALID_ARGUMENT;
                break;
            }
            moved = audio_device_read(device, chunk, n);
            copy_to_user(a[1] + done * frame, chunk, moved * frame); /* checked above */
        }
        done += moved;
        if (moved < n)
            break; /* ring full / empty */
    }
    object_release(object);
    /* Frames already moved count even if a later part of the buffer was bad. */
    if (STATUS_IS_ERROR(status) && !done)
        return status;
    return put_user_u64(a[3], done);
}

status_t sys_audio_write(const uint64_t *a)
{
    return transfer(a, true);
}

status_t sys_audio_read(const uint64_t *a)
{
    return transfer(a, false);
}

status_t sys_audio_control(const uint64_t *a)
{
    object_t *object;
    uint64_t result = 0;

    if (a[3] && !user_range_ok(a[3], sizeof(uint64_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = handle_get(syscall_handles(), (handle_t)a[0], OBJECT_AUDIO, JELLY_RIGHT_WRITE, &object, NULL);
    if (STATUS_IS_ERROR(status))
        return status;
    status = audio_device_control(audio_device_of(object), (uint32_t)a[1], a[2], &result);
    object_release(object);
    if (!STATUS_IS_ERROR(status) && a[3])
        status = put_user_u64(a[3], result);
    return status;
}
