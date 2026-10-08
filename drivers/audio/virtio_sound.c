/*
 * VirtIO sound card (virtio-sound, PCI 1af4:1059): the sound card of
 * virtual machines, behind the same audio device interface as the HD Audio
 * driver (audio/device/audio_device.h).
 *
 * The card has streams, each one playing or recording, and four queues:
 *
 *   control (0)  requests with an answer: what the streams can do, a
 *                stream's format, prepare, start, stop, release
 *   events  (1)  jacks plugged and unplugged; not used
 *   tx      (2)  buffers of samples to play
 *   rx      (3)  empty buffers the card fills with what it recorded
 *
 * There is no clock to read and no position register. The card takes a
 * buffer from the queue, plays it at its sample rate and gives it back when
 * it is through; that return is the driver's interrupt, and the pace of the
 * sound. So a few periods are kept with the card at all times:
 *
 *   playback  PERIODS buffers of 10 ms are filled and queued at the start.
 *             Whenever one comes back it is filled with the next period
 *             and queued again
 *   capture   PERIODS empty buffers are queued; each one that comes back
 *             full is passed on and queued again
 *
 * A stream is set up anew for every start (format, prepare, start) and
 * given up at the stop (stop, release): the release makes the card hand
 * back the buffers it still holds, and the driver waits for them.
 *
 * The format is fixed, as for the HD Audio driver: 48 kHz, 16 bits, stereo.
 * The first stream of each direction that takes it is used.
 *
 * Tested with QEMU's virtio-sound-pci. Not yet: jacks, channel maps, other
 * formats, more than one stream per direction.
 */

#include "audio/device/audio_device.h"
#include "drivers/bus/virtio/virtio.h"
#include "drivers/core/device.h"
#include "drivers/core/module.h"

#include "core/arch.h"
#include "core/format.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "memory/layout.h"
#include "scheduler/mutex.h"
#include "scheduler/thread.h"
#include "scheduler/wait.h"

#define QUEUE_CONTROL 0
#define QUEUE_EVENTS  1
#define QUEUE_TX      2
#define QUEUE_RX      3
#define NO_INTERRUPT  0xFFFF

/* Device configuration: three numbers */
#define CONFIG_STREAMS 4

/* Requests */
#define REQUEST_PCM_INFO   0x0100
#define REQUEST_SET_PARAMS 0x0101
#define REQUEST_PREPARE    0x0102
#define REQUEST_RELEASE    0x0103
#define REQUEST_START      0x0104
#define REQUEST_STOP       0x0105
#define ANSWER_OK          0x8000

#define DIRECTION_OUTPUT   0
#define DIRECTION_INPUT    1
#define FORMAT_S16         5 /* bit in `formats`, and the value in a stream's parameters */
#define RATE_48000         7

#define RATE          48000
#define CHANNELS      2
#define PERIOD_FRAMES 480 /* 10 ms */
#define PERIOD_BYTES  (PERIOD_FRAMES * CHANNELS * sizeof(int16_t))
#define PERIODS       4   /* with the card at any time: 40 ms */
#define MAX_STREAMS   8

/* A buffer on its way to the card and back: which stream, the card's verdict, the samples. Three descriptors. */
#define SLOT_STATUS   8
#define SLOT_DATA     16
#define SLOT_BYTES    (SLOT_DATA + PERIOD_BYTES)

#define RESPONSE_AT        2048 /* in the page of requests */
#define COMMAND_TIMEOUT_NS 2000000000ull

typedef struct {
    uint32_t code;
    uint32_t start, count, size;
} snd_query_t;

typedef struct {
    uint32_t function; /* an HD Audio function group the stream belongs to, if it models one */
    uint32_t features;
    uint64_t formats, rates;
    uint8_t  direction, channels_min, channels_max;
    uint8_t  padding[5];
} snd_stream_info_t;

typedef struct {
    uint32_t code;
    uint32_t stream;
} snd_stream_request_t;

typedef struct {
    uint32_t code;
    uint32_t stream;
    uint32_t buffer_bytes, period_bytes;
    uint32_t features;
    uint8_t  channels, format, rate, padding;
} snd_parameters_t;

/* One direction: its stream of the card, its queue, its buffers. */
typedef struct {
    int           stream;  /* -1: the card has none for this direction */
    virtqueue_t  *queue;
    dma_buffer_t  memory;  /* PERIODS slots */
    bool          running; /* buffers that come back are queued again */
    uint32_t      away;    /* buffers the card holds */
    bool          capture;
} vsnd_side_t;

typedef struct {
    virtio_device_t virtio;
    virtqueue_t     control, events, tx, rx;
    uint32_t        irq;
    mutex_t         lock;       /* requests */
    wait_queue_t    completion; /* woken by every interrupt */
    bool            failed;     /* a request got no answer: nothing more is sent */
    dma_buffer_t    commands;
    vsnd_side_t     out, in;
    audio_device_t  audio;
} vsnd_t;

