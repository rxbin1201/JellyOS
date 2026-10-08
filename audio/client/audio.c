/*
 * Audio client library: streams to and from the audio server. See audio.h
 * and protocol.h.
 */

#include "audio/client/audio.h"

#include <jelly/os.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS 200000000ull /* how long to sleep on a full or empty ring before looking at the server again */

struct audio_stream {
    jelly_handle_t channel;
    audio_ring_t  *ring;
    size_t         mapping;
    uint32_t       direction;
};

static uint32_t load(const uint32_t *word)
{
    return __atomic_load_n(word, __ATOMIC_SEQ_CST);
}

static void store(uint32_t *word, uint32_t value)
{
    __atomic_store_n(word, value, __ATOMIC_SEQ_CST);
}

/* --- Messages -------------------------------------------------------------------- */

/* Send a request and wait for the reply of the same type. */
static status_t request(jelly_handle_t channel, audio_message_t *message, jelly_handle_t *handle)
{
    jelly_handle_t handles[JELLY_CHANNEL_MAX_HANDLES];
    uint32_t type = message->type, count;
    size_t size;

    status_t status = jelly_channel_send(channel, message, sizeof(*message));
    while (!STATUS_IS_ERROR(status)) {
        count = 0;
        status = jelly_channel_receive_handles(channel, message, sizeof(*message), &size, handles, &count);
        if (status == STATUS_WOULD_BLOCK) {
            status = jelly_wait(channel, JELLY_WAIT_FOREVER);
            continue;
        }
        if (STATUS_IS_ERROR(status))
            break;
        for (uint32_t i = 0; i < count; i++) {
            if (i == 0 && handle && size == sizeof(*message) && message->type == type)
                *handle = handles[0];
            else
                jelly_handle_close(handles[i]);
        }
        if (size == sizeof(*message) && message->type == type)
            return (status_t)message->status;
    }
    return status;
}

/* The server went away without telling us? */
static bool server_gone(audio_stream_t *stream)
{
    audio_message_t stray;
    size_t size;
    return jelly_channel_receive(stream->channel, &stray, sizeof(stray), &size) == STATUS_PEER_CLOSED;
}

/* --- Streams --------------------------------------------------------------------- */

status_t audio_open(uint32_t direction, uint32_t rate, uint32_t channels, audio_stream_t **out)
{
    audio_message_t m = { .type = AUDIO_OPEN, .a = direction, .b = rate, .c = channels };
    jelly_handle_t memory = JELLY_HANDLE_INVALID;
    void *address = NULL;

    audio_stream_t *stream = calloc(1, sizeof(*stream));
    if (!stream)
        return STATUS_OUT_OF_MEMORY;
    status_t status = jelly_service_connect(AUDIO_SERVICE_NAME, &stream->channel);
    if (STATUS_IS_ERROR(status)) {
        free(stream);
        return status;
    }
    status = request(stream->channel, &m, &memory);
    if (!STATUS_IS_ERROR(status))
        status = memory ? jelly_shm_map(memory, JELLY_MEMORY_WRITE, &address) : STATUS_IO_ERROR;
    if (memory)
        jelly_handle_close(memory); /* the mapping keeps the ring alive */
    if (STATUS_IS_ERROR(status)) {
        jelly_handle_close(stream->channel);
        free(stream);
        return status;
    }
    stream->ring = address;
    stream->mapping = m.d;
    stream->direction = direction;
    *out = stream;
    return STATUS_SUCCESS;
}

void audio_close(audio_stream_t *stream)
{
    if (!stream)
        return;
    jelly_handle_close(stream->channel); /* the server drops the stream */
    jelly_memory_unmap(stream->ring, stream->mapping);
    free(stream);
}

/* Sleep until the server moves `counter` away from `seen` (or a while). False if the stream is dead. */
static bool wait_for_server(audio_stream_t *stream, const uint32_t *counter, uint32_t seen)
{
    audio_ring_t *ring = stream->ring;
    status_t status = STATUS_SUCCESS;

    store(&ring->waiting, 1);
    if (load(counter) == seen && !load(&ring->closed))
        status = jelly_futex_wait(counter, seen, WAIT_NS);
    store(&ring->waiting, 0);
    if (status == STATUS_TIMEOUT && server_gone(stream))
        store(&ring->closed, 1);
    return !load(&ring->closed);
}

