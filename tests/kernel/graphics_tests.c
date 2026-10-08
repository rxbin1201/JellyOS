/*
 * Kernel tests for Phase 9: waiting for several objects, objects in channel
 * messages, named services, the input manager and displays.
 *
 * The display server and applications on top are exercised by the
 * integration test (tests/integration/shell_test.py, GUI steps).
 */

#include "tests/kernel/ktest.h"

#include "core/string.h"
#include "drivers/graphics/display.h"
#include "input/input.h"
#include "ipc/ipc.h"
#include "core/log.h"
#include "memory/heap.h"
#include "memory/layout.h"
#include "memory/pmm.h"
#include "scheduler/thread.h"

#include <jelly/input.h>
#include <jelly/syscall.h>

KTEST(wait_for_several_objects)
{
    object_t *a, *b;
    uint32_t index = 99;
    KASSERT(event_create(0, &a) == STATUS_SUCCESS);
    KASSERT(event_create(JELLY_EVENT_AUTO_RESET, &b) == STATUS_SUCCESS);
    object_t *both[] = { a, b };

    KEXPECT(object_wait_many(both, 2, 0, &index) == STATUS_TIMEOUT);
    KEXPECT(object_wait_many(both, 2, 10000000ULL, &index) == STATUS_TIMEOUT);
    event_signal(b);
    KEXPECT(object_wait_many(both, 2, 0, &index) == STATUS_SUCCESS && index == 1);
    /* b resets automatically when a wait is satisfied */
    KEXPECT(object_wait_many(both, 2, 0, &index) == STATUS_TIMEOUT);
    event_signal(a);
    KEXPECT(object_wait_many(both, 2, 0, &index) == STATUS_SUCCESS && index == 0);
    KEXPECT(object_wait_many(both, 0, 0, &index) == STATUS_INVALID_ARGUMENT);
    /* No observers stay behind. */
    KEXPECT(list_empty(&a->observers) && list_empty(&b->observers));

    object_release(a);
    object_release(b);
}

KTEST(channels_carry_objects)
{
    object_t *end0, *end1, *event, *received[CHANNEL_MAX_OBJECTS];
    uint32_t rights = JELLY_RIGHT_WAIT | JELLY_RIGHT_SIGNAL, received_rights[CHANNEL_MAX_OBJECTS], count;
    void *data;
    size_t size;

    KASSERT(channel_create(&end0, &end1) == STATUS_SUCCESS);
    KASSERT(event_create(0, &event) == STATUS_SUCCESS);
    char *message = kmalloc(5);
    KASSERT(message != NULL);
    memcpy(message, "hand", 5);

    object_retain(event); /* the message takes one reference */
    KEXPECT(channel_send_objects(end0, message, 5, &event, &rights, 1) == STATUS_SUCCESS);
    KEXPECT(event->refs == 2);
    KEXPECT(channel_peek_objects(end1, &size, &count) == STATUS_SUCCESS && size == 5 && count == 1);
    KASSERT(channel_take_objects(end1, &data, &size, received, received_rights, &count) == STATUS_SUCCESS);
    KEXPECT(count == 1 && received[0] == event && received_rights[0] == rights);
    KEXPECT(!memcmp(data, "hand", 5));
    kfree(data);
    object_release(received[0]);

    /* An endpoint cannot travel through its own channel. */
    char *bad = kmalloc(1);
    KEXPECT(channel_send_objects(end0, bad, 1, &end0, &rights, 1) == STATUS_INVALID_ARGUMENT);
    KEXPECT(channel_send_objects(end0, bad, 1, &end1, &rights, 1) == STATUS_INVALID_ARGUMENT);
    kfree(bad);

    /* Unread messages release their objects when the channel goes away. */
    message = kmalloc(1);
    object_retain(event);
    KEXPECT(channel_send_objects(end0, message, 1, &event, &rights, 1) == STATUS_SUCCESS);
    KEXPECT(event->refs == 2);
    object_release(end1);
    KEXPECT(event->refs == 1);

    object_release(end0);
    object_release(event);
}