/* --- Requests ------------------------------------------------------------------------ */

/* Send what is in the page of requests and sleep until the card has answered. Lock held. */
static status_t command(vsnd_t *v, uint32_t request_bytes, uint32_t response_bytes)
{
    uint32_t *sent = v->commands.virt, *answer = (uint32_t *)((uint8_t *)v->commands.virt + RESPONSE_AT);

    if (v->failed)
        return STATUS_DEVICE_ERROR;
    *answer = 0;
    v->control.desc[0] = (virtq_desc_t){ v->commands.phys, request_bytes, VIRTQ_DESC_F_NEXT, 1 };
    v->control.desc[1] = (virtq_desc_t){ v->commands.phys + RESPONSE_AT, response_bytes, VIRTQ_DESC_F_WRITE, 0 };
    virtio_queue_submit(&v->control, 0);

    uint64_t deadline = wait_deadline(COMMAND_TIMEOUT_NS);
    status_t status = STATUS_SUCCESS;
    uint64_t flags = arch_interrupts_save();
    while (!virtio_queue_has_used(&v->control) && status == STATUS_SUCCESS)
        status = wait_queue_block_uninterruptible(&v->completion, deadline);
    arch_interrupts_restore(flags);
    if (status != STATUS_SUCCESS) {
        /* The card still holds the buffers: nothing more can be sent. */
        v->failed = true;
        klog_error("virtio-sound: request 0x%x got no answer: the card is not used any more", *sent);
        return STATUS_TIMEOUT;
    }
    virtio_queue_pop_used(&v->control);
    if (*answer != ANSWER_OK) {
        klog_warn("virtio-sound: request 0x%x failed (answer 0x%x)", *sent, *answer);
        return STATUS_DEVICE_ERROR;
    }
    return STATUS_SUCCESS;
}

/* A request that names a stream and nothing else. Lock held. */
static status_t stream_command(vsnd_t *v, uint32_t code, int stream)
{
    snd_stream_request_t *request = v->commands.virt;

    *request = (snd_stream_request_t){ code, (uint32_t)stream };
    return command(v, sizeof(*request), sizeof(uint32_t));
}

/* --- Buffers ------------------------------------------------------------------------- */

static int16_t *slot_samples(vsnd_side_t *side, uint32_t slot)
{
    return (int16_t *)((uint8_t *)side->memory.virt + slot * SLOT_BYTES + SLOT_DATA);
}

/* Slot `slot` to the card: the stream's number, the samples (to read or to fill), room for its verdict. */
static void slot_send(vsnd_side_t *side, uint32_t slot)
{
    uint64_t base = side->memory.phys + slot * SLOT_BYTES;
    uint16_t d = (uint16_t)(slot * 3);

    *(uint32_t *)((uint8_t *)side->memory.virt + slot * SLOT_BYTES) = (uint32_t)side->stream;
    side->queue->desc[d] = (virtq_desc_t){ base, sizeof(uint32_t), VIRTQ_DESC_F_NEXT, (uint16_t)(d + 1) };
    side->queue->desc[d + 1] = (virtq_desc_t){ base + SLOT_DATA, PERIOD_BYTES,
                                               VIRTQ_DESC_F_NEXT | (side->capture ? VIRTQ_DESC_F_WRITE : 0),
                                               (uint16_t)(d + 2) };
    side->queue->desc[d + 2] = (virtq_desc_t){ base + SLOT_STATUS, 2 * sizeof(uint32_t), VIRTQ_DESC_F_WRITE, 0 };
    virtio_queue_submit(side->queue, d);
}

/* Buffers the card is through with: the next period into each, or out of it, and back to the card. */
static void side_interrupt(vsnd_t *v, vsnd_side_t *side)
{
    uint32_t id, length;

    while (side->queue->desc && virtio_queue_next_used(side->queue, &id, &length)) {
        uint32_t slot = id / 3;
        if (slot >= PERIODS || id % 3)
            continue;
        if (!side->running) {
            side->away--; /* the stream was stopped: the buffer stays here */
            continue;
        }
        if (!side->capture)
            audio_playback_pull(&v->audio, slot_samples(side, slot), PERIOD_FRAMES);
        else if (length >= 2 * sizeof(uint32_t) + PERIOD_BYTES) /* (less: handed back early, at a stop) */
            audio_capture_push(&v->audio, slot_samples(side, slot), PERIOD_FRAMES);
        slot_send(side, slot);
    }
}

static void vsnd_interrupt(void *context)
{
    vsnd_t *v = context;

    wait_queue_wake_all(&v->completion, STATUS_SUCCESS); /* (command() looks at the control queue itself) */
    side_interrupt(v, &v->out);
    side_interrupt(v, &v->in);
}

