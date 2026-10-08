/*
 * VirtIO GPU (2D): the paravirtual graphics card of QEMU and other virtual
 * machines ("virtio-vga", PCI 1af4:1050).
 *
 * The card differs from real ones in one thing that shapes this driver: it
 * has no framebuffer the host looks at. The picture lives in a "resource"
 * on the host. The guest keeps the pixels in its own memory (the resource's
 * "backing"), and says when to copy them over (TRANSFER_TO_HOST_2D) and to
 * show them (RESOURCE_FLUSH). Commands and their answers travel over a
 * virtqueue.
 *
 * Behind the display layer's interface (drivers/graphics/display.h) it
 * looks like any other card:
 *
 *   framebuffers    two, in guest memory, both the backing of one resource
 *   presenting      a thread copies the framebuffer on the screen to the
 *                   host 60 times a second: the card's "vertical blank"
 *   flip            the other framebuffer is copied from the next frame on.
 *                   The host only ever sees whole frames
 *   wait_vblank     until the next frame has been presented
 *   pointer         a second, small resource, placed by the host (the
 *                   cursor queue): no drawing in the framebuffer
 *   set_mode        a resource of the new size with the same backing; the
 *                   framebuffers do not move
 *   panic           one last frame, sent without the thread and without
 *                   interrupts: the device is polled for its answers
 *
 * As virtio-vga the card is also a VGA card: the firmware shows its picture
 * through that side, and the boot framebuffer is the VGA memory. With the
 * first command that sets a scanout the host switches over to the VirtIO
 * side, until the device is reset.
 *
 * A change of the host's window (another preferred size, the output gone or
 * back) arrives as an event in the device configuration; it is looked at
 * once a second and passed on like another monitor: a new list of modes.
 *
 * Not here: 3D (virgl), several scanouts, and a card without the VGA side
 * (virtio-gpu-pci), for which the firmware gives no boot framebuffer.
 */

#include "drivers/bus/virtio/virtio.h"
#include "drivers/core/device.h"
#include "drivers/core/module.h"
#include "drivers/graphics/display.h"

#include "core/arch.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/layout.h"
#include "memory/vmm.h"
#include "scheduler/mutex.h"
#include "scheduler/thread.h"
#include "scheduler/wait.h"
#include "time/clock.h"

#define VIRTIO_GPU_DEVICE       0x1050

/* Device configuration */
#define CONFIG_EVENTS_READ      0
#define CONFIG_EVENTS_CLEAR     4
#define CONFIG_SCANOUTS         8
#define EVENT_DISPLAY           1u

/* Commands and answers */
#define CMD_GET_DISPLAY_INFO    0x0100
#define CMD_RESOURCE_CREATE_2D  0x0101
#define CMD_RESOURCE_UNREF      0x0102
#define CMD_SET_SCANOUT         0x0103
#define CMD_RESOURCE_FLUSH      0x0104
#define CMD_TRANSFER_TO_HOST_2D 0x0105
#define CMD_ATTACH_BACKING      0x0106
#define CMD_UPDATE_CURSOR       0x0300
#define CMD_MOVE_CURSOR         0x0301
#define RESPONSE_OK             0x1100 /* 0x1100-0x11FF: done, some with data */
#define RESPONSE_ERROR          0x1200

/* Pixel formats, named after the order of the bytes in memory */
#define FORMAT_B8G8R8A8         1
#define FORMAT_B8G8R8X8         2
#define FORMAT_R8G8B8X8         134

#define MAX_SCANOUTS            16
#define QUEUE_CONTROL           0
#define QUEUE_CURSOR            1
#define CURSOR_SLOTS            16
#define NO_INTERRUPT            0xFFFF

/* The command buffer (one page): a request, the cursor queue's commands, an answer */
#define CURSOR_AT               1024
#define CURSOR_SLOT_BYTES       64
#define RESPONSE_AT             2048

#define POINTER_RESOURCE        1 /* the screen's resources are numbered from 2 on */
#define REFRESH_MHZ             60000
#define FRAME_NS                (1000000000ull * 1000 / REFRESH_MHZ)
#define COMMAND_TIMEOUT_NS      2000000000ull
#define PANIC_SPINS             20000000u /* how long a panic waits for an answer of the device */
#define LARGEST_WIDTH           1920 /* the framebuffers hold this, or the firmware's or host's size if larger */
#define LARGEST_HEIGHT          1080

typedef struct __attribute__((packed)) {
    uint32_t type, flags;
    uint64_t fence;
    uint32_t context;
    uint8_t  ring, padding[3];
} gpu_header_t;