size_t audio_write(audio_stream_t *stream, const int16_t *frames, size_t count)
{
    audio_ring_t *ring = stream->ring;
    size_t done = 0, frame = ring->channels * sizeof(int16_t);

    if (stream->direction != AUDIO_PLAYBACK)
        return 0;
    while (done < count && !load(&ring->closed)) {
        uint32_t read = load(&ring->read), write = ring->write;
        uint32_t space = ring->capacity - (write - read);
        if (!space) {
            if (!wait_for_server(stream, &ring->read, read))
                break;
            continue;
        }
        uint32_t index = write % ring->capacity, n = space;
        if (n > count - done)
            n = (uint32_t)(count - done);
        if (n > ring->capacity - index)
            n = ring->capacity - index; /* up to the end of the ring; the rest in the next round */
        memcpy((uint8_t *)ring->samples + index * frame, (const uint8_t *)frames + done * frame, n * frame);
        store(&ring->write, write + n);
        done += n;
        /* The server stops mixing a stream that stays empty; tell it there is data again. */
        if (!load(&ring->active)) {
            audio_message_t start = { .type = AUDIO_START };
            if (STATUS_IS_ERROR(jelly_channel_send(stream->channel, &start, sizeof(start))))
                break;
        }
    }
    return done;
}

size_t audio_read(audio_stream_t *stream, int16_t *frames, size_t count)
{
    audio_ring_t *ring = stream->ring;
    size_t done = 0, frame = ring->channels * sizeof(int16_t);

    if (stream->direction != AUDIO_CAPTURE)
        return 0;
    while (done < count) {
        uint32_t write = load(&ring->write), read = ring->read;
        uint32_t available = write - read;
        if (!available) {
            if (!wait_for_server(stream, &ring->write, write))
                break;
            continue;
        }
        uint32_t index = read % ring->capacity, n = available;
        if (n > count - done)
            n = (uint32_t)(count - done);
        if (n > ring->capacity - index)
            n = ring->capacity - index;
        memcpy((uint8_t *)frames + done * frame, (const uint8_t *)ring->samples + index * frame, n * frame);
        store(&ring->read, read + n);
        done += n;
    }
    return done;
}

size_t audio_available(audio_stream_t *stream)
{
    audio_ring_t *ring = stream->ring;
    uint32_t fill = load(&ring->write) - load(&ring->read);
    return stream->direction == AUDIO_PLAYBACK ? ring->capacity - fill : fill;
}

status_t audio_drain(audio_stream_t *stream)
{
    audio_message_t m = { .type = AUDIO_DRAIN };
    return request(stream->channel, &m, NULL);
}

status_t audio_set_volume(audio_stream_t *stream, uint32_t percent)
{
    audio_message_t m = { .type = AUDIO_SET_VOLUME, .a = percent };
    return request(stream->channel, &m, NULL);
}

/* --- Server-wide requests ---------------------------------------------------------- */

static status_t server_request(audio_message_t *m)
{
    jelly_handle_t channel;
    status_t status = jelly_service_connect(AUDIO_SERVICE_NAME, &channel);
    if (STATUS_IS_ERROR(status))
        return status;
    status = request(channel, m, NULL);
    jelly_handle_close(channel);
    return status;
}

status_t audio_get_master(uint32_t *percent, bool *muted)
{
    audio_message_t m = { .type = AUDIO_GET_MASTER };
    status_t status = server_request(&m);
    if (!STATUS_IS_ERROR(status)) {
        if (percent)
            *percent = m.a;
        if (muted)
            *muted = m.b != 0;
    }
    return status;
}

status_t audio_set_master(uint32_t percent, bool muted)
{
    audio_message_t m = { .type = AUDIO_SET_MASTER, .a = percent, .b = muted };
    return server_request(&m);
}

status_t audio_get_info(audio_info_t *info)
{
    audio_message_t m = { .type = AUDIO_GET_INFO };
    status_t status = server_request(&m);
    if (!STATUS_IS_ERROR(status)) {
        info->rate = m.a;
        info->channels = m.b;
        info->streams = m.c;
        info->underruns = m.d;
        memcpy(info->name, m.name, sizeof(info->name));
        info->name[sizeof(info->name) - 1] = '\0';
    }
    return status;
}

status_t audio_get_device(uint32_t index, audio_device_entry_t *entry)
{
    audio_message_t m = { .type = AUDIO_GET_DEVICE, .a = index };
    status_t status = server_request(&m);
    if (!STATUS_IS_ERROR(status)) {
        entry->directions = m.a;
        entry->output = m.b;
        entry->input = m.c;
        memcpy(entry->name, m.name, sizeof(entry->name));
        entry->name[sizeof(entry->name) - 1] = '\0';
    }
    return status;
}

status_t audio_set_output(uint32_t index)
{
    audio_message_t m = { .type = AUDIO_SET_OUTPUT, .a = index };
    return server_request(&m);
}