/* --- Streams ------------------------------------------------------------------------- */

/* Wait until the card has given back every buffer of this side. */
static bool side_drained(vsnd_side_t *side)
{
    for (int i = 0; i < 500 && side->away; i++)
        thread_sleep(1000000);
    return side->away == 0;
}

static status_t side_stop(vsnd_t *v, vsnd_side_t *side)
{
    uint64_t flags = arch_interrupts_save();
    bool was_running = side->running;
    side->running = false;
    arch_interrupts_restore(flags);
    if (!was_running)
        return STATUS_SUCCESS;

    mutex_lock(&v->lock);
    stream_command(v, REQUEST_STOP, side->stream);
    status_t status = stream_command(v, REQUEST_RELEASE, side->stream); /* the card hands its buffers back */
    mutex_unlock(&v->lock);
    if (!side_drained(side))
        klog_warn("virtio-sound: the card keeps %u buffer%s of a stopped stream", side->away, side->away == 1 ? "" : "s");
    return status;
}

static status_t side_start(vsnd_t *v, vsnd_side_t *side)
{
    if (side->running)
        return STATUS_SUCCESS;
    if (!side_drained(side))
        return STATUS_BUSY; /* its buffers are still with the card */

    mutex_lock(&v->lock);
    snd_parameters_t *parameters = v->commands.virt;
    *parameters = (snd_parameters_t){ .code = REQUEST_SET_PARAMS, .stream = (uint32_t)side->stream,
                                      .buffer_bytes = PERIODS * PERIOD_BYTES, .period_bytes = PERIOD_BYTES,
                                      .channels = CHANNELS, .format = FORMAT_S16, .rate = RATE_48000 };
    status_t status = command(v, sizeof(*parameters), sizeof(uint32_t));
    if (!STATUS_IS_ERROR(status))
        status = stream_command(v, REQUEST_PREPARE, side->stream);
    if (!STATUS_IS_ERROR(status)) {
        /* The first periods are with the card before it starts. */
        uint64_t flags = arch_interrupts_save();
        for (uint32_t slot = 0; slot < PERIODS; slot++) {
            if (!side->capture)
                audio_playback_pull(&v->audio, slot_samples(side, slot), PERIOD_FRAMES);
            slot_send(side, slot);
        }
        side->away = PERIODS;
        side->running = true;
        arch_interrupts_restore(flags);
        status = stream_command(v, REQUEST_START, side->stream);
    }
    mutex_unlock(&v->lock);
    if (STATUS_IS_ERROR(status))
        side_stop(v, side);
    return status;
}

static status_t vsnd_playback_enable(audio_device_t *audio, bool enable)
{
    vsnd_t *v = audio->driver_data;
    return enable ? side_start(v, &v->out) : side_stop(v, &v->out);
}

static status_t vsnd_capture_enable(audio_device_t *audio, bool enable)
{
    vsnd_t *v = audio->driver_data;
    return enable ? side_start(v, &v->in) : side_stop(v, &v->in);
}

static const audio_device_ops_t vsnd_audio_ops = {
    .playback_enable = vsnd_playback_enable,
    .capture_enable = vsnd_capture_enable,
};

/* --- Start --------------------------------------------------------------------------- */

