/*
 * Kernel tests for Phase 11, audio: the device layer's rings with a driver
 * that exists only in this file, and the sound card drivers (HD Audio,
 * VirtIO sound) playing and recording in real time (QEMU's cards with the
 * "none" backend).
 *
 * The audio server and the tools on top are exercised by the integration
 * test, which checks the sound QEMU writes to a WAV file.
 */

#include "tests/kernel/ktest.h"

#include "audio/device/audio_device.h"
#include "core/log.h"
#include "core/string.h"
#include "scheduler/thread.h"
#include "time/clock.h"

/* --- The device layer with a fake driver ------------------------------------------- */

static int fake_playback_calls, fake_capture_calls;

static status_t fake_playback(audio_device_t *device, bool enable)
{
    (void)device;
    fake_playback_calls += enable ? 1 : 100;
    return STATUS_SUCCESS;
}

static status_t fake_capture(audio_device_t *device, bool enable)
{
    (void)device;
    fake_capture_calls += enable ? 1 : 100;
    return STATUS_SUCCESS;
}

static const audio_device_ops_t fake_ops = { fake_playback, fake_capture };

static audio_device_t fake = {
    .name = "ktest audio",
    .flags = JELLY_AUDIO_PLAYBACK | JELLY_AUDIO_CAPTURE,
    .rate = 8000,
    .channels = 2,
    .period = 100,
    .ops = &fake_ops,
};

KTEST(audio_device_rings)
{
    static int16_t frames[1000 * 2], out[100 * 2];
    object_t *handle, *second;
    jelly_audio_info_t info;
    uint64_t queued;

    KASSERT(audio_device_register(&fake) == STATUS_SUCCESS);
    KASSERT(audio_device_get(fake.index) == &fake);
    audio_device_info(&fake, &info);
    KEXPECT(info.rate == 8000 && info.period == 100 && info.buffer == 100 * AUDIO_RING_PERIODS);
    KEXPECT(strcmp(info.name, "ktest audio") == 0);

    /* One user at a time */
    KASSERT(audio_device_open(fake.index, &handle) == STATUS_SUCCESS);
    KEXPECT(audio_device_open(fake.index, &second) == STATUS_BUSY);
    KEXPECT(audio_device_open(99, &second) == STATUS_NOT_FOUND);
    KEXPECT(!handle->ops->signaled(handle)); /* nothing runs yet */

    /* Playback: frames come out in order; the ring takes what fits */
    for (int i = 0; i < 1000; i++)
        frames[2 * i] = frames[2 * i + 1] = (int16_t)i;
    KEXPECT(audio_device_write(&fake, frames, 250) == 250);
    KEXPECT(audio_device_control(&fake, JELLY_AUDIO_PLAYBACK_ENABLE, 1, NULL) == STATUS_SUCCESS);
    KEXPECT(fake_playback_calls == 1 && fake.playing);
    KEXPECT(!handle->ops->signaled(handle)); /* 250 queued: above the low-water mark of two periods */

    audio_playback_pull(&fake, out, 100);
    KEXPECT(out[0] == 0 && out[1] == 0 && out[198] == 99);
    KEXPECT(handle->ops->signaled(handle)); /* 150 left: the device wants more */
    KEXPECT(audio_device_write(&fake, frames + 2 * 250, 750) == 650); /* the ring holds 800 */
    KEXPECT(audio_device_control(&fake, JELLY_AUDIO_PLAYBACK_QUEUED, 0, &queued) == STATUS_SUCCESS && queued == 800);
    KEXPECT(!handle->ops->signaled(handle));
    audio_playback_pull(&fake, out, 100);
    KEXPECT(out[0] == 100 && out[198] == 199);
    for (int i = 0; i < 6; i++)
        audio_playback_pull(&fake, out, 100); /* across the end of the ring */
    KEXPECT(out[0] == 700 && out[198] == 799 && fake.underruns == 0);

    /* The ring runs dry: the rest of the period is silence and counts as an underrun */
    audio_playback_pull(&fake, out, 100);
    KEXPECT(out[0] == 800 && out[198] == 899);
    KEXPECT(audio_device_write(&fake, frames, 30) == 30);
    out[100] = 77;
    audio_playback_pull(&fake, out, 100);
    KEXPECT(out[58] == 29 && out[60] == 0 && out[100] == 0 && fake.underruns == 1);
    audio_device_info(&fake, &info);
    KEXPECT(info.played_frames == 1000 && info.underruns == 1);

    /* Capture: pushed periods can be read; what nobody reads in time is dropped, oldest first */
    audio_capture_push(&fake, frames, 100); /* not recording: ignored */
    KEXPECT(audio_device_read(&fake, out, 100) == 0);
    KEXPECT(audio_device_control(&fake, JELLY_AUDIO_CAPTURE_ENABLE, 1, NULL) == STATUS_SUCCESS && fake_capture_calls == 1);
    audio_capture_push(&fake, frames, 100);
    KEXPECT(handle->ops->signaled(handle));
    KEXPECT(audio_device_read(&fake, out, 60) == 60 && out[0] == 0 && out[118] == 59);
    KEXPECT(audio_device_read(&fake, out, 100) == 40 && out[0] == 60 && out[78] == 99);
    for (int i = 0; i < 9; i++)
        audio_capture_push(&fake, frames + 2 * 100 * i, 100); /* 900 frames into a ring of 800 */
    KEXPECT(fake.overruns == 1);
    KEXPECT(audio_device_read(&fake, out, 100) == 100 && out[0] == 100 && out[198] == 199);

    /* Closing the handle stops both directions and frees the device for the next user */
    object_release(handle);
    KEXPECT(!fake.playing && !fake.capturing && fake_playback_calls == 101 && fake_capture_calls == 101);
    KEXPECT(audio_device_control(&fake, JELLY_AUDIO_PLAYBACK_QUEUED, 0, &queued) == STATUS_SUCCESS && queued == 0);
    KASSERT(audio_device_open(fake.index, &handle) == STATUS_SUCCESS);
    object_release(handle);
}

