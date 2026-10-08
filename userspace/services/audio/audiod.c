/*
 * audiod: the audio server (README section 38).
 *
 *     audio driver -> audio device -> audio server -> mixer -> applications
 *
 * The server owns the sound devices and offers the service "audio". Every
 * client stream is a shared ring (audio/client/protocol.h) in the client's
 * own format. A machine can have several sound devices (the sound card's
 * jacks, the loudspeakers of a monitor on HDMI or DisplayPort): the sound
 * goes out on one of them, the output, and recordings come from one, the
 * input, which may be the same or another. The output can be changed
 * while sound plays (AUDIO_SET_OUTPUT); the streams move along. A device
 * handle wakes the server whenever its card wants more data or has
 * recorded some:
 *
 *   playback: take what each active stream has, convert it to the card's
 *             rate (mixer.c), scale it by the stream volume, sum the
 *             streams, apply the master volume and queue the result
 *   capture:  convert what the card recorded for every capture stream
 *
 * The card only runs while it is needed: output stops shortly after the
 * last stream went silent, recording when the last capture stream closes.
 *
 * /etc/audio.conf may set the start volume ("volume=80", "muted=no") and
 * the output device by its number ("output=1"); without that the output is
 * the first device that can play, the input the first that can record.
 */

#include "audio/client/protocol.h"
#include "audio/mixer/mixer.h"

#include <jelly/os.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CLIENTS     16
#define FILL_PERIODS    4   /* frames kept queued in the device: 40 ms at 10 ms periods */
#define MIX_FRAMES_MAX  4096
#define IDLE_ROUNDS     15  /* empty mixing rounds before a stream counts as idle */
#define STOP_DELAY_NS   200000000ull

typedef struct {
    bool            used;
    jelly_handle_t  channel;
    uint32_t        direction;   /* 0: no stream yet */
    audio_ring_t   *ring;
    size_t          mapping;
    uint32_t        capacity, channels, rate; /* our copy: the client can write the ring's header */
    mix_resampler_t resampler;
    uint32_t        gain;
    bool            active;      /* playback: being mixed */
    uint32_t        idle_rounds;
    bool            draining, drain_armed;
    uint64_t        drain_target; /* device frame counter at which the stream's last frame is out */
} client_t;

/* A sound device in use. Output and input may be the same device: then they share its one handle. */
typedef struct {
    bool               open;
    jelly_handle_t     handle;
    jelly_audio_info_t card;
} endpoint_t;

static jelly_handle_t service_channel;
static endpoint_t out, in;
static uint32_t wanted_output = UINT32_MAX; /* from /etc/audio.conf */
static client_t clients[MAX_CLIENTS];
static uint32_t master_percent = 100;
static bool master_muted;
static bool playing, capturing;
static uint64_t stop_deadline;

static int32_t mix_sum[MIX_FRAMES_MAX * 2];
static int16_t mix_stream[MIX_FRAMES_MAX * 2], mix_out[MIX_FRAMES_MAX * 2];