typedef struct __attribute__((packed)) {
    uint32_t x, y, width, height;
} gpu_rect_t;

typedef struct __attribute__((packed)) {
    gpu_header_t header;
    struct __attribute__((packed)) {
        gpu_rect_t rect;
        uint32_t   enabled, flags;
    } outputs[MAX_SCANOUTS];
} gpu_display_info_t;

typedef struct __attribute__((packed)) {
    gpu_header_t header;
    uint32_t     resource, format, width, height;
} gpu_create_t;

typedef struct __attribute__((packed)) {
    gpu_header_t header;
    uint32_t     resource, padding;
} gpu_unref_t;

typedef struct __attribute__((packed)) {
    gpu_header_t header;
    gpu_rect_t   rect;
    uint32_t     scanout, resource;
} gpu_scanout_t;

typedef struct __attribute__((packed)) {
    gpu_header_t header;
    gpu_rect_t   rect;
    uint32_t     resource, padding;
} gpu_flush_t;

typedef struct __attribute__((packed)) {
    gpu_header_t header;
    gpu_rect_t   rect;
    uint64_t     offset; /* into the backing: where the rectangle's first pixel is */
    uint32_t     resource, padding;
} gpu_transfer_t;

typedef struct __attribute__((packed)) {
    gpu_header_t header;
    uint32_t     resource, entries;
    struct __attribute__((packed)) {
        uint64_t address;
        uint32_t length, padding;
    } entry[2];
} gpu_backing_t;

typedef struct __attribute__((packed)) {
    gpu_header_t header;
    uint32_t     scanout, x, y, padding;
    uint32_t     resource, hot_x, hot_y, padding2;
} gpu_cursor_t;

typedef struct {
    virtio_device_t virtio;
    virtqueue_t     control, cursor;
    uint32_t        irq;
    dma_buffer_t    commands;
    mutex_t         lock;       /* one command at a time on the control queue, and everything below */
    wait_queue_t    completion; /* the control queue's interrupt */
    bool            failed;     /* a command got no answer: the queue is not used any more */
    bool            in_flight;  /* a command is with the device */

    /* The screen */
    dma_buffer_t    memory;       /* the framebuffers, one after the other */
    uint64_t        buffer_bytes; /* one of them */
    uint32_t        buffers;      /* 2, or 1 if there was no memory for flipping */
    uint32_t        format;
    uint32_t        screen;       /* the resource on the scanout */
    uint32_t        next_resource;
    uint32_t        width, height;
    uint32_t        front;        /* the framebuffer that is presented */
    bool            flip_pending; /* `front` changed and has not been presented yet */
    uint64_t        frames;       /* presented so far */
    wait_queue_t    frame;        /* woken after each one */

    /* What the host says about its output */
    uint32_t        host_width, host_height;
    bool            host_enabled;

    /* The pointer */
    dma_buffer_t    pointer;      /* its image: the backing of POINTER_RESOURCE */
    uint32_t        cursor_free;  /* bit i: slot i of the cursor queue is free */
    bool            pointer_shown, pointer_new;
    uint32_t        hot_x, hot_y;
} vgpu_t;

/* --- Commands ------------------------------------------------------------------------ */

static void vgpu_interrupt(void *context)
{
    vgpu_t *g = context;

    wait_queue_wake_all(&g->completion, STATUS_SUCCESS);
}

/* The request buffer, zeroed, with its header filled in. Lock held. */
static void *request(vgpu_t *g, uint32_t type, size_t bytes)
{
    gpu_header_t *header = g->commands.virt;

    memset(header, 0, bytes);
    header->type = type;
    return header;
}

