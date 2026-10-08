/*
 * Input manager (README section 37).
 *
 * Every open queue has a ring of QUEUE_SIZE events; when a reader falls
 * behind, the oldest events are dropped (a lost mouse move matters less
 * than a stuck reader). Reports come from interrupt handlers, so the queues
 * are protected by disabling interrupts.
 */

#include "input/input.h"

#include "core/arch.h"
#include "core/log.h"
#include "core/string.h"
#include "memory/heap.h"
#include "time/clock.h"

#define QUEUE_SIZE 512
#define DEVICE_MAX 16

typedef struct {
    object_t            object;
    list_node_t         node;
    jelly_input_event_t events[QUEUE_SIZE];
    uint32_t            head, count;
    uint64_t            dropped;
} input_queue_t;

static list_t queues = { { &queues.head, &queues.head } };
static uint32_t devices;

uint32_t input_register_device(const char *name)
{
    uint32_t number = devices < DEVICE_MAX ? devices++ : DEVICE_MAX - 1;
    klog_info("input: device %u: %s", number, name);
    return number;
}

uint32_t input_device_count(void)
{
    return devices;
}

static void (*activity_hook)(uint32_t type, int32_t value);

void input_set_activity_hook(void (*hook)(uint32_t type, int32_t value))
{
    activity_hook = hook;
}

void input_report(uint32_t device, uint32_t type, uint32_t code, int32_t value, int32_t dx, int32_t dy, int32_t x,
                  int32_t y, uint32_t flags)
{
    jelly_input_event_t event = {
        .time_ns = clock_monotonic_ns(),
        .type = type,
        .code = code,
        .value = value,
        .dx = dx,
        .dy = dy,
        .x = x,
        .y = y,
        .flags = flags,
        .device = device,
    };
    uint64_t saved = arch_interrupts_save();
    list_for_each(node, &queues) {
        input_queue_t *q = container_of(node, input_queue_t, node);
        if (q->count == QUEUE_SIZE) {
            q->head = (q->head + 1) % QUEUE_SIZE;
            q->count--;
            q->dropped++;
        }
        q->events[(q->head + q->count) % QUEUE_SIZE] = event;
        q->count++;
        object_notify(&q->object);
    }
    if (activity_hook)
        activity_hook(type, value);
    arch_interrupts_restore(saved);
}

static input_queue_t *queue_of(object_t *object)
{
    return container_of(object, input_queue_t, object);
}

static bool queue_signaled(object_t *object)
{
    return queue_of(object)->count > 0;
}

static void queue_destroy(object_t *object)
{
    input_queue_t *q = queue_of(object);
    uint64_t saved = arch_interrupts_save();
    list_remove(&q->node);
    arch_interrupts_restore(saved);
    kfree(q);
}

static const object_ops_t queue_ops = {
    .destroy = queue_destroy,
    .signaled = queue_signaled,
};

status_t input_open(object_t **out)
{
    input_queue_t *q = kcalloc(1, sizeof(*q));
    if (!q)
        return STATUS_OUT_OF_MEMORY;
    object_init(&q->object, OBJECT_INPUT, &queue_ops);
    uint64_t saved = arch_interrupts_save();
    list_push_back(&queues, &q->node);
    arch_interrupts_restore(saved);
    *out = &q->object;
    return STATUS_SUCCESS;
}

size_t input_read(object_t *object, jelly_input_event_t *events, size_t count)
{
    input_queue_t *q = queue_of(object);
    size_t taken = 0;
    uint64_t saved = arch_interrupts_save();
    while (taken < count && q->count) {
        events[taken++] = q->events[q->head];
        q->head = (q->head + 1) % QUEUE_SIZE;
        q->count--;
    }
    arch_interrupts_restore(saved);
    return taken;
}