/* Ask what the streams are: the first of each direction that takes our format. Lock held. */
static status_t find_streams(vsnd_t *v)
{
    uint32_t streams = *(volatile uint32_t *)(v->virtio.device_config + CONFIG_STREAMS);
    snd_query_t *query = v->commands.virt;
    const snd_stream_info_t *info = (const snd_stream_info_t *)((uint8_t *)v->commands.virt + RESPONSE_AT + sizeof(uint32_t));

    v->out.stream = v->in.stream = -1;
    if (streams > MAX_STREAMS)
        streams = MAX_STREAMS;
    if (!streams)
        return STATUS_NOT_FOUND;
    *query = (snd_query_t){ REQUEST_PCM_INFO, 0, streams, sizeof(snd_stream_info_t) };
    status_t status = command(v, sizeof(*query), sizeof(uint32_t) + streams * sizeof(snd_stream_info_t));
    if (STATUS_IS_ERROR(status))
        return status;
    for (uint32_t i = 0; i < streams; i++) {
        bool fits = (info[i].formats & (1ull << FORMAT_S16)) && (info[i].rates & (1ull << RATE_48000)) &&
                    info[i].channels_min <= CHANNELS && info[i].channels_max >= CHANNELS;
        klog_debug("virtio-sound: stream %u: %s, formats 0x%lx, rates 0x%lx, %u to %u channels%s", i,
                   info[i].direction == DIRECTION_OUTPUT ? "plays" : "records", info[i].formats, info[i].rates,
                   info[i].channels_min, info[i].channels_max, fits ? "" : " (not ours)");
        if (!fits)
            continue;
        if (info[i].direction == DIRECTION_OUTPUT && v->out.stream < 0)
            v->out.stream = (int)i;
        else if (info[i].direction == DIRECTION_INPUT && v->in.stream < 0)
            v->in.stream = (int)i;
    }
    return v->out.stream >= 0 || v->in.stream >= 0 ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

static void release(vsnd_t *v, device_t *device)
{
    if (v->virtio.common)
        virtio_reset(&v->virtio);
    pci_disable_msix(v->virtio.pci);
    virtio_queue_free(&v->control);
    virtio_queue_free(&v->events);
    virtio_queue_free(&v->tx);
    virtio_queue_free(&v->rx);
    dma_free(&v->commands);
    dma_free(&v->out.memory);
    dma_free(&v->in.memory);
    kfree(v);
    device->driver_data = NULL;
}

static status_t vsnd_probe(device_t *device)
{
    pci_device_t *pci = pci_from_device(device);
    vsnd_t *v = kcalloc(1, sizeof(*v));

    if (!v)
        return STATUS_OUT_OF_MEMORY;
    device->driver_data = v;
    mutex_init(&v->lock);
    wait_queue_init(&v->completion);
    v->out.queue = &v->tx;
    v->in.queue = &v->rx;
    v->in.capture = true;

    /* One interrupt for the answers and for the buffers of both directions. */
    status_t status = virtio_init(&v->virtio, pci, 0, device);
    if (!STATUS_IS_ERROR(status))
        status = pci_enable_msix(pci, 0, vsnd_interrupt, v, &v->irq);
    if (!STATUS_IS_ERROR(status))
        status = virtio_queue_setup(&v->virtio, device, QUEUE_CONTROL, 16, 0, &v->control);
    if (!STATUS_IS_ERROR(status))
        status = virtio_queue_setup(&v->virtio, device, QUEUE_EVENTS, 16, NO_INTERRUPT, &v->events);
    if (!STATUS_IS_ERROR(status))
        status = virtio_queue_setup(&v->virtio, device, QUEUE_TX, 16, 0, &v->tx);
    if (!STATUS_IS_ERROR(status))
        status = virtio_queue_setup(&v->virtio, device, QUEUE_RX, 16, 0, &v->rx);
    if (!STATUS_IS_ERROR(status) && (v->tx.size < PERIODS * 3 || v->rx.size < PERIODS * 3 || v->control.size < 2))
        status = STATUS_NOT_SUPPORTED;
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, PAGE_SIZE, ~0ULL, &v->commands);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, PERIODS * SLOT_BYTES, ~0ULL, &v->out.memory);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, PERIODS * SLOT_BYTES, ~0ULL, &v->in.memory);
    if (!STATUS_IS_ERROR(status)) {
        virtio_driver_ok(&v->virtio);
        mutex_lock(&v->lock);
        status = find_streams(v);
        mutex_unlock(&v->lock);
    }
    if (!STATUS_IS_ERROR(status)) {
        format(v->audio.name, sizeof(v->audio.name), "VirtIO sound");
        v->audio.flags = (v->out.stream >= 0 ? JELLY_AUDIO_PLAYBACK : 0) | (v->in.stream >= 0 ? JELLY_AUDIO_CAPTURE : 0);
        v->audio.rate = RATE;
        v->audio.channels = CHANNELS;
        v->audio.period = PERIOD_FRAMES;
        v->audio.ops = &vsnd_audio_ops;
        v->audio.driver_data = v;
        status = audio_device_register(&v->audio);
    }
    if (STATUS_IS_ERROR(status)) {
        klog_warn("virtio-sound: the card cannot be used (%s)", status_name(status));
        release(v, device);
        return status;
    }
    klog_info("virtio-sound: %s%s%s at 48000 Hz, 16 bits, stereo", v->out.stream >= 0 ? "plays" : "",
              v->out.stream >= 0 && v->in.stream >= 0 ? " and " : "", v->in.stream >= 0 ? "records" : "");
    return STATUS_SUCCESS;
}

static const device_match_t vsnd_ids[] = {
    DEVICE_MATCH_ID(VIRTIO_VENDOR, 0x1059),
    DEVICE_MATCH_END,
};

static driver_t vsnd_driver = {
    .name = "virtio-sound",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_AUDIO,
    .ids = vsnd_ids,
    .probe = vsnd_probe,
};

static status_t vsnd_module_init(void)
{
    return driver_register(&vsnd_driver);
}

static const char *const vsnd_dependencies[] = { "pci", NULL };

MODULE(.name = "virtio_sound", .description = "VirtIO sound card", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 12, 0), .dependencies = vsnd_dependencies, .init = vsnd_module_init);