KTEST(named_services_connect_clients)
{
    object_t *server, *registered, *client, *connection, *objects[CHANNEL_MAX_OBJECTS];
    uint32_t rights[CHANNEL_MAX_OBJECTS], count;
    void *data;
    size_t size;

    KEXPECT(service_connect("ktest-service", &client) == STATUS_NOT_FOUND);
    KASSERT(channel_create(&server, &registered) == STATUS_SUCCESS);
    KASSERT(service_register("ktest-service", registered) == STATUS_SUCCESS);
    object_release(registered); /* the registry keeps its own reference */
    KEXPECT(service_register("ktest-service", server) == STATUS_ALREADY_EXISTS);

    KASSERT(service_connect("ktest-service", &client) == STATUS_SUCCESS);
    KASSERT(channel_take_objects(server, &data, &size, objects, rights, &count) == STATUS_SUCCESS);
    KEXPECT(count == 1 && size == 8 && !memcmp(data, "connect", 8));
    kfree(data);
    connection = objects[0];
    KEXPECT(rights[0] & JELLY_RIGHT_READ);

    /* The two ends talk to each other. */
    char *hello = kmalloc(3);
    memcpy(hello, "hi", 3);
    KEXPECT(channel_send(client, hello, 3) == STATUS_SUCCESS);
    KEXPECT(channel_take(connection, &data, &size) == STATUS_SUCCESS && size == 3);
    kfree(data);

    /* Without its server the service disappears. */
    object_release(server);
    object_t *late;
    KEXPECT(service_connect("ktest-service", &late) == STATUS_NOT_FOUND);
    object_release(client);
    object_release(connection);
}

KTEST(input_queues_receive_events)
{
    object_t *queue;
    jelly_input_event_t events[4];
    KASSERT(input_open(&queue) == STATUS_SUCCESS);
    KEXPECT(!queue->ops->signaled(queue));

    input_report(7, JELLY_INPUT_KEY_DOWN, JELLY_KEY_A, 1, 0, 0, 0, 0, 0);
    input_report(7, JELLY_INPUT_MOUSE_MOVE, 0, 0, 0, 0, 100, 200, JELLY_INPUT_ABSOLUTE);
    KEXPECT(queue->ops->signaled(queue));
    KASSERT(input_read(queue, events, 4) == 2);
    KEXPECT(events[0].type == JELLY_INPUT_KEY_DOWN && events[0].code == JELLY_KEY_A && events[0].device == 7);
    KEXPECT(events[1].x == 100 && events[1].y == 200 && (events[1].flags & JELLY_INPUT_ABSOLUTE));
    KEXPECT(events[1].time_ns >= events[0].time_ns && events[0].time_ns > 0);
    KEXPECT(input_read(queue, events, 4) == 0);

    /* A full queue drops the oldest events. */
    for (int i = 0; i < 600; i++)
        input_report(0, JELLY_INPUT_MOUSE_WHEEL, 0, i, 0, 0, 0, 0, 0);
    KASSERT(input_read(queue, events, 1) == 1);
    KEXPECT(events[0].value == 600 - 512);
    object_release(queue);
}

KTEST(display_acquire_is_exclusive)
{
    display_t *d = display_get(0);
    object_t *memory, *again;
    if (!d) {
        KEXPECT(display_acquire(0, &memory) == STATUS_NOT_FOUND);
        return;
    }
    KEXPECT(d->info.width > 0 && d->info.height > 0 && d->info.pitch >= d->info.width * 4);
    KEXPECT(d->info.size >= (uint64_t)d->info.pitch * d->info.height);
    KASSERT(display_acquire(0, &memory) == STATUS_SUCCESS);
    KEXPECT(d->acquired && (d->info.flags & JELLY_DISPLAY_ACQUIRED));
    KEXPECT(display_acquire(0, &again) == STATUS_BUSY);
    KEXPECT(shm_size(memory) == d->info.size);
    object_release(memory);
    KEXPECT(!d->acquired);
    KEXPECT(display_acquire(99, &again) == STATUS_NOT_FOUND);
}

