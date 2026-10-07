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
    const uint32_t width = 1000, height = 500, pitch = 4032; /* a pitch wider than the picture, as hardware has it */
    size_t pages = (size_t)(align_up((uint64_t)pitch * height, PAGE_SIZE) / PAGE_SIZE);
    KASSERT(pmm_alloc_pages(pages, &phys) == STATUS_SUCCESS);
    volatile uint32_t *memory_view = phys_to_virt(phys);

    KEXPECT(display_set_framebuffer(99, phys, width, height, pitch) == STATUS_NOT_FOUND);
    KEXPECT(display_set_framebuffer(0, phys, width, height, width * 4 - 4) == STATUS_INVALID_ARGUMENT);
    KEXPECT(display_set_framebuffer(0, phys + 1, width, height, pitch) == STATUS_INVALID_ARGUMENT);

    KASSERT(display_set_framebuffer(0, phys, width, height, pitch) == STATUS_SUCCESS);
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
    KEXPECT(display_set_framebuffer(0, old_phys, old_width, old_height, old_pitch) == STATUS_BUSY);
    object_release(memory);

    KASSERT(display_set_framebuffer(0, old_phys, old_width, old_height, old_pitch) == STATUS_SUCCESS);
    KEXPECT(d->info.width == old_width && d->info.height == old_height && d->phys == old_phys);
    pmm_free_pages(phys, pages);
}