static void log_message(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_message(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    printf("audiod: ");
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

static uint32_t load(const uint32_t *word)
{
    return __atomic_load_n(word, __ATOMIC_SEQ_CST);
}

static void store(uint32_t *word, uint32_t value)
{
    __atomic_store_n(word, value, __ATOMIC_SEQ_CST);
}

static void reply(client_t *client, audio_message_t *m, status_t status)
{
    m->status = (int32_t)status;
    jelly_channel_send(client->channel, m, sizeof(*m));
}

static uint64_t output_value(uint32_t command)
{
    uint64_t value = 0;
    jelly_audio_control(out.handle, command, 0, &value);
    return value;
}

/* --- Playback -------------------------------------------------------------------- */

/* Convert up to `frames` frames of a stream into mix_stream; returns the frames produced. */
static size_t take_from(client_t *client, size_t frames)
{
    audio_ring_t *ring = client->ring;
    uint32_t capacity = client->capacity, channels = client->channels;
    uint32_t read = ring->read, available = load(&ring->write) - read;
    size_t made = 0;

    if (available > capacity)
        available = 0; /* a client that scribbles over its counters gets silence */
    while (made < frames) {
        uint32_t index = read % capacity, span = capacity - index;
        size_t used;
        if (span > available)
            span = available;
        size_t got = mix_resample(&client->resampler, ring->samples + (size_t)index * channels, span, &used,
                                  mix_stream + made * 2, frames - made);
        read += (uint32_t)used;
        available -= (uint32_t)used;
        made += got;
        if (!used && !got)
            break;
    }
    if (read != ring->read) {
        store(&ring->read, read);
        if (load(&ring->waiting))
            jelly_futex_wake(&ring->read, 1);
    }
    return made;
}

/* Mix `frames` frames of all active streams and queue them in the device. */
static void mix(size_t frames)
{
    if (frames > MIX_FRAMES_MAX)
        frames = MIX_FRAMES_MAX;
    memset(mix_sum, 0, frames * 2 * sizeof(mix_sum[0]));
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &clients[i];
        if (!c->used || c->direction != AUDIO_PLAYBACK || !c->active)
            continue;
        size_t made = take_from(c, frames);
        mix_add(mix_sum, mix_stream, made * 2, c->gain);

        /* A stream that stays empty is not mixed any more until its client says AUDIO_START. */
        c->idle_rounds = made ? 0 : c->idle_rounds + 1;
        if (c->idle_rounds >= IDLE_ROUNDS && !c->draining) {
            store(&c->ring->active, 0);
            if (load(&c->ring->write) != c->ring->read)
                store(&c->ring->active, 1); /* data arrived at the last moment */
            else
                c->active = false;
            c->idle_rounds = 0;
        }
    }
    mix_output(mix_sum, mix_out, frames * 2, master_muted ? 0 : mix_gain(master_percent));
    size_t written;
    jelly_audio_write(out.handle, mix_out, frames, &written);
}

static void fill_device(void)
{
    uint64_t queued = output_value(JELLY_AUDIO_PLAYBACK_QUEUED), target = (uint64_t)out.card.period * FILL_PERIODS;
    if (queued < target)
        mix(target - queued);
}

static void start_stream(client_t *client)
{
    if (client->direction != AUDIO_PLAYBACK)
        return;
    client->active = true;
    client->idle_rounds = 0;
    store(&client->ring->active, 1);
    stop_deadline = 0;
    if (!playing) {
        fill_device(); /* the card starts with sound, not with a gap */
        status_t status = jelly_audio_control(out.handle, JELLY_AUDIO_PLAYBACK_ENABLE, 1, NULL);
        if (STATUS_IS_ERROR(status))
            log_message("cannot start the output: %s", status_name(status));
        playing = !STATUS_IS_ERROR(status);
    }
}

/* Answer AUDIO_DRAIN once the stream's last frame has left the card. */
static void check_drains(void)
{
    jelly_audio_info_t now;
    bool have_info = false;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &clients[i];
        if (!c->used || !c->draining)
            continue;
        if (load(&c->ring->write) != c->ring->read && playing)
            continue; /* still being mixed */
        if (!have_info) {
            jelly_audio_info(out.card.index, &now);
            have_info = true;
        }
        if (!c->drain_armed) {
            /* What is queued in the device and in the card's own buffer still has to play. */
            c->drain_target = now.played_frames + output_value(JELLY_AUDIO_PLAYBACK_QUEUED) + 2 * out.card.period;
            c->drain_armed = true;
        }
        if (!playing || now.played_frames >= c->drain_target) {
            audio_message_t done = { .type = AUDIO_DRAIN };
            c->draining = c->drain_armed = false;
            reply(c, &done, STATUS_SUCCESS);
        }
    }
}