/*
 * What a graphics driver does after switching modes: display 0 gets another
 * framebuffer and size, and the kernel console moves there. Here the "new
 * screen" is plain memory, and the real one is put back afterwards.
 */
KTEST(display_framebuffer_can_be_replaced)
{
    display_t *d = display_get(0);
    object_t *memory;
    uint64_t phys;

    if (!d)
        return;
    uint64_t old_phys = d->phys;
    uint32_t old_width = d->info.width, old_height = d->info.height, old_pitch = d->info.pitch;
    uint64_t old_size = d->info.size;
    const uint32_t width = 1000, height = 500, pitch = 4032; /* a pitch wider than the picture, as hardware has it */
    size_t pages = (size_t)(align_up((uint64_t)pitch * height, PAGE_SIZE) / PAGE_SIZE);
    KASSERT(pmm_alloc_pages(pages, &phys) == STATUS_SUCCESS);
    volatile uint32_t *memory_view = phys_to_virt(phys);

    KEXPECT(display_set_framebuffer(99, phys, 0, width, height, pitch) == STATUS_NOT_FOUND);
    KEXPECT(display_set_framebuffer(0, phys, 0, width, height, width * 4 - 4) == STATUS_INVALID_ARGUMENT);
    KEXPECT(display_set_framebuffer(0, phys + 1, 0, width, height, pitch) == STATUS_INVALID_ARGUMENT);

    KASSERT(display_set_framebuffer(0, phys, 0, width, height, pitch) == STATUS_SUCCESS);
    KEXPECT(d->info.width == width && d->info.height == height && d->info.pitch == pitch && d->phys == phys);
    KEXPECT(d->info.size >= (uint64_t)pitch * height);
    /* The console repainted the new screen: its background fills the corners, and a message draws glyphs. */
    uint32_t background = memory_view[(height - 1) * (pitch / 4) + width - 1];
    KEXPECT(memory_view[0] == background || memory_view[1] == background);
    klog_info("ktest: this line is drawn into the replaced framebuffer");
    bool drawn = false;
    for (uint32_t i = 0; i < (pitch / 4) * 64 && !drawn; i++)
        drawn = memory_view[i] != background;
    KEXPECT(drawn);

    /* A display server gets the new memory; while it has it, the framebuffer cannot be replaced. */
    KASSERT(display_acquire(0, &memory) == STATUS_SUCCESS);
    KEXPECT(shm_size(memory) == d->info.size);
    KEXPECT(display_set_framebuffer(0, old_phys, old_size, old_width, old_height, old_pitch) == STATUS_BUSY);
    object_release(memory);

    KASSERT(display_set_framebuffer(0, old_phys, old_size, old_width, old_height, old_pitch) == STATUS_SUCCESS);
    KEXPECT(d->info.width == old_width && d->info.height == old_height && d->phys == old_phys);
    pmm_free_pages(phys, pages);
}

/* --- The driver interface of a display, with a driver that exists only here ---------------- */

/* The driver the machine really has (in QEMU: bochs-gpu), put aside while a test brings its own. */
static struct {
    const display_ops_t *ops;
    void                *data;
    uint64_t             second_phys;
    jelly_display_mode_t modes[JELLY_DISPLAY_MODE_MAX];
    uint32_t             mode_count, current;
} real_driver;

static void real_driver_save(display_t *d)
{
    real_driver.ops = d->ops;
    real_driver.data = d->driver_data;
    real_driver.second_phys = d->second_phys;
    real_driver.mode_count = d->mode_count;
    real_driver.current = d->current_mode;
    memcpy(real_driver.modes, d->modes, sizeof(real_driver.modes));
    display_set_driver(0, NULL, NULL, 0);
}

