/*
 * System calls of ABI version 5: waiting for several objects, handles in
 * channel messages, named services, displays and input.
 */

#include "syscall/internal.h"

#include "core/string.h"
#include "drivers/graphics/display.h"
#include "input/input.h"
#include "ipc/ipc.h"
#include "memory/heap.h"
#include "process/process.h"
#include "process/usercopy.h"
#include "core/string.h"
#include "core/version.h"
#include "memory/pmm.h"
#include "time/clock.h"
#include "time/rtc.h"

#include <jelly/syscall.h>

static bool is_root(void)
{
    return process_current()->credentials.uid == UID_ROOT;
}

/* --- Waiting for several objects ------------------------------------------------ */

status_t sys_object_wait_many(const uint64_t *a)
{
    jelly_handle_t handles[JELLY_WAIT_MANY_MAX];
    object_t *objects[JELLY_WAIT_MANY_MAX];
    uint32_t count = (uint32_t)a[1], index = 0, got = 0;

    if (count == 0 || count > JELLY_WAIT_MANY_MAX || !user_range_ok(a[3], sizeof(uint32_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = copy_from_user(handles, a[0], count * sizeof(jelly_handle_t));
    for (; !STATUS_IS_ERROR(status) && got < count; got++)
        status = handle_get(syscall_handles(), handles[got], 0, JELLY_RIGHT_WAIT, &objects[got], NULL);
    if (STATUS_IS_ERROR(status))
        got = got ? got - 1 : 0; /* the failed one holds no reference */
    else
        status = object_wait_many(objects, count, a[2], &index);
    for (uint32_t i = 0; i < got; i++)
        object_release(objects[i]);
    return STATUS_IS_ERROR(status) ? status : put_user_u32(a[3], index);
}

/* --- Handles in channel messages ------------------------------------------------ */

status_t sys_channel_send_handles(const uint64_t *a)
{
    jelly_handle_t handles[JELLY_CHANNEL_MAX_HANDLES];
    object_t *objects[JELLY_CHANNEL_MAX_HANDLES];
    uint32_t rights[JELLY_CHANNEL_MAX_HANDLES];
    uint32_t count = (uint32_t)a[4], got = 0;
    object_t *endpoint;

    if (count > JELLY_CHANNEL_MAX_HANDLES || a[2] > CHANNEL_MESSAGE_MAX)
        return STATUS_INVALID_ARGUMENT;
    status_t status = copy_from_user(handles, a[3], count * sizeof(jelly_handle_t));
    if (STATUS_IS_ERROR(status))
        return status;
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = 0; j < i; j++) {
            if (handles[i] == handles[j])
                return STATUS_INVALID_ARGUMENT; /* each handle moves once */
        }
    }
    status = handle_get(syscall_handles(), (handle_t)a[0], OBJECT_CHANNEL, JELLY_RIGHT_WRITE, &endpoint, NULL);
    if (STATUS_IS_ERROR(status))
        return status;
    for (; got < count; got++) {
        status = handle_get(syscall_handles(), handles[got], 0, 0, &objects[got], &rights[got]);
        if (STATUS_IS_ERROR(status))
            break;
    }

    void *data = NULL;
    if (!STATUS_IS_ERROR(status)) {
        data = kmalloc(a[2] ? a[2] : 1);
        status = data ? copy_from_user(data, a[1], a[2]) : STATUS_OUT_OF_MEMORY;
    }
    if (!STATUS_IS_ERROR(status))
        status = channel_send_objects(endpoint, data, a[2], objects, rights, count);
    if (STATUS_IS_ERROR(status)) {
        kfree(data);
        for (uint32_t i = 0; i < got; i++)
            object_release(objects[i]);
    } else {
        /* Moved: the message holds the references now, the sender loses its handles. */
        for (uint32_t i = 0; i < count; i++)
            handle_close(syscall_handles(), handles[i]);
    }
    object_release(endpoint);
    return status;
}

status_t sys_channel_receive_handles(const uint64_t *a)
{
    object_t *objects[JELLY_CHANNEL_MAX_HANDLES];
    uint32_t rights[JELLY_CHANNEL_MAX_HANDLES], count = 0;
    jelly_handle_t handles[JELLY_CHANNEL_MAX_HANDLES];
    object_t *endpoint;
    size_t size;

    if (!user_range_ok(a[3], sizeof(uint64_t), true) ||
        !user_range_ok(a[4], JELLY_CHANNEL_MAX_HANDLES * sizeof(jelly_handle_t), true) ||
        !user_range_ok(a[5], sizeof(uint32_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = handle_get(syscall_handles(), (handle_t)a[0], OBJECT_CHANNEL, JELLY_RIGHT_READ, &endpoint, NULL);
    if (STATUS_IS_ERROR(status))
        return status;

    status = channel_peek_objects(endpoint, &size, &count);
    if (!STATUS_IS_ERROR(status) && size > a[2]) {
        put_user_u64(a[3], size);
        status = STATUS_BUFFER_TOO_SMALL;
    }
    if (!STATUS_IS_ERROR(status) && size && !user_range_ok(a[1], size, true))
        status = STATUS_INVALID_ARGUMENT;

    if (!STATUS_IS_ERROR(status)) {
        void *data;
        channel_take_objects(endpoint, &data, &size, objects, rights, &count);
        copy_to_user(a[1], data, size); /* checked above */
        kfree(data);
        uint32_t installed = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!STATUS_IS_ERROR(status))
                status = handle_install(syscall_handles(), objects[i], rights[i], &handles[i]);
            if (!STATUS_IS_ERROR(status))
                installed++;
            object_release(objects[i]); /* the handle (if any) holds its own reference */
        }
        if (STATUS_IS_ERROR(status)) {
            for (uint32_t i = 0; i < installed; i++)
                handle_close(syscall_handles(), handles[i]);
            installed = 0;
        }
        put_user_u64(a[3], size);
        copy_to_user(a[4], handles, installed * sizeof(jelly_handle_t));
        put_user_u32(a[5], installed);
    }
    object_release(endpoint);
    return status;
}

/* --- Named services -------------------------------------------------------------- */

static status_t copy_name(uint64_t pointer, uint64_t length, char *name)
{
    if (length == 0 || length > JELLY_SERVICE_NAME_MAX)
        return STATUS_INVALID_ARGUMENT;
    status_t status = copy_from_user(name, pointer, length);
    name[length] = '\0';
    if (!STATUS_IS_ERROR(status) && strlen(name) != length)
        status = STATUS_INVALID_ARGUMENT;
    return status;
}

status_t sys_service_register(const uint64_t *a)
{
    char name[JELLY_SERVICE_NAME_MAX + 1];
    object_t *channel;

    if (!is_root())
        return STATUS_ACCESS_DENIED;
    status_t status = copy_name(a[0], a[1], name);
    if (STATUS_IS_ERROR(status))
        return status;
    status = handle_get(syscall_handles(), (handle_t)a[2], OBJECT_CHANNEL, JELLY_RIGHT_WRITE, &channel, NULL);
    if (STATUS_IS_ERROR(status))
        return status;
    status = service_register(name, channel);
    object_release(channel);
    if (!STATUS_IS_ERROR(status))
        handle_close(syscall_handles(), (handle_t)a[2]); /* the registry owns this end now */
    return status;
}

status_t sys_service_connect(const uint64_t *a)
{
    char name[JELLY_SERVICE_NAME_MAX + 1];
    object_t *channel;

    if (!user_range_ok(a[2], sizeof(jelly_handle_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = copy_name(a[0], a[1], name);
    if (!STATUS_IS_ERROR(status))
        status = service_connect(name, &channel);
    if (STATUS_IS_ERROR(status))
        return status;
    return syscall_give_handle(channel, JELLY_RIGHT_READ | JELLY_RIGHT_WRITE | JELLY_RIGHT_WAIT | JELLY_RIGHT_DUPLICATE,
                               a[2]);
}

/* --- Displays and input ---------------------------------------------------------- */

status_t sys_display_info(const uint64_t *a)
{
    display_t *d = display_get((uint32_t)a[0]);
    if (!d)
        return STATUS_NOT_FOUND;
    return copy_to_user(a[1], &d->info, sizeof(d->info));
}

status_t sys_display_acquire(const uint64_t *a)
{
    object_t *memory;
    if (!is_root())
        return STATUS_ACCESS_DENIED;
    if (!user_range_ok(a[1], sizeof(jelly_handle_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = display_acquire((uint32_t)a[0], &memory);
    if (STATUS_IS_ERROR(status))
        return status;
    return syscall_give_handle(memory, JELLY_RIGHT_MAP | JELLY_RIGHT_WRITE, a[1]);
}

status_t sys_input_open(const uint64_t *a)
{
    object_t *queue;
    if (!is_root())
        return STATUS_ACCESS_DENIED;
    if (!user_range_ok(a[0], sizeof(jelly_handle_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = input_open(&queue);
    if (STATUS_IS_ERROR(status))
        return status;
    return syscall_give_handle(queue, JELLY_RIGHT_READ | JELLY_RIGHT_WAIT | JELLY_RIGHT_DUPLICATE, a[0]);
}

status_t sys_input_read(const uint64_t *a)
{
    jelly_input_event_t events[32];
    object_t *queue;
    size_t count = a[2] < 32 ? a[2] : 32;

    if (!user_range_ok(a[3], sizeof(uint64_t), true) || (count && !user_range_ok(a[1], count * sizeof(events[0]), true)))
        return STATUS_INVALID_ARGUMENT;
    status_t status = handle_get(syscall_handles(), (handle_t)a[0], OBJECT_INPUT, JELLY_RIGHT_READ, &queue, NULL);
    if (STATUS_IS_ERROR(status))
        return status;
    size_t taken = input_read(queue, events, count);
    object_release(queue);
    if (taken == 0)
        return count ? STATUS_WOULD_BLOCK : put_user_u64(a[3], 0);
    status = copy_to_user(a[1], events, taken * sizeof(events[0]));
    return STATUS_IS_ERROR(status) ? status : put_user_u64(a[3], taken);
}

/* --- Desktop: time and system information (ABI 6) ---------------------------- */

status_t sys_clock_realtime(const uint64_t *a)
{
    if (!rtc_available())
        return STATUS_NOT_SUPPORTED;
    return put_user_u64(a[0], clock_realtime_ns());
}

status_t sys_system_info(const uint64_t *a)
{
    jelly_system_info_t info;
    pmm_stats_t memory;
    memset(&info, 0, sizeof(info));
    strcpy(info.version, KERNEL_VERSION_STRING);
    pmm_get_stats(&memory);
    info.uptime_ns = clock_monotonic_ns();
    info.memory_total = memory.total_bytes;
    info.memory_free = memory.free_bytes;
    info.processes = process_live_count();
    info.abi_version = JELLY_SYSCALL_ABI_VERSION;
    info.realtime_ns = clock_realtime_ns();
    return copy_to_user(a[0], &info, sizeof(info));
}

/* --- Graphics driver features of a display (ABI 8) ----------------------------- */

status_t sys_display_cursor(const uint64_t *a)
{
    jelly_cursor_t cursor;
    uint32_t *pixels = NULL;
    size_t bytes = JELLY_CURSOR_SIZE * JELLY_CURSOR_SIZE * sizeof(uint32_t);

    if (!is_root())
        return STATUS_ACCESS_DENIED;
    status_t status = copy_from_user(&cursor, a[1], sizeof(cursor));
    if (STATUS_IS_ERROR(status))
        return status;
    if (cursor.flags & JELLY_CURSOR_IMAGE) {
        pixels = kmalloc(bytes);
        if (!pixels)
            return STATUS_OUT_OF_MEMORY;
        status = copy_from_user(pixels, (uint64_t)(uintptr_t)cursor.pixels, bytes);
    }
    if (!STATUS_IS_ERROR(status))
        status = display_cursor((uint32_t)a[0], &cursor, pixels);
    kfree(pixels);
    return status;
}

status_t sys_display_vblank(const uint64_t *a)
{
    if (!is_root())
        return STATUS_ACCESS_DENIED;
    return display_wait_vblank((uint32_t)a[0], a[1]);
}

status_t sys_display_buffer(const uint64_t *a)
{
    object_t *memory;

    if (!is_root())
        return STATUS_ACCESS_DENIED;
    if (!user_range_ok(a[2], sizeof(jelly_handle_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = display_buffer((uint32_t)a[0], (uint32_t)a[1], &memory);
    if (STATUS_IS_ERROR(status))
        return status;
    return syscall_give_handle(memory, JELLY_RIGHT_MAP | JELLY_RIGHT_WRITE, a[2]);
}

status_t sys_display_flip(const uint64_t *a)
{
    if (!is_root())
        return STATUS_ACCESS_DENIED;
    return display_flip((uint32_t)a[0], (uint32_t)a[1]);
}

/* --- Display modes and hot plug (ABI 9) ------------------------------------------ */

status_t sys_display_modes(const uint64_t *a)
{
    jelly_display_mode_t modes[JELLY_DISPLAY_MODE_MAX];
    uint32_t max = a[2] < JELLY_DISPLAY_MODE_MAX ? (uint32_t)a[2] : JELLY_DISPLAY_MODE_MAX, count = 0;

    if (!user_range_ok(a[3], sizeof(uint32_t), true) || (max && !user_range_ok(a[1], max * sizeof(modes[0]), true)))
        return STATUS_INVALID_ARGUMENT;
    status_t status = display_modes((uint32_t)a[0], modes, max, &count);
    if (STATUS_IS_ERROR(status))
        return status;
    uint32_t stored = count < max ? count : max;
    if (stored)
        status = copy_to_user(a[1], modes, stored * sizeof(modes[0]));
    return STATUS_IS_ERROR(status) ? status : put_user_u32(a[3], count);
}

status_t sys_display_set_mode(const uint64_t *a)
{
    if (!is_root())
        return STATUS_ACCESS_DENIED;
    return display_set_mode((uint32_t)a[0], (uint32_t)a[1]);
}

status_t sys_display_watch(const uint64_t *a)
{
    object_t *event;

    if (!is_root())
        return STATUS_ACCESS_DENIED;
    if (!user_range_ok(a[1], sizeof(jelly_handle_t), true))
        return STATUS_INVALID_ARGUMENT;
    status_t status = display_watch((uint32_t)a[0], &event);
    if (STATUS_IS_ERROR(status))
        return status;
    return syscall_give_handle(event, JELLY_RIGHT_WAIT | JELLY_RIGHT_SIGNAL | JELLY_RIGHT_DUPLICATE, a[1]);
}