static void pump_playback(void)
{
    bool needed = false;

    fill_device();
    check_drains();
    for (int i = 0; i < MAX_CLIENTS; i++)
        needed = needed || (clients[i].used && clients[i].direction == AUDIO_PLAYBACK &&
                            (clients[i].active || clients[i].draining));
    if (needed) {
        stop_deadline = 0;
    } else if (!stop_deadline) {
        stop_deadline = jelly_clock_ns() + STOP_DELAY_NS; /* let the tail play out */
    } else if (jelly_clock_ns() >= stop_deadline) {
        jelly_audio_control(out.handle, JELLY_AUDIO_PLAYBACK_ENABLE, 0, NULL);
        playing = false;
        stop_deadline = 0;
    }
}

/* --- Capture --------------------------------------------------------------------- */

static void give_to(client_t *client, const int16_t *frames, size_t count)
{
    static int16_t converted[MIX_FRAMES_MAX * 2];
    audio_ring_t *ring = client->ring;
    uint32_t capacity = client->capacity, channels = client->channels;
    size_t used, offset = 0;

    while (offset < count) {
        size_t made = mix_resample(&client->resampler, frames + offset * 2, count - offset, &used, converted,
                                   MIX_FRAMES_MAX);
        offset += used;
        if (!made && !used)
            break;
        if (channels == 1)
            mix_stereo_to_mono(converted, converted, made);

        uint32_t write = ring->write, fill = write - load(&ring->read);
        if (fill > capacity)
            fill = capacity;
        size_t space = capacity - fill, frame = channels * sizeof(int16_t);
        if (made > space)
            made = space; /* a reader that fell behind loses the newest frames */
        for (size_t done = 0; done < made;) {
            uint32_t index = (write + (uint32_t)done) % capacity;
            size_t run = capacity - index;
            if (run > made - done)
                run = made - done;
            memcpy((uint8_t *)ring->samples + index * frame, (uint8_t *)converted + done * frame, run * frame);
            done += run;
        }
        if (made) {
            store(&ring->write, write + (uint32_t)made);
            if (load(&ring->waiting))
                jelly_futex_wake(&ring->write, 1);
        }
    }
}

static void pump_capture(void)
{
    static int16_t recorded[480 * 2];
    size_t got;

    while (jelly_audio_read(in.handle, recorded, sizeof(recorded) / (2 * sizeof(int16_t)), &got) == STATUS_SUCCESS && got) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].used && clients[i].direction == AUDIO_CAPTURE)
                give_to(&clients[i], recorded, got);
        }
    }
}

static void update_capture(void)
{
    bool needed = false;
    for (int i = 0; i < MAX_CLIENTS; i++)
        needed = needed || (clients[i].used && clients[i].direction == AUDIO_CAPTURE);
    if (needed == capturing || !in.open)
        return;
    status_t status = jelly_audio_control(in.handle, JELLY_AUDIO_CAPTURE_ENABLE, needed, NULL);
    if (STATUS_IS_ERROR(status))
        log_message("cannot %s recording: %s", needed ? "start" : "stop", status_name(status));
    capturing = needed && !STATUS_IS_ERROR(status);
}

/* --- Devices --------------------------------------------------------------------- */

/* Can the mixer play on this device? (Stereo, and periods the mixing buffers hold.) */
static bool playable(const jelly_audio_info_t *card)
{
    return (card->flags & JELLY_AUDIO_PLAYBACK) && card->channels == 2 && card->period &&
           card->period * FILL_PERIODS <= MIX_FRAMES_MAX;
}

/*
 * The sound goes out on device `index` from now on. Streams that are playing go on there: their converters
 * are set to the new card's rate, and what was queued in the old card is lost (a fraction of a second).
 */