static void real_driver_restore(void)
{
    display_set_driver(0, real_driver.ops, real_driver.data, real_driver.second_phys);
    if (real_driver.mode_count)
        display_set_modes(0, real_driver.modes, real_driver.mode_count, real_driver.current);
}

static struct {
    int      images, moves, waits, flips;
    int32_t  x, y;
    bool     visible;
    uint32_t shown, first_pixel;
} fake_gpu;

static status_t fake_cursor_image(display_t *display, const uint32_t *pixels)
{
    (void)display;
    fake_gpu.images++;
    fake_gpu.first_pixel = pixels[0];
    return STATUS_SUCCESS;
}

static void fake_cursor_move(display_t *display, int32_t x, int32_t y, bool visible)
{
    (void)display;
    fake_gpu.moves++;
    fake_gpu.x = x;
    fake_gpu.y = y;
    fake_gpu.visible = visible;
}

static status_t fake_wait_vblank(display_t *display, uint64_t timeout_ns)
{
    (void)display;
    fake_gpu.waits++;
    return timeout_ns ? STATUS_SUCCESS : STATUS_TIMEOUT;
}

static status_t fake_flip(display_t *display, uint32_t buffer)
{
    (void)display;
    fake_gpu.flips++;
    fake_gpu.shown = buffer;
    return STATUS_SUCCESS;
}

KTEST(display_driver_operations)
{
    static const display_ops_t all = { .cursor_image = fake_cursor_image, .cursor_move = fake_cursor_move,
                                       .wait_vblank = fake_wait_vblank, .flip = fake_flip };
    static const display_ops_t pointer_only = { .cursor_image = fake_cursor_image, .cursor_move = fake_cursor_move };
    static uint32_t image[JELLY_CURSOR_SIZE * JELLY_CURSOR_SIZE] = { 0xFF112233 };
    display_t *d = display_get(0);
    jelly_cursor_t cursor = { .flags = JELLY_CURSOR_IMAGE | JELLY_CURSOR_VISIBLE, .x = -3, .y = 700, .pixels = image };
    object_t *memory, *second;
    uint64_t phys;

    if (!d)
        return;
    size_t pages = (size_t)(d->info.size / PAGE_SIZE);
    real_driver_save(d);

    /* Without a driver the display is a plain framebuffer and says so. */
    KEXPECT(!(d->info.flags & (JELLY_DISPLAY_CURSOR | JELLY_DISPLAY_VBLANK | JELLY_DISPLAY_FLIP)));
    KEXPECT(display_cursor(0, &cursor, image) == STATUS_NOT_SUPPORTED);
    KEXPECT(display_wait_vblank(0, 1000000) == STATUS_NOT_SUPPORTED);
    KEXPECT(display_flip(0, 1) == STATUS_NOT_SUPPORTED);
    KEXPECT(display_buffer(0, 1, &second) == STATUS_NOT_SUPPORTED);
    KEXPECT(display_set_driver(99, &all, NULL, 0) == STATUS_NOT_FOUND);

    /* A driver with only a pointer: only that capability appears. */
    KASSERT(display_set_driver(0, &pointer_only, NULL, 0) == STATUS_SUCCESS);
    KEXPECT((d->info.flags & JELLY_DISPLAY_CURSOR) && !(d->info.flags & (JELLY_DISPLAY_VBLANK | JELLY_DISPLAY_FLIP)));
    KEXPECT(display_cursor(0, &cursor, image) == STATUS_SUCCESS);
    KEXPECT(fake_gpu.images == 1 && fake_gpu.first_pixel == 0xFF112233 && fake_gpu.x == -3 && fake_gpu.y == 700 &&
            fake_gpu.visible);
    cursor.flags = 0; /* move only: no new image, hidden */
    KEXPECT(display_cursor(0, &cursor, NULL) == STATUS_SUCCESS && fake_gpu.images == 1 && !fake_gpu.visible);
    KEXPECT(display_flip(0, 1) == STATUS_NOT_SUPPORTED);

    /* Everything, with a second framebuffer: flipping needs both the operation and the memory. */
    KASSERT(display_set_driver(0, &all, NULL, 0) == STATUS_SUCCESS);
    KEXPECT(!(d->info.flags & JELLY_DISPLAY_FLIP));
    KASSERT(pmm_alloc_pages(pages, &phys) == STATUS_SUCCESS);
    KASSERT(display_set_driver(0, &all, NULL, phys) == STATUS_SUCCESS);
    KEXPECT((d->info.flags & JELLY_DISPLAY_FLIP) && (d->info.flags & JELLY_DISPLAY_VBLANK));
    KEXPECT(display_wait_vblank(0, 1000000) == STATUS_SUCCESS && display_wait_vblank(0, 0) == STATUS_TIMEOUT);
    KEXPECT(display_flip(0, 2) == STATUS_INVALID_ARGUMENT);
    KEXPECT(display_buffer(0, 0, &second) == STATUS_INVALID_ARGUMENT);

    /* A display server takes the display, flips to the second buffer and goes away: the first one is shown again. */
    KASSERT(display_acquire(0, &memory) == STATUS_SUCCESS);
    KEXPECT(display_set_driver(0, &all, NULL, phys) == STATUS_BUSY);
    KASSERT(display_buffer(0, 1, &second) == STATUS_SUCCESS);
    KEXPECT(shm_size(second) == d->info.size);
    cursor.flags = JELLY_CURSOR_VISIBLE;
    KEXPECT(display_cursor(0, &cursor, NULL) == STATUS_SUCCESS && fake_gpu.visible);
    KEXPECT(display_flip(0, 1) == STATUS_SUCCESS && fake_gpu.shown == 1);
    object_release(second);
    object_release(memory);
    KEXPECT(fake_gpu.shown == 0 && !fake_gpu.visible && !d->acquired);

    /* Without a driver again, then with the one the machine has. */
    KASSERT(display_set_driver(0, NULL, NULL, 0) == STATUS_SUCCESS);
    KEXPECT(!(d->info.flags & (JELLY_DISPLAY_CURSOR | JELLY_DISPLAY_VBLANK | JELLY_DISPLAY_FLIP)));
    pmm_free_pages(phys, pages);
    real_driver_restore();
}