/* Send the request and sleep until the device has answered. Lock held. */
static status_t command(vgpu_t *g, uint32_t request_bytes, uint32_t response_bytes)
{
    gpu_header_t *sent = g->commands.virt, *response = (gpu_header_t *)((uint8_t *)g->commands.virt + RESPONSE_AT);

    if (g->failed)
        return STATUS_DEVICE_ERROR;
    response->type = 0;
    g->control.desc[0] = (virtq_desc_t){ g->commands.phys, request_bytes, VIRTQ_DESC_F_NEXT, 1 };
    g->control.desc[1] = (virtq_desc_t){ g->commands.phys + RESPONSE_AT, response_bytes, VIRTQ_DESC_F_WRITE, 0 };
    g->in_flight = true;
    virtio_queue_submit(&g->control, 0);

    uint64_t deadline = wait_deadline(COMMAND_TIMEOUT_NS);
    status_t status = STATUS_SUCCESS;
    uint64_t flags = arch_interrupts_save();
    while (!virtio_queue_has_used(&g->control) && status == STATUS_SUCCESS)
        status = wait_queue_block_uninterruptible(&g->completion, deadline);
    arch_interrupts_restore(flags);
    if (status != STATUS_SUCCESS) {
        /* The device still holds the buffers: nothing more can be sent. */
        g->failed = true;
        klog_error("virtio-gpu: command 0x%x got no answer: the card is not used any more", sent->type);
        return STATUS_TIMEOUT;
    }
    virtio_queue_pop_used(&g->control);
    g->in_flight = false;
    if (response->type < RESPONSE_OK || response->type >= RESPONSE_ERROR) {
        klog_warn("virtio-gpu: command 0x%x failed (answer 0x%x)", sent->type, response->type);
        return STATUS_DEVICE_ERROR;
    }
    return STATUS_SUCCESS;
}

static status_t simple(vgpu_t *g, uint32_t request_bytes)
{
    return command(g, request_bytes, sizeof(gpu_header_t));
}

/* A resource of width x height whose pixels are read from `entries` pieces of guest memory. Lock held. */
static status_t resource_create(vgpu_t *g, uint32_t id, uint32_t format, uint32_t width, uint32_t height,
                                uint64_t address, uint32_t bytes, uint32_t entries)
{
    gpu_create_t *create = request(g, CMD_RESOURCE_CREATE_2D, sizeof(*create));
    create->resource = id;
    create->format = format;
    create->width = width;
    create->height = height;
    status_t status = simple(g, sizeof(*create));
    if (STATUS_IS_ERROR(status))
        return status;

    gpu_backing_t *backing = request(g, CMD_ATTACH_BACKING, sizeof(*backing));
    backing->resource = id;
    backing->entries = entries;
    for (uint32_t i = 0; i < entries; i++) {
        backing->entry[i].address = address + (uint64_t)i * bytes;
        backing->entry[i].length = bytes;
    }
    status = simple(g, (uint32_t)(sizeof(gpu_header_t) + 8 + entries * sizeof(backing->entry[0])));
    if (STATUS_IS_ERROR(status)) {
        gpu_unref_t *unref = request(g, CMD_RESOURCE_UNREF, sizeof(*unref));
        unref->resource = id;
        simple(g, sizeof(*unref));
    }
    return status;
}

static status_t resource_unref(vgpu_t *g, uint32_t id)
{
    gpu_unref_t *unref = request(g, CMD_RESOURCE_UNREF, sizeof(*unref));

    unref->resource = id;
    return simple(g, sizeof(*unref));
}

/* Scanout 0 shows `resource` (0: nothing). Lock held. */
static status_t scanout_set(vgpu_t *g, uint32_t resource, uint32_t width, uint32_t height)
{
    gpu_scanout_t *scanout = request(g, CMD_SET_SCANOUT, sizeof(*scanout));

    scanout->rect = (gpu_rect_t){ 0, 0, width, height };
    scanout->resource = resource;
    return simple(g, sizeof(*scanout));
}

/* Copy a whole resource from its backing, starting `offset` bytes into it. Lock held. */
static status_t transfer(vgpu_t *g, uint32_t resource, uint32_t width, uint32_t height, uint64_t offset)
{
    gpu_transfer_t *copy = request(g, CMD_TRANSFER_TO_HOST_2D, sizeof(*copy));

    copy->rect = (gpu_rect_t){ 0, 0, width, height };
    copy->offset = offset;
    copy->resource = resource;
    return simple(g, sizeof(*copy));
}

/* One frame: the front framebuffer to the host and onto its screen. Lock held. */
static status_t present(vgpu_t *g)
{
    status_t status = transfer(g, g->screen, g->width, g->height, (uint64_t)g->front * g->buffer_bytes);

    if (STATUS_IS_ERROR(status))
        return status;
    gpu_flush_t *flush = request(g, CMD_RESOURCE_FLUSH, sizeof(*flush));
    flush->rect = (gpu_rect_t){ 0, 0, g->width, g->height };
    flush->resource = g->screen;
    return simple(g, sizeof(*flush));
}