static status_t set_output(uint32_t index)
{
    jelly_audio_info_t card;
    jelly_handle_t handle;
    bool restart = false;

    if (STATUS_IS_ERROR(jelly_audio_info(index, &card)))
        return STATUS_NOT_FOUND;
    if (!playable(&card))
        return STATUS_NOT_SUPPORTED;
    if (out.open && out.card.index == index)
        return STATUS_SUCCESS;
    if (in.open && in.card.index == index) {
        handle = in.handle; /* a device has one handle */
    } else {
        status_t status = jelly_audio_open(index, &handle);
        if (STATUS_IS_ERROR(status))
            return status;
    }
    if (out.open) {
        if (playing)
            jelly_audio_control(out.handle, JELLY_AUDIO_PLAYBACK_ENABLE, 0, NULL);
        if (!(in.open && in.handle == out.handle))
            jelly_handle_close(out.handle);
    }
    playing = false;
    stop_deadline = 0;
    out = (endpoint_t){ true, handle, card };

    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_t *c = &clients[i];
        if (!c->used || c->direction != AUDIO_PLAYBACK)
            continue;
        mix_resampler_init(&c->resampler, c->rate, c->channels, out.card.rate);
        c->drain_armed = false; /* the new card counts its frames from elsewhere */
        restart = restart || c->active || c->draining;
    }
    if (restart) {
        fill_device();
        playing = !STATUS_IS_ERROR(jelly_audio_control(out.handle, JELLY_AUDIO_PLAYBACK_ENABLE, 1, NULL));
    }
    log_message("output: %s", out.card.name);
    return STATUS_SUCCESS;
}

/* At the start: the output from the configuration or the first device that plays; the first that records as input. */
static void choose_devices(void)
{
    jelly_audio_info_t card;

    if (wanted_output != UINT32_MAX && STATUS_IS_ERROR(set_output(wanted_output)))
        log_message("output=%u in /etc/audio.conf is not a device that can play", wanted_output);
    for (uint32_t i = 0; !out.open && jelly_audio_info(i, &card) == STATUS_SUCCESS; i++) {
        if (playable(&card))
            set_output(i);
    }
    for (uint32_t i = 0; !in.open && jelly_audio_info(i, &card) == STATUS_SUCCESS; i++) {
        if (!(card.flags & JELLY_AUDIO_CAPTURE))
            continue;
        if (out.open && out.card.index == i) {
            in = out;
        } else if (!STATUS_IS_ERROR(jelly_audio_open(i, &in.handle))) {
            in.card = card;
            in.open = true;
        }
    }
}

/* --- Clients --------------------------------------------------------------------- */

static void open_stream(client_t *client, audio_message_t *m)
{
    uint32_t direction = m->a, rate = m->b, channels = m->c, capacity = 1024;
    jelly_handle_t memory = JELLY_HANDLE_INVALID;
    void *address = NULL;
    status_t status = STATUS_SUCCESS;

    if (client->direction)
        status = STATUS_BUSY;
    else if ((direction != AUDIO_PLAYBACK && direction != AUDIO_CAPTURE) || rate < AUDIO_RATE_MIN ||
             rate > AUDIO_RATE_MAX || channels < 1 || channels > AUDIO_CHANNELS_MAX)
        status = STATUS_INVALID_ARGUMENT;
    else if (direction == AUDIO_PLAYBACK ? !out.open : !in.open)
        status = STATUS_NOT_SUPPORTED;
    /* Playback converts to the card's rate, capture from it. */
    if (!STATUS_IS_ERROR(status) &&
        !(direction == AUDIO_PLAYBACK ? mix_resampler_init(&client->resampler, rate, channels, out.card.rate)
                                      : mix_resampler_init(&client->resampler, in.card.rate, 2, rate)))
        status = STATUS_INVALID_ARGUMENT;

    /* About a quarter of a second, as a power of two (the counters wrap around cleanly). */
    while (capacity < rate / 4)
        capacity *= 2;
    size_t size = (sizeof(audio_ring_t) + (size_t)capacity * channels * sizeof(int16_t) + 4095) & ~(size_t)4095;
    if (!STATUS_IS_ERROR(status))
        status = jelly_shm_create(size, &memory);
    if (!STATUS_IS_ERROR(status))
        status = jelly_shm_map(memory, JELLY_MEMORY_WRITE, &address);
    if (STATUS_IS_ERROR(status)) {
        if (memory)
            jelly_handle_close(memory);
        reply(client, m, status);
        return;
    }

    audio_ring_t *ring = address;
    memset(ring, 0, sizeof(*ring));
    ring->capacity = capacity;
    ring->rate = rate;
    ring->channels = channels;
    ring->direction = direction;
    client->ring = ring;
    client->mapping = size;
    client->capacity = capacity;
    client->channels = channels;
    client->rate = rate;
    client->direction = direction;
    client->gain = MIX_GAIN_UNITY;
    client->active = client->draining = client->drain_armed = false;
    client->idle_rounds = 0;

    m->status = STATUS_SUCCESS;
    m->a = capacity;
    m->d = (uint32_t)size;
    /* The memory handle moves to the client; our mapping stays. */
    jelly_channel_send_handles(client->channel, m, sizeof(*m), &memory, 1);
    update_capture();
}