/* --- Modes: a list from the driver, switching, and telling whoever watches the display ------ */

static struct {
    jelly_display_mode_t list[3];
    uint32_t             pitch[3];
    uint32_t             shown;
    int                  calls;
    int                  panics;
    bool                 fails;
} fake_modes;

static void fake_panic(display_t *display)
{
    (void)display;
    fake_modes.panics++;
}

static status_t fake_set_mode(display_t *display, uint32_t mode, uint32_t *pitch)
{
    (void)display;
    fake_modes.calls++;
    if (fake_modes.fails)
        return STATUS_DEVICE_ERROR; /* as a driver whose mode did not come up and that put the old one back */
    fake_modes.shown = mode;
    *pitch = fake_modes.pitch[mode];
    return STATUS_SUCCESS;
}

static bool event_is_signaled(object_t *event)
{
    uint32_t index;
    return object_wait_many(&event, 1, 0, &index) == STATUS_SUCCESS;
}

KTEST(display_modes_can_be_switched)
{
    static const display_ops_t ops = { .set_mode = fake_set_mode, .panic = fake_panic };
    display_t *d = display_get(0);
    jelly_display_mode_t modes[4];
    object_t *event, *memory;
    uint32_t count = 0;

    if (!d || d->info.width < 640 || d->info.height < 480)
        return;
    real_driver_save(d);
    /* The framebuffer stays the same memory in every mode: the real one and two smaller ones that fit into it. */
    const uint32_t width = d->info.width, height = d->info.height, pitch = d->info.pitch;
    fake_modes.list[0] = (jelly_display_mode_t){ width, height, 60000, JELLY_MODE_PREFERRED };
    fake_modes.list[1] = (jelly_display_mode_t){ 640, 480, 75000, 0 };
    fake_modes.list[2] = (jelly_display_mode_t){ 320, 240, 0, 0 };
    fake_modes.pitch[0] = pitch;
    fake_modes.pitch[1] = 640 * 4 + 64; /* a pitch wider than the picture */
    fake_modes.pitch[2] = 320 * 4;

    /* No driver: one mode, the one on the screen, and no way to change it. */
    KEXPECT(display_modes(0, modes, 4, &count) == STATUS_SUCCESS && count == 1);
    KEXPECT(modes[0].width == width && modes[0].height == height && (modes[0].flags & JELLY_MODE_CURRENT));
    KEXPECT(display_set_mode(0, 0) == STATUS_NOT_SUPPORTED);
    KEXPECT(display_modes(99, modes, 4, &count) == STATUS_NOT_FOUND);

    /* A driver that can switch, but only once it has named its modes. */
    KASSERT(display_set_driver(0, &ops, NULL, 0) == STATUS_SUCCESS);
    KEXPECT(!(d->info.flags & JELLY_DISPLAY_MODES));
    KEXPECT(display_set_modes(0, fake_modes.list, 3, 3) == STATUS_INVALID_ARGUMENT);
    KASSERT(display_set_modes(0, fake_modes.list, 3, 0) == STATUS_SUCCESS);
    KEXPECT((d->info.flags & JELLY_DISPLAY_MODES) && d->info.refresh_mhz == 60000);
    KEXPECT(display_modes(0, modes, 2, &count) == STATUS_SUCCESS && count == 3); /* two stored, three there are */
    KEXPECT(modes[0].flags == (JELLY_MODE_CURRENT | JELLY_MODE_PREFERRED) && modes[1].flags == 0);

    /* Switching: geometry, refresh rate and the marked mode follow; the watcher's event is signaled. */
    KASSERT(display_watch(0, &event) == STATUS_SUCCESS);
    event_reset(event);
    uint32_t generation = d->info.generation;
    KASSERT(display_set_mode(0, 1) == STATUS_SUCCESS);
    KEXPECT(fake_modes.shown == 1 && d->info.width == 640 && d->info.height == 480 &&
            d->info.pitch == fake_modes.pitch[1] && d->info.refresh_mhz == 75000);
    KEXPECT(d->info.generation != generation && event_is_signaled(event));
    KEXPECT(display_modes(0, modes, 4, &count) == STATUS_SUCCESS && modes[1].flags == JELLY_MODE_CURRENT &&
            modes[0].flags == JELLY_MODE_PREFERRED);
    klog_info("ktest: this line is drawn by the console in the switched mode");
    event_reset(event);
    KEXPECT(!event_is_signaled(event));

    /* The mode being shown again costs nothing; a mode that is not in the list is refused. */
    int calls = fake_modes.calls;
    KEXPECT(display_set_mode(0, 1) == STATUS_SUCCESS && fake_modes.calls == calls);
    KEXPECT(display_set_mode(0, 3) == STATUS_INVALID_ARGUMENT);

    /* A mode that does not come up: the error is passed on and nothing changes. */
    fake_modes.fails = true;
    KEXPECT(display_set_mode(0, 2) == STATUS_DEVICE_ERROR);
    KEXPECT(d->info.width == 640 && d->info.height == 480 && d->current_mode == 1);
    fake_modes.fails = false;

    /* While a display server owns the display its mapping stays valid, and the console catches up afterwards. */
    KASSERT(display_acquire(0, &memory) == STATUS_SUCCESS);
    uint64_t size = shm_size(memory);
    event_reset(event);
    KASSERT(display_set_mode(0, 2) == STATUS_SUCCESS);
    KEXPECT(d->info.width == 320 && d->info.height == 240 && d->info.size == size && d->console_stale);
    KEXPECT(event_is_signaled(event));

    /*
     * A panic now: the display server has the screen, and the console's text grid is still the one of the mode
     * before. The console lays itself out for the screen as it is and draws, and the driver is told to show it.
     * (The functions are those the panic path calls; here the kernel goes on afterwards.)
     */
    const uint32_t marker = 0x00123456;
    uint32_t stride = d->info.pitch / 4, corner = 239 * stride + 319, below = 240 * stride;
    uint32_t below_before = d->pixels[below];
    d->pixels[0] = marker;
    d->pixels[corner] = marker;
    fake_modes.panics = 0;
    display_panic_prepare();
    KEXPECT(d->pixels[corner] == marker && fake_modes.panics == 0); /* nothing on the screen yet */
    klog_raw("ktest: a line as a panic writes it\n");
    display_panic_show();
    KEXPECT(fake_modes.panics == 1);
    KEXPECT(d->pixels[0] != marker && d->pixels[corner] != marker); /* the console drew over the whole 320x240 */
    KEXPECT(d->pixels[below] == below_before);                      /* and not as for the 640x480 before */
    fb_console_set_active(false); /* the display server's again */
    object_release(memory);
    KEXPECT(!d->console_stale && !d->acquired);

    /* Hot plug: the flag and the event. */
    event_reset(event);
    display_set_connected(0, false);
    KEXPECT((d->info.flags & JELLY_DISPLAY_DISCONNECTED) && event_is_signaled(event));
    display_set_connected(0, true);
    KEXPECT(!(d->info.flags & JELLY_DISPLAY_DISCONNECTED));
    /* Another monitor: a new list in which the mode being shown does not appear. */
    KASSERT(display_set_modes(0, fake_modes.list, 2, DISPLAY_NO_MODE) == STATUS_SUCCESS);
    KEXPECT(display_modes(0, modes, 4, &count) == STATUS_SUCCESS && count == 2 && !(modes[0].flags & JELLY_MODE_CURRENT));

    /* The real screen back. */
    KASSERT(display_set_mode(0, 0) == STATUS_SUCCESS);
    KEXPECT(d->info.width == width && d->info.height == height && d->info.pitch == pitch);
    object_release(event);
    real_driver_restore();
}