/* What the host wants its output to be. Lock held. */
static status_t host_output(vgpu_t *g)
{
    request(g, CMD_GET_DISPLAY_INFO, sizeof(gpu_header_t));
    status_t status = command(g, sizeof(gpu_header_t), sizeof(gpu_display_info_t));
    if (STATUS_IS_ERROR(status))
        return status;
    const gpu_display_info_t *info = (const gpu_display_info_t *)((uint8_t *)g->commands.virt + RESPONSE_AT);
    g->host_enabled = info->outputs[0].enabled;
    g->host_width = info->outputs[0].rect.width;
    g->host_height = info->outputs[0].rect.height;
    return STATUS_SUCCESS;
}

/* --- Modes --------------------------------------------------------------------------- */

static void add_mode(vgpu_t *g, jelly_display_mode_t *modes, uint32_t *count, uint32_t width, uint32_t height,
                     uint32_t flags)
{
    if (!width || !height || (uint64_t)width * height * 4 > g->buffer_bytes || *count == JELLY_DISPLAY_MODE_MAX)
        return;
    for (uint32_t i = 0; i < *count; i++) {
        if (modes[i].width == width && modes[i].height == height)
            return;
    }
    modes[(*count)++] = (jelly_display_mode_t){ width, height, REFRESH_MHZ, flags };
}

/*
 * The list of modes: the size the host prefers (its window), common sizes, and what is on the screen if it
 * is none of these. The card takes any size; the list is what the framebuffers hold.
 */
static status_t publish_modes(vgpu_t *g, display_t *d)
{
    static const uint16_t sizes[][2] = { { 1920, 1080 }, { 1600, 900 }, { 1440, 900 }, { 1280, 1024 }, { 1280, 800 },
                                         { 1280, 720 },  { 1024, 768 }, { 800, 600 },  { 640, 480 } };
    jelly_display_mode_t modes[JELLY_DISPLAY_MODE_MAX];
    status_t status = STATUS_SUCCESS;

    /* A mode switch may come in between looking at the screen and handing the list over: then once more. */
    for (int attempt = 0; attempt < 3; attempt++) {
        uint32_t count = 0, current = DISPLAY_NO_MODE, width = d->info.width, height = d->info.height;

        add_mode(g, modes, &count, g->host_width, g->host_height, JELLY_MODE_PREFERRED);
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++)
            add_mode(g, modes, &count, sizes[i][0], sizes[i][1], 0);
        add_mode(g, modes, &count, width, height, 0);
        if (count && !(modes[0].flags & JELLY_MODE_PREFERRED))
            modes[0].flags = JELLY_MODE_PREFERRED; /* the host's size does not fit: the largest that does */
        for (uint32_t i = 0; i < count; i++) {
            if (modes[i].width == width && modes[i].height == height)
                current = i;
        }
        status = display_set_modes(0, modes, count, current);
        if (STATUS_IS_ERROR(status) || (d->info.width == width && d->info.height == height))
            break;
    }
    return status;
}

static status_t vgpu_set_mode(display_t *display, uint32_t mode, uint32_t *pitch)
{
    vgpu_t *g = display->driver_data;
    uint32_t width = display->modes[mode].width, height = display->modes[mode].height;

    if ((uint64_t)width * height * 4 > g->buffer_bytes)
        return STATUS_INVALID_ARGUMENT;
    mutex_lock(&g->lock);
    /* A resource has its size for good: a new one of the new size, reading the same memory. */
    uint32_t id = g->next_resource++;
    status_t status = resource_create(g, id, g->format, width, height, g->memory.phys, (uint32_t)g->buffer_bytes,
                                      g->buffers);
    if (!STATUS_IS_ERROR(status)) {
        status = scanout_set(g, id, width, height);
        if (STATUS_IS_ERROR(status)) {
            scanout_set(g, g->screen, g->width, g->height); /* the mode before */
            resource_unref(g, id);
        }
    }
    if (!STATUS_IS_ERROR(status)) {
        resource_unref(g, g->screen);
        g->screen = id;
        g->width = width;
        g->height = height;
        g->front = 0;
        g->flip_pending = false;
        /* What is in the framebuffers was laid out for other lines: empty until it is drawn again. */
        for (uint64_t i = 0; i < g->buffer_bytes * g->buffers / 4; i++)
            ((volatile uint32_t *)g->memory.virt)[i] = 0;
        present(g);
        *pitch = width * 4;
    }
    mutex_unlock(&g->lock);
    return status;
}

/* --- Frames -------------------------------------------------------------------------- */