static void drop_client(client_t *client)
{
    if (client->ring) {
        store(&client->ring->closed, 1);
        jelly_memory_unmap(client->ring, client->mapping);
    }
    jelly_handle_close(client->channel);
    memset(client, 0, sizeof(*client));
    update_capture();
}

static uint32_t stream_count(void)
{
    uint32_t count = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        count += clients[i].used && clients[i].direction;
    return count;
}

static void serve_client(client_t *client)
{
    audio_message_t m;
    size_t size;

    for (;;) {
        status_t status = jelly_channel_receive(client->channel, &m, sizeof(m), &size);
        if (status == STATUS_WOULD_BLOCK)
            return;
        if (STATUS_IS_ERROR(status)) { /* the client closed its end */
            drop_client(client);
            return;
        }
        if (size != sizeof(m))
            continue;
        switch (m.type) {
        case AUDIO_OPEN:
            open_stream(client, &m);
            break;
        case AUDIO_START:
            start_stream(client);
            break;
        case AUDIO_SET_VOLUME:
            client->gain = mix_gain(m.a);
            reply(client, &m, client->direction ? STATUS_SUCCESS : STATUS_INVALID_ARGUMENT);
            break;
        case AUDIO_DRAIN:
            if (client->direction != AUDIO_PLAYBACK) {
                reply(client, &m, STATUS_INVALID_ARGUMENT);
            } else {
                client->draining = true;
                client->drain_armed = false;
                if (load(&client->ring->write) != client->ring->read)
                    start_stream(client);
                check_drains(); /* nothing to wait for? */
            }
            break;
        case AUDIO_SET_MASTER:
            master_percent = m.a > 100 ? 100 : m.a;
            master_muted = m.b != 0;
            /* fall through */
        case AUDIO_GET_MASTER:
            m.a = master_percent;
            m.b = master_muted;
            reply(client, &m, STATUS_SUCCESS);
            break;
        case AUDIO_GET_INFO: {
            const jelly_audio_info_t *card = out.open ? &out.card : &in.card;
            jelly_audio_info_t now = *card;
            jelly_audio_info(card->index, &now);
            m.a = card->rate;
            m.b = card->channels;
            m.c = stream_count();
            m.d = (uint32_t)now.underruns;
            memcpy(m.name, card->name, sizeof(m.name));
            reply(client, &m, STATUS_SUCCESS);
            break;
        }
        case AUDIO_GET_DEVICE: {
            jelly_audio_info_t card;
            uint32_t index = m.a;
            if (STATUS_IS_ERROR(jelly_audio_info(index, &card))) {
                reply(client, &m, STATUS_NOT_FOUND);
                break;
            }
            m.a = ((card.flags & JELLY_AUDIO_PLAYBACK) ? AUDIO_PLAYBACK : 0) |
                  ((card.flags & JELLY_AUDIO_CAPTURE) ? AUDIO_CAPTURE : 0);
            m.b = out.open && out.card.index == index;
            m.c = in.open && in.card.index == index;
            memcpy(m.name, card.name, sizeof(m.name));
            reply(client, &m, STATUS_SUCCESS);
            break;
        }
        case AUDIO_SET_OUTPUT:
            reply(client, &m, set_output(m.a));
            break;
        default:
            reply(client, &m, STATUS_NOT_SUPPORTED);
        }
    }
}