/* --- The screen off and on ------------------------------------------------------------------ */

static struct {
    int  offs, ons;
    bool fails;
} fake_power;

static status_t fake_power_set(display_t *display, bool on)
{
    (void)display;
    if (fake_power.fails)
        return STATUS_DEVICE_ERROR;
    if (on)
        fake_power.ons++;
    else
        fake_power.offs++;
    return STATUS_SUCCESS;
}

KTEST(display_screen_goes_off_and_comes_back)
{
    static const display_ops_t plain = { .set_mode = fake_set_mode };
    static const display_ops_t ops = { .set_mode = fake_set_mode, .power = fake_power_set };
    display_t *d = display_get(0);
    object_t *event, *memory;

    if (!d || d->info.width < 640 || d->info.height < 480)
        return;
    real_driver_save(d);
    memset(&fake_power, 0, sizeof(fake_power));
    fake_modes.fails = false;
    fake_modes.list[0] = (jelly_display_mode_t){ d->info.width, d->info.height, 60000, JELLY_MODE_PREFERRED };
    fake_modes.list[1] = (jelly_display_mode_t){ 640, 480, 75000, 0 };
    fake_modes.pitch[0] = d->info.pitch;
    fake_modes.pitch[1] = 640 * 4;

    /* A driver that cannot: said so, by the flag and by the call. */
    KASSERT(display_set_driver(0, &plain, NULL, 0) == STATUS_SUCCESS);
    KEXPECT(!(d->info.flags & JELLY_DISPLAY_POWER) && display_set_power(0, false) == STATUS_NOT_SUPPORTED);
    KEXPECT(display_set_power(99, false) == STATUS_NOT_FOUND);

    /* Off: the driver is asked once, the flag and the watcher's event tell. */
    KASSERT(display_set_driver(0, &ops, NULL, 0) == STATUS_SUCCESS);
    KASSERT(display_set_modes(0, fake_modes.list, 2, 0) == STATUS_SUCCESS);
    KEXPECT((d->info.flags & JELLY_DISPLAY_POWER) && !(d->info.flags & JELLY_DISPLAY_OFF));
    KASSERT(display_watch(0, &event) == STATUS_SUCCESS);
    event_reset(event);
    KEXPECT(display_set_power(0, true) == STATUS_SUCCESS && fake_power.ons == 0); /* it is on */
    KEXPECT(display_set_power(0, false) == STATUS_SUCCESS && fake_power.offs == 1);
    KEXPECT((d->info.flags & JELLY_DISPLAY_OFF) && event_is_signaled(event));
    KEXPECT(display_set_power(0, false) == STATUS_SUCCESS && fake_power.offs == 1);

    /*
     * Input brings it back, a moment later (a thread does it): not in the first half second, when the hand
     * that switched it off is still on the mouse, and not a key that goes up.
     */
    event_reset(event);
    input_report(0, JELLY_INPUT_MOUSE_MOVE, 0, 0, 1, 1, 0, 0, 0);
    thread_sleep(20000000);
    KEXPECT((d->info.flags & JELLY_DISPLAY_OFF) && fake_power.ons == 0);
    thread_sleep(600000000);
    input_report(0, JELLY_INPUT_KEY_UP, JELLY_KEY_A, 0, 0, 0, 0, 0, 0);
    input_report(0, JELLY_INPUT_MOUSE_BUTTON, 1, 0, 0, 0, 0, 0, 0);
    thread_sleep(20000000);
    KEXPECT((d->info.flags & JELLY_DISPLAY_OFF) && fake_power.ons == 0);
    input_report(0, JELLY_INPUT_KEY_DOWN, JELLY_KEY_A, 1, 0, 0, 0, 0, 0);
    for (int i = 0; i < 200 && (d->info.flags & JELLY_DISPLAY_OFF); i++)
        thread_sleep(1000000);
    KEXPECT(!(d->info.flags & JELLY_DISPLAY_OFF) && fake_power.ons == 1 && event_is_signaled(event));
    /* Input while the screen is on asks the driver for nothing. */
    input_report(0, JELLY_INPUT_MOUSE_MOVE, 0, 0, 1, 1, 0, 0, 0);
    thread_sleep(5000000);
    KEXPECT(fake_power.ons == 1 && fake_power.offs == 1);

    /* A driver that cannot switch off right now: the screen stays on. */
    fake_power.fails = true;
    KEXPECT(display_set_power(0, false) == STATUS_DEVICE_ERROR && !(d->info.flags & JELLY_DISPLAY_OFF));
    fake_power.fails = false;

    /* A change of the mode is something to look at: on first. */
    KASSERT(display_set_power(0, false) == STATUS_SUCCESS);
    KASSERT(display_set_mode(0, 1) == STATUS_SUCCESS);
    KEXPECT(!(d->info.flags & JELLY_DISPLAY_OFF) && fake_power.ons == 2 && d->info.width == 640);
    KASSERT(display_set_mode(0, 0) == STATUS_SUCCESS);

    /* A display server that goes away with the screen off leaves it on for the console. */
    KASSERT(display_acquire(0, &memory) == STATUS_SUCCESS);
    KASSERT(display_set_power(0, false) == STATUS_SUCCESS);
    object_release(memory);
    KEXPECT(!(d->info.flags & JELLY_DISPLAY_OFF) && fake_power.ons == 3 && !d->acquired);

    object_release(event);
    real_driver_restore();
}