static status_t vgpu_flip(display_t *display, uint32_t buffer)
{
    vgpu_t *g = display->driver_data;

    mutex_lock(&g->lock);
    if (buffer != g->front) {
        g->front = buffer;
        g->flip_pending = true; /* shown with the next frame */
    }
    mutex_unlock(&g->lock);
    return STATUS_SUCCESS;
}

static status_t vgpu_wait_vblank(display_t *display, uint64_t timeout_ns)
{
    vgpu_t *g = display->driver_data;
    uint64_t deadline = wait_deadline(timeout_ns);
    status_t status = STATUS_SUCCESS;

    /* The next frame; after a flip, the frame that shows the new framebuffer. */
    uint64_t flags = arch_interrupts_save();
    uint64_t frame = g->frames;
    while ((g->frames == frame || g->flip_pending) && status == STATUS_SUCCESS && !g->failed)
        status = wait_queue_block(&g->frame, deadline);
    arch_interrupts_restore(flags);
    return g->failed ? STATUS_DEVICE_ERROR : status;
}

/* The host's output changed (its window has another size, or is gone or back): passed on like another monitor. */
static void host_changed(vgpu_t *g, display_t *d)
{
    volatile uint32_t *config = (volatile uint32_t *)g->virtio.device_config;
    uint32_t width = g->host_width, height = g->host_height;
    bool enabled = g->host_enabled;

    mutex_lock(&g->lock);
    status_t status = host_output(g);
    config[CONFIG_EVENTS_CLEAR / 4] = EVENT_DISPLAY;
    mutex_unlock(&g->lock);
    if (STATUS_IS_ERROR(status))
        return;
    if (g->host_width != width || g->host_height != height) {
        klog_info("virtio-gpu: the host's output is now %ux%u", g->host_width, g->host_height);
        publish_modes(g, d);
    }
    if (g->host_enabled != enabled)
        display_set_connected(0, g->host_enabled);
}

/* The card's clock: a frame to the host every 1/60 second. */
static void present_thread(void *argument)
{
    vgpu_t *g = argument;
    display_t *d = display_get(0);
    volatile uint32_t *config = (volatile uint32_t *)g->virtio.device_config;
    uint64_t next = clock_monotonic_ns();
    uint32_t count = 0;

    while (!g->failed) {
        uint64_t now = clock_monotonic_ns();
        next += FRAME_NS;
        if (next <= now)
            next = now + FRAME_NS; /* fell behind (a slow host): no catching up */
        thread_sleep(next - now);

        mutex_lock(&g->lock);
        present(g);
        uint64_t flags = arch_interrupts_save();
        g->flip_pending = false;
        g->frames++;
        arch_interrupts_restore(flags);
        mutex_unlock(&g->lock);
        wait_queue_wake_all(&g->frame, STATUS_SUCCESS);

        if (++count % (REFRESH_MHZ / 1000) == 0 && config && (config[CONFIG_EVENTS_READ / 4] & EVENT_DISPLAY))
            host_changed(g, d);
    }
    wait_queue_wake_all(&g->frame, STATUS_SUCCESS);
}

/* --- The pointer --------------------------------------------------------------------- */

/* A command on the cursor queue. The device gives the buffers back without an interrupt; they are collected here. */
static void cursor_send(vgpu_t *g, const gpu_cursor_t *what)
{
    uint32_t id, length;

    for (int tries = 0;; tries++) {
        while (virtio_queue_next_used(&g->cursor, &id, &length)) {
            if (id < CURSOR_SLOTS)
                g->cursor_free |= 1u << id;
        }
        if (g->cursor_free)
            break;
        if (tries == 5) {
            g->pointer_new = true; /* the host is behind: this one is left out, the next says everything again */
            return;
        }
        thread_sleep(1000000);
    }
    uint16_t slot = (uint16_t)__builtin_ctz(g->cursor_free);
    g->cursor_free &= ~(1u << slot);
    memcpy((uint8_t *)g->commands.virt + CURSOR_AT + slot * CURSOR_SLOT_BYTES, what, sizeof(*what));
    g->cursor.desc[slot] = (virtq_desc_t){ g->commands.phys + CURSOR_AT + slot * CURSOR_SLOT_BYTES, sizeof(*what), 0, 0 };
    virtio_queue_submit(&g->cursor, slot);
}

static status_t vgpu_cursor_image(display_t *display, const uint32_t *pixels)
{
    vgpu_t *g = display->driver_data;

    mutex_lock(&g->lock);
    memcpy(g->pointer.virt, pixels, JELLY_CURSOR_SIZE * JELLY_CURSOR_SIZE * 4);
    status_t status = transfer(g, POINTER_RESOURCE, JELLY_CURSOR_SIZE, JELLY_CURSOR_SIZE, 0);
    g->pointer_new = true; /* the host takes the image with the next UPDATE_CURSOR */
    mutex_unlock(&g->lock);
    return status;
}