static void accept_clients(void)
{
    jelly_handle_t handles[JELLY_CHANNEL_MAX_HANDLES];
    uint32_t count = 0;
    char message[64];
    size_t size;

    while (jelly_channel_receive_handles(service_channel, message, sizeof(message), &size, handles, &count) ==
           STATUS_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            client_t *slot = NULL;
            for (int c = 0; c < MAX_CLIENTS && !slot; c++) {
                if (!clients[c].used)
                    slot = &clients[c];
            }
            if (!slot) {
                jelly_handle_close(handles[i]);
                log_message("too many clients");
                continue;
            }
            memset(slot, 0, sizeof(*slot));
            slot->used = true;
            slot->channel = handles[i];
        }
    }
}

static void load_config(void)
{
    char line[128];
    FILE *file = fopen("/etc/audio.conf", "r");

    if (!file)
        return;
    while (fgets(line, sizeof(line), file)) {
        if (!strncmp(line, "volume=", 7)) {
            int value = atoi(line + 7);
            master_percent = value < 0 ? 0 : value > 100 ? 100 : (uint32_t)value;
        } else if (!strncmp(line, "muted=", 6)) {
            master_muted = !strncmp(line + 6, "yes", 3);
        } else if (!strncmp(line, "output=", 7) && line[7] >= '0' && line[7] <= '9') {
            wanted_output = (uint32_t)atoi(line + 7);
        }
    }
    fclose(file);
}

int main(void)
{
    jelly_handle_t registry_end, handles[3 + MAX_CLIENTS];
    client_t *owners[3 + MAX_CLIENTS];

    jelly_audio_info_t first;

    if (STATUS_IS_ERROR(jelly_audio_info(0, &first))) {
        log_message("no audio device");
        return 0; /* nothing to serve: not a failure worth a restart */
    }
    load_config();
    choose_devices();
    if (!out.open && !in.open) {
        log_message("no audio device can be used (%s: %u channels, period %u)", first.name, first.channels, first.period);
        return 1;
    }
    if (STATUS_IS_ERROR(jelly_channel_create(&service_channel, &registry_end)) ||
        STATUS_IS_ERROR(jelly_service_register(AUDIO_SERVICE_NAME, registry_end))) {
        log_message("cannot register the service '%s'", AUDIO_SERVICE_NAME);
        return 1;
    }
    const jelly_audio_info_t *card = out.open ? &out.card : &in.card;
    log_message("%s, %u Hz, volume %u%%%s", card->name, card->rate, master_percent, master_muted ? " (muted)" : "");
    if (in.open && out.open && in.card.index != out.card.index)
        log_message("input: %s", in.card.name);

    for (;;) {
        uint32_t count = 0, index;
        handles[count++] = service_channel;
        /* A device is signaled when it wants data or has some. */
        if (playing)
            handles[count++] = out.handle;
        if (capturing && !(playing && in.handle == out.handle))
            handles[count++] = in.handle;
        uint32_t first_client = count;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].used) {
                owners[count] = &clients[i];
                handles[count++] = clients[i].channel;
            }
        }
        jelly_wait_many(handles, count, JELLY_WAIT_FOREVER, &index);

        accept_clients();
        for (uint32_t i = first_client; i < count; i++) {
            if (owners[i]->used)
                serve_client(owners[i]);
        }
        if (capturing)
            pump_capture();
        if (playing)
            pump_playback();
    }
}