/* --- The sound cards --------------------------------------------------------------- */

static void card_plays_and_records(audio_device_t *card)
{
    static int16_t frames[256 * 2];
    jelly_audio_info_t before, after;
    object_t *handle;

    klog_info("ktest: sound card %u: %s", card->index, card->name);
    KASSERT(audio_device_open(card->index, &handle) == STATUS_SUCCESS);
    KEXPECT(card->rate == 48000 && card->channels == 2 && (card->flags & JELLY_AUDIO_PLAYBACK));

    /* Half a second of output: the card must take frames at its sample rate. */
    for (int i = 0; i < 256; i++)
        frames[2 * i] = frames[2 * i + 1] = (int16_t)((i % 64) * 256 - 8192);
    audio_device_info(card, &before);
    audio_device_write(card, frames, 256);
    uint64_t start = clock_monotonic_ns();
    KASSERT(audio_device_control(card, JELLY_AUDIO_PLAYBACK_ENABLE, 1, NULL) == STATUS_SUCCESS);
    for (int i = 0; i < 50; i++) {
        if (handle->ops->signaled(handle))
            audio_device_write(card, frames, 256);
        thread_sleep(10000000);
    }
    audio_device_info(card, &after);
    /* A few periods are fetched ahead when the stream starts (two by HD Audio, four by VirtIO sound). */
    uint64_t played = after.played_frames - before.played_frames;
    uint64_t expected = (clock_monotonic_ns() - start) * 48 / 1000000 + 2 * card->period;
    klog_info("ktest: sound card played %lu frames, %lu expected", played, expected);
    KEXPECT(played >= expected * 7 / 10 && played <= expected * 13 / 10);
    KEXPECT(audio_device_control(card, JELLY_AUDIO_PLAYBACK_ENABLE, 0, NULL) == STATUS_SUCCESS);
    audio_device_info(card, &before);
    thread_sleep(50000000);
    audio_device_info(card, &after);
    KEXPECT(after.played_frames == before.played_frames); /* stopped */

    if (card->flags & JELLY_AUDIO_CAPTURE) {
        size_t recorded = 0;
        start = clock_monotonic_ns();
        KASSERT(audio_device_control(card, JELLY_AUDIO_CAPTURE_ENABLE, 1, NULL) == STATUS_SUCCESS);
        for (int i = 0; i < 30; i++) {
            thread_sleep(10000000);
            recorded += audio_device_read(card, frames, 256);
            recorded += audio_device_read(card, frames, 256);
        }
        expected = (clock_monotonic_ns() - start) * 48 / 1000000;
        klog_info("ktest: sound card recorded %lu frames, %lu expected", recorded, expected);
        KEXPECT(recorded >= expected * 7 / 10 && recorded <= expected * 13 / 10);
        KEXPECT(audio_device_control(card, JELLY_AUDIO_CAPTURE_ENABLE, 0, NULL) == STATUS_SUCCESS);
    }
    object_release(handle);
    KEXPECT(!card->playing && !card->capturing);
}

KTEST(audio_cards_play_and_record)
{
    uint32_t cards = 0;

    for (uint32_t i = 0; i < audio_device_count(); i++) {
        audio_device_t *card = audio_device_get(i);
        if (!card || card == &fake)
            continue;
        cards++;
        card_plays_and_records(card);
    }
    if (!cards)
        klog_info("ktest: no sound card, skipped");
}