static void vgpu_cursor_move(display_t *display, int32_t x, int32_t y, bool visible)
{
    vgpu_t *g = display->driver_data;
    int32_t size = JELLY_CURSOR_SIZE;
    gpu_cursor_t cursor = { 0 };

    /* The host places the image's hot spot; an image hanging over the left or top edge is shown by moving the
     * hot spot into the image instead. */
    if (x <= -size || y <= -size || x >= (int32_t)display->info.width || y >= (int32_t)display->info.height)
        visible = false;
    uint32_t hot_x = x < 0 ? (uint32_t)-x : 0, hot_y = y < 0 ? (uint32_t)-y : 0;

    mutex_lock(&g->lock);
    if (!visible && !g->pointer_shown && !g->pointer_new) {
        mutex_unlock(&g->lock);
        return;
    }
    bool update = g->pointer_new || visible != g->pointer_shown || hot_x != g->hot_x || hot_y != g->hot_y;
    cursor.header.type = update ? CMD_UPDATE_CURSOR : CMD_MOVE_CURSOR;
    cursor.x = x < 0 ? 0 : (uint32_t)x;
    cursor.y = y < 0 ? 0 : (uint32_t)y;
    cursor.resource = visible ? POINTER_RESOURCE : 0; /* (also in a move: without it the host hides the pointer) */
    cursor.hot_x = hot_x;
    cursor.hot_y = hot_y;
    g->pointer_new = false;
    g->pointer_shown = visible;
    g->hot_x = hot_x;
    g->hot_y = hot_y;
    cursor_send(g, &cursor);
    mutex_unlock(&g->lock);
}

/* --- Panic --------------------------------------------------------------------------- */

/* The device's answer without sleeping and without its interrupt. False if none comes. */
static bool panic_answer(vgpu_t *g)
{
    for (uint32_t spins = 0; spins < PANIC_SPINS; spins++) {
        if (virtio_queue_has_used(&g->control)) {
            virtio_queue_pop_used(&g->control);
            g->in_flight = false;
            return true;
        }
        __asm__ volatile("pause");
    }
    g->failed = true;
    return false;
}

static bool panic_command(vgpu_t *g, uint32_t request_bytes)
{
    g->control.desc[0] = (virtq_desc_t){ g->commands.phys, request_bytes, VIRTQ_DESC_F_NEXT, 1 };
    g->control.desc[1] = (virtq_desc_t){ g->commands.phys + RESPONSE_AT, sizeof(gpu_header_t), VIRTQ_DESC_F_WRITE, 0 };
    g->in_flight = true;
    virtio_queue_submit(&g->control, 0);
    return panic_answer(g);
}

/*
 * The kernel's last words are in the first framebuffer: one more frame from there, and the pointer away. The
 * thread that presents frames does not run any more, nor does anything else: a command that was on its way is
 * waited for first, then the frame is sent with the device polled for its answers. The lock is not taken.
 */
static void vgpu_panic(display_t *display)
{
    vgpu_t *g = display->driver_data;
    uint32_t id, length;

    if (g->failed || !g->screen || (g->in_flight && !panic_answer(g)))
        return;
    g->front = 0;
    gpu_transfer_t *copy = request(g, CMD_TRANSFER_TO_HOST_2D, sizeof(*copy));
    copy->rect = (gpu_rect_t){ 0, 0, g->width, g->height };
    copy->resource = g->screen;
    if (!panic_command(g, sizeof(*copy)))
        return;
    gpu_flush_t *flush = request(g, CMD_RESOURCE_FLUSH, sizeof(*flush));
    flush->rect = (gpu_rect_t){ 0, 0, g->width, g->height };
    flush->resource = g->screen;
    if (!panic_command(g, sizeof(*flush)))
        return;

    /* The pointer, if a place on its queue is free (nothing is waited for there). */
    while (virtio_queue_next_used(&g->cursor, &id, &length)) {
        if (id < CURSOR_SLOTS)
            g->cursor_free |= 1u << id;
    }
    if (g->pointer_shown && g->cursor_free) {
        uint16_t slot = (uint16_t)__builtin_ctz(g->cursor_free);
        gpu_cursor_t *cursor = (gpu_cursor_t *)((uint8_t *)g->commands.virt + CURSOR_AT + slot * CURSOR_SLOT_BYTES);
        memset(cursor, 0, sizeof(*cursor));
        cursor->header.type = CMD_UPDATE_CURSOR; /* (resource 0: no pointer) */
        g->cursor_free &= ~(1u << slot);
        g->cursor.desc[slot] = (virtq_desc_t){ g->commands.phys + CURSOR_AT + slot * CURSOR_SLOT_BYTES, sizeof(*cursor), 0, 0 };
        virtio_queue_submit(&g->cursor, slot);
        g->pointer_shown = false;
    }
}

static display_ops_t vgpu_ops = { .wait_vblank = vgpu_wait_vblank, .set_mode = vgpu_set_mode, .panic = vgpu_panic };

/* --- Start --------------------------------------------------------------------------- */

static void release(vgpu_t *g)
{
    if (g->virtio.common)
        virtio_reset(&g->virtio); /* the host shows the VGA side again */
    pci_disable_msix(g->virtio.pci);
    virtio_queue_free(&g->control);
    virtio_queue_free(&g->cursor);
    dma_free(&g->commands);
    dma_free(&g->memory);
    dma_free(&g->pointer);
    memset(g, 0, sizeof(*g));
}

static status_t vgpu_probe(device_t *device)
{
    static vgpu_t card; /* one such card shows the boot framebuffer */
    vgpu_t *g = &card;
    pci_device_t *pci = pci_from_device(device);
    display_t *d = display_get(0);
    thread_t *thread;

    if (g->virtio.pci || !d)
        return STATUS_NOT_SUPPORTED;
    if (!pci->bars[0].phys || pci->bars[0].io || d->phys != pci->bars[0].phys || d->info.bpp != 32) {
        klog_info("virtio-gpu: the boot framebuffer is not this card's (a card without the VGA side?): left alone");
        return STATUS_NOT_SUPPORTED;
    }
    if (d->info.red_shift == 16 && d->info.blue_shift == 0) {
        g->format = FORMAT_B8G8R8X8;
    } else if (d->info.red_shift == 0 && d->info.blue_shift == 16) {
        g->format = FORMAT_R8G8B8X8;
    } else {
        klog_info("virtio-gpu: the boot framebuffer has a pixel format the card does not know: left alone");
        return STATUS_NOT_SUPPORTED;
    }
    mutex_init(&g->lock);
    wait_queue_init(&g->completion);
    wait_queue_init(&g->frame);
    g->next_resource = POINTER_RESOURCE + 1;
    g->cursor_free = (1u << CURSOR_SLOTS) - 1;
    g->width = d->info.width;
    g->height = d->info.height;

    status_t status = virtio_init(&g->virtio, pci, 0, device);
    if (!STATUS_IS_ERROR(status))
        status = pci_enable_msix(pci, 0, vgpu_interrupt, g, &g->irq);
    if (!STATUS_IS_ERROR(status))
        status = virtio_queue_setup(&g->virtio, device, QUEUE_CONTROL, 64, 0, &g->control);
    if (!STATUS_IS_ERROR(status))
        status = virtio_queue_setup(&g->virtio, device, QUEUE_CURSOR, CURSOR_SLOTS, NO_INTERRUPT, &g->cursor);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, PAGE_SIZE, ~0ULL, &g->commands);
    if (!STATUS_IS_ERROR(status))
        status = dma_alloc(device, JELLY_CURSOR_SIZE * JELLY_CURSOR_SIZE * 4, ~0ULL, &g->pointer);
    if (STATUS_IS_ERROR(status)) {
        klog_warn("virtio-gpu: the device cannot be set up (%s)", status_name(status));
        release(g);
        return status;
    }
    virtio_driver_ok(&g->virtio);

    mutex_lock(&g->lock);
    status = host_output(g);
    if (!STATUS_IS_ERROR(status)) {
        /* Framebuffers for the largest mode of the list, two if the memory is there. */
        uint64_t pixels = (uint64_t)LARGEST_WIDTH * LARGEST_HEIGHT;
        if ((uint64_t)g->width * g->height > pixels)
            pixels = (uint64_t)g->width * g->height;
        if ((uint64_t)g->host_width * g->host_height > pixels && g->host_width <= 4096 && g->host_height <= 4096)
            pixels = (uint64_t)g->host_width * g->host_height;
        g->buffer_bytes = align_up(pixels * 4, PAGE_SIZE);
        g->buffers = 2;
        if (STATUS_IS_ERROR(dma_alloc(device, g->buffer_bytes * 2, ~0ULL, &g->memory))) {
            g->buffers = 1;
            status = dma_alloc(device, g->buffer_bytes, ~0ULL, &g->memory);
        }
    }
    if (!STATUS_IS_ERROR(status))
        status = resource_create(g, POINTER_RESOURCE, FORMAT_B8G8R8A8, JELLY_CURSOR_SIZE, JELLY_CURSOR_SIZE,
                                 g->pointer.phys, JELLY_CURSOR_SIZE * JELLY_CURSOR_SIZE * 4, 1);
    if (!STATUS_IS_ERROR(status)) {
        g->screen = g->next_resource++;
        status = resource_create(g, g->screen, g->format, g->width, g->height, g->memory.phys,
                                 (uint32_t)g->buffer_bytes, g->buffers);
    }
    mutex_unlock(&g->lock);
    if (STATUS_IS_ERROR(status)) {
        klog_warn("virtio-gpu: the card does not take the screen (%s)", status_name(status));
        release(g);
        return status;
    }
    klog_info("virtio-gpu: VirtIO GPU 1af4:%04x with %u output%s, the host's is %ux%u%s; %u framebuffer%s of %lu KiB in "
              "guest memory", device->id.device, *(volatile uint32_t *)(g->virtio.device_config + CONFIG_SCANOUTS),
              *(volatile uint32_t *)(g->virtio.device_config + CONFIG_SCANOUTS) == 1 ? "" : "s", g->host_width,
              g->host_height, g->host_enabled ? "" : " (off)", g->buffers, g->buffers == 1 ? "" : "s",
              g->buffer_bytes >> 10);

    /*
     * The console moves into the new framebuffer first; only then does the host switch from the VGA side to
     * this one, so that it never shows an empty screen for longer than a frame.
     */
    status = display_set_framebuffer(0, g->memory.phys, g->buffer_bytes, g->width, g->height, g->width * 4);
    if (!STATUS_IS_ERROR(status)) {
        mutex_lock(&g->lock);
        status = scanout_set(g, g->screen, g->width, g->height);
        if (!STATUS_IS_ERROR(status))
            status = present(g);
        mutex_unlock(&g->lock);
    }
    if (STATUS_IS_ERROR(status)) {
        /* (After display_set_framebuffer() the display keeps the memory: it is not given back.) */
        klog_error("virtio-gpu: the card does not show the screen (%s)", status_name(status));
        g->failed = true;
        return status;
    }
    if (STATUS_IS_ERROR(thread_create_kernel("virtio-gpu", present_thread, g, THREAD_PRIORITY_KERNEL, &thread))) {
        klog_error("virtio-gpu: no thread to present frames: the screen stays as it is");
        g->failed = true;
        return STATUS_OUT_OF_MEMORY;
    }
    thread_start(thread);
    object_release(&thread->object);

    vgpu_ops.cursor_image = vgpu_cursor_image;
    vgpu_ops.cursor_move = vgpu_cursor_move;
    if (g->buffers == 2)
        vgpu_ops.flip = vgpu_flip;
    status = display_set_driver(0, &vgpu_ops, g, g->buffers == 2 ? g->memory.phys + g->buffer_bytes : 0);
    if (!STATUS_IS_ERROR(status))
        status = publish_modes(g, d);
    if (STATUS_IS_ERROR(status))
        klog_warn("virtio-gpu: the display does not take the driver (%s)", status_name(status));
    if (!g->host_enabled)
        display_set_connected(0, false);
    return STATUS_SUCCESS; /* the screen is this card's now, with or without the extras */
}

static const device_match_t vgpu_ids[] = {
    DEVICE_MATCH_ID(VIRTIO_VENDOR, VIRTIO_GPU_DEVICE),
    DEVICE_MATCH_END,
};

static driver_t vgpu_driver = {
    .name = "virtio-gpu",
    .bus_name = "pci",
    .version = 1,
    .capabilities = DRIVER_CAP_DISPLAY,
    .ids = vgpu_ids,
    .probe = vgpu_probe,
};

static status_t vgpu_module_init(void)
{
    return driver_register(&vgpu_driver);
}

static const char *const vgpu_dependencies[] = { "pci", NULL };

MODULE(.name = "virtio_gpu", .description = "VirtIO GPU: frames, pointer, page flipping and modes in a virtual machine",
       .version = 1, .min_kernel_version = KERNEL_VERSION(0, 12, 0), .dependencies = vgpu_dependencies,
       .init = vgpu_module_init);
