/*
 * System call implementations.
 *
 * Every handler validates its arguments, resolves handles with the rights it
 * needs, and returns a status_t. Output values go through copy_to_user, and
 * nothing is written unless the call succeeds.
 */

#include "syscall/syscall.h"
#include "syscall/internal.h"

#include "core/handle.h"
#include "core/log.h"
#include "ipc/ipc.h"
#include "memory/heap.h"
#include "memory/layout.h"
#include "process/process.h"
#include "process/usercopy.h"
#include "scheduler/scheduler.h"
#include "time/clock.h"

#include <jelly/syscall.h>

#define DEBUG_WRITE_MAX 256

typedef status_t (*syscall_fn)(const uint64_t *a);

handle_table_t *syscall_handles(void)
{
    return &process_current()->handles;
}

status_t syscall_give_handle(object_t *object, uint32_t rights, uint64_t user_out)
{
    handle_t handle;
    status_t status = handle_install(syscall_handles(), object, rights, &handle);

    object_release(object);
    if (STATUS_IS_ERROR(status))
        return status;
    status = put_user_u32(user_out, handle);
    if (STATUS_IS_ERROR(status))
        handle_close(syscall_handles(), handle);
    return status;
}

static uint32_t memory_flags(uint32_t flags, status_t *status)
{
    *status = STATUS_SUCCESS;
    if ((flags & ~(JELLY_MEMORY_WRITE | JELLY_MEMORY_EXEC)) ||
        ((flags & JELLY_MEMORY_WRITE) && (flags & JELLY_MEMORY_EXEC))) {
        *status = STATUS_INVALID_ARGUMENT; /* W^X */
        return 0;
    }
    return ((flags & JELLY_MEMORY_WRITE) ? VM_WRITE : 0) | ((flags & JELLY_MEMORY_EXEC) ? VM_EXEC : 0);
}

/* --- Basics ------------------------------------------------------------------ */

static status_t sys_abi_version(const uint64_t *a)
{
    return put_user_u32(a[0], JELLY_SYSCALL_ABI_VERSION);
}

static status_t sys_debug_write(const uint64_t *a)
{
    char text[DEBUG_WRITE_MAX + 1];
    uint64_t length = a[1] < DEBUG_WRITE_MAX ? a[1] : DEBUG_WRITE_MAX;
    status_t status = copy_from_user(text, a[0], length);

    if (STATUS_IS_ERROR(status))
        return status;
    while (length && (text[length - 1] == '\n' || text[length - 1] == '\r'))
        length--;
    text[length] = '\0';

    process_t *p = process_current();
    klog_info("[%s:%lu] %s", p->name, p->pid, text);
    return STATUS_SUCCESS;
}

static status_t sys_clock_monotonic(const uint64_t *a)
{
    return put_user_u64(a[0], clock_monotonic_ns());
}

/* --- Processes and threads --------------------------------------------------- */

static status_t sys_process_exit(const uint64_t *a)
{
    process_exit_current((int32_t)a[0], "");
}

static status_t sys_thread_create(const uint64_t *a)
{
    thread_t *t;
    process_t *p = process_current();

    if (a[0] < USER_SPACE_START || a[0] >= USER_SPACE_END || a[1] < USER_SPACE_START || a[1] > USER_SPACE_END)
        return STATUS_INVALID_ARGUMENT;

    /* Validate the output pointer first so a started thread is never lost. */
    if (!user_range_ok(a[4], sizeof(handle_t), true))
        return STATUS_INVALID_ARGUMENT;

    uint64_t sp = (a[1] & ~15ULL) - 8; /* RSP = 8 mod 16, as after a call */
    status_t status = thread_create_user(p, "thread", a[0], sp, a[2], a[3], 0, &t);
    if (STATUS_IS_ERROR(status))
        return status;

    /* Only start the thread once the caller holds its handle. */
    object_retain(&t->object);
    status = syscall_give_handle(&t->object, JELLY_RIGHT_WAIT | JELLY_RIGHT_DUPLICATE, a[4]);
    if (!STATUS_IS_ERROR(status))
        thread_start(t);
    object_release(&t->object); /* a thread that never started is destroyed here */
    return status;
}

static status_t sys_thread_exit(const uint64_t *a)
{
    (void)a;
    thread_exit();
}

static status_t sys_thread_yield(const uint64_t *a)
{
    (void)a;
    scheduler_yield();
    return STATUS_SUCCESS;
}

static status_t sys_thread_sleep(const uint64_t *a)
{
    return thread_sleep(a[0]);
}

/* --- Memory ------------------------------------------------------------------ */

static status_t sys_memory_allocate(const uint64_t *a)
{
    status_t status;
    uint32_t flags = memory_flags((uint32_t)a[1], &status);
    uint64_t address;

    if (STATUS_IS_ERROR(status))
        return status;
    if (!user_range_ok(a[2], sizeof(uint64_t), true))
        return STATUS_INVALID_ARGUMENT;

    process_t *p = process_current();
    status = process_memory_allocate(p, a[0], flags, &address);
    if (!STATUS_IS_ERROR(status))
        status = put_user_u64(a[2], address);
    return status;
}

static status_t sys_memory_unmap(const uint64_t *a)
{
    return process_memory_unmap(process_current(), a[0], a[1]);
}

/* --- Handles and waiting ----------------------------------------------------- */

static status_t sys_handle_close(const uint64_t *a)
{
    return handle_close(syscall_handles(), (handle_t)a[0]);
}

static status_t sys_handle_duplicate(const uint64_t *a)
{
    object_t *object;
    uint32_t held;
    status_t status = handle_get(syscall_handles(), (handle_t)a[0], 0, JELLY_RIGHT_DUPLICATE, &object, &held);

    if (STATUS_IS_ERROR(status))
        return status;
    uint32_t rights = (uint32_t)a[1];
    if (rights & ~held) {
        object_release(object);
        return STATUS_ACCESS_DENIED; /* rights can only be reduced */
    }
    return syscall_give_handle(object, rights, a[2]);
}

static status_t sys_object_wait(const uint64_t *a)
{
    object_t *object;
    status_t status = handle_get(syscall_handles(), (handle_t)a[0], 0, JELLY_RIGHT_WAIT, &object, NULL);

    if (STATUS_IS_ERROR(status))
        return status;
    status = object_wait(object, a[1]);
    object_release(object);
    return status;
}

/* --- Events ------------------------------------------------------------------ */

static status_t sys_event_create(const uint64_t *a)
{
    object_t *event;
    status_t status = event_create((uint32_t)a[0], &event);

    if (STATUS_IS_ERROR(status))
        return status;
    return syscall_give_handle(event, JELLY_RIGHT_WAIT | JELLY_RIGHT_SIGNAL | JELLY_RIGHT_DUPLICATE, a[1]);
}

static status_t with_event(handle_t handle, void (*action)(object_t *))
{
    object_t *event;
    status_t status = handle_get(syscall_handles(), handle, OBJECT_EVENT, JELLY_RIGHT_SIGNAL, &event, NULL);

    if (STATUS_IS_ERROR(status))
        return status;
    action(event);
    object_release(event);
    return STATUS_SUCCESS;
}

static status_t sys_event_signal(const uint64_t *a)
{
    return with_event((handle_t)a[0], event_signal);
}

static status_t sys_event_reset(const uint64_t *a)
{
    return with_event((handle_t)a[0], event_reset);
}

/* --- Channels ---------------------------------------------------------------- */

#define CHANNEL_RIGHTS (JELLY_RIGHT_READ | JELLY_RIGHT_WRITE | JELLY_RIGHT_WAIT | JELLY_RIGHT_DUPLICATE)

static status_t sys_channel_create(const uint64_t *a)
{
    object_t *end0, *end1;

    if (!user_range_ok(a[0], sizeof(handle_t), true) || !user_range_ok(a[1], sizeof(handle_t), true))
        return STATUS_INVALID_ARGUMENT;

    status_t status = channel_create(&end0, &end1);
    if (STATUS_IS_ERROR(status))
        return status;

    status = syscall_give_handle(end0, CHANNEL_RIGHTS, a[0]);
    if (STATUS_IS_ERROR(status)) {
        object_release(end1);
        return status;
    }
    return syscall_give_handle(end1, CHANNEL_RIGHTS, a[1]);
}

static status_t sys_channel_send(const uint64_t *a)
{
    object_t *ep;
    status_t status = handle_get(syscall_handles(), (handle_t)a[0], OBJECT_CHANNEL, JELLY_RIGHT_WRITE, &ep, NULL);

    if (STATUS_IS_ERROR(status))
        return status;
    if (a[2] > CHANNEL_MESSAGE_MAX) {
        object_release(ep);
        return STATUS_INVALID_ARGUMENT;
    }

    void *data = kmalloc(a[2] ? a[2] : 1);
    if (!data)
        status = STATUS_OUT_OF_MEMORY;
    else
        status = copy_from_user(data, a[1], a[2]);
    if (!STATUS_IS_ERROR(status))
        status = channel_send(ep, data, a[2]);
    if (STATUS_IS_ERROR(status))
        kfree(data);
    object_release(ep);
    return status;
}

static status_t sys_channel_receive(const uint64_t *a)
{
    object_t *ep;
    size_t size;
    status_t status = handle_get(syscall_handles(), (handle_t)a[0], OBJECT_CHANNEL, JELLY_RIGHT_READ, &ep, NULL);

    if (STATUS_IS_ERROR(status))
        return status;

    status = channel_peek(ep, &size);
    if (!STATUS_IS_ERROR(status) && !user_range_ok(a[3], sizeof(uint64_t), true))
        status = STATUS_INVALID_ARGUMENT;
    if (!STATUS_IS_ERROR(status) && size > a[2]) {
        put_user_u64(a[3], size); /* tell the caller how much room is needed */
        status = STATUS_BUFFER_TOO_SMALL;
    }
    if (!STATUS_IS_ERROR(status) && size && !user_range_ok(a[1], size, true))
        status = STATUS_INVALID_ARGUMENT;

    if (!STATUS_IS_ERROR(status)) {
        void *data;
        channel_take(ep, &data, &size);
        copy_to_user(a[1], data, size); /* checked above, cannot fail */
        put_user_u64(a[3], size);
        kfree(data);
    }
    object_release(ep);
    return status;
}

/* --- Shared memory ----------------------------------------------------------- */

static status_t sys_shm_create(const uint64_t *a)
{
    object_t *shm;
    status_t status = shm_create(a[0], &shm);

    if (STATUS_IS_ERROR(status))
        return status;
    return syscall_give_handle(shm, JELLY_RIGHT_MAP | JELLY_RIGHT_WRITE | JELLY_RIGHT_DUPLICATE, a[1]);
}

static status_t sys_shm_map(const uint64_t *a)
{
    status_t status;
    uint32_t flags = memory_flags((uint32_t)a[1], &status);
    uint32_t needed = JELLY_RIGHT_MAP | ((flags & VM_WRITE) ? JELLY_RIGHT_WRITE : 0);
    object_t *shm;
    uint64_t address;

    if (STATUS_IS_ERROR(status))
        return status;
    if (flags & VM_EXEC)
        return STATUS_ACCESS_DENIED; /* shared memory is data only */
    if (!user_range_ok(a[2], sizeof(uint64_t), true))
        return STATUS_INVALID_ARGUMENT;

    status = handle_get(syscall_handles(), (handle_t)a[0], OBJECT_SHARED_MEMORY, needed, &shm, NULL);
    if (STATUS_IS_ERROR(status))
        return status;
    status = shm_map(process_current(), shm, flags, &address);
    object_release(shm);
    if (!STATUS_IS_ERROR(status))
        status = put_user_u64(a[2], address);
    return status;
}

/* --- Futex ------------------------------------------------------------------- */

static status_t sys_futex_wait(const uint64_t *a)
{
    return futex_wait(a[0], (uint32_t)a[1], a[2]);
}

static status_t sys_futex_wake(const uint64_t *a)
{
    uint32_t woken;
    return futex_wake(a[0], (uint32_t)a[1], &woken);
}

/* --- Dispatch ---------------------------------------------------------------- */

static const syscall_fn table[SYS_COUNT] = {
    [SYS_ABI_VERSION]      = sys_abi_version,
    [SYS_DEBUG_WRITE]      = sys_debug_write,
    [SYS_CLOCK_MONOTONIC]  = sys_clock_monotonic,
    [SYS_PROCESS_EXIT]     = sys_process_exit,
    [SYS_THREAD_CREATE]    = sys_thread_create,
    [SYS_THREAD_EXIT]      = sys_thread_exit,
    [SYS_THREAD_YIELD]     = sys_thread_yield,
    [SYS_THREAD_SLEEP]     = sys_thread_sleep,
    [SYS_MEMORY_ALLOCATE]  = sys_memory_allocate,
    [SYS_MEMORY_UNMAP]     = sys_memory_unmap,
    [SYS_HANDLE_CLOSE]     = sys_handle_close,
    [SYS_HANDLE_DUPLICATE] = sys_handle_duplicate,
    [SYS_OBJECT_WAIT]      = sys_object_wait,
    [SYS_EVENT_CREATE]     = sys_event_create,
    [SYS_EVENT_SIGNAL]     = sys_event_signal,
    [SYS_EVENT_RESET]      = sys_event_reset,
    [SYS_CHANNEL_CREATE]   = sys_channel_create,
    [SYS_CHANNEL_SEND]     = sys_channel_send,
    [SYS_CHANNEL_RECEIVE]  = sys_channel_receive,
    [SYS_SHM_CREATE]       = sys_shm_create,
    [SYS_SHM_MAP]          = sys_shm_map,
    [SYS_FUTEX_WAIT]       = sys_futex_wait,
    [SYS_FUTEX_WAKE]       = sys_futex_wake,
    [SYS_FILE_OPEN]        = sys_file_open,
    [SYS_FILE_READ]        = sys_file_read,
    [SYS_FILE_WRITE]       = sys_file_write,
    [SYS_FILE_SEEK]        = sys_file_seek,
    [SYS_FILE_TRUNCATE]    = sys_file_truncate,
    [SYS_FILE_STAT]        = sys_file_stat,
    [SYS_DIRECTORY_READ]   = sys_directory_read,
    [SYS_PATH_STAT]        = sys_path_stat,
    [SYS_PATH_MKDIR]       = sys_path_mkdir,
    [SYS_PATH_UNLINK]      = sys_path_unlink,
    [SYS_PATH_RENAME]      = sys_path_rename,
    [SYS_PATH_SYMLINK]     = sys_path_symlink,
    [SYS_PATH_READLINK]    = sys_path_readlink,
    [SYS_CHDIR]            = sys_chdir,
    [SYS_GETCWD]           = sys_getcwd,
    [SYS_FS_SYNC]          = sys_fs_sync,
    [SYS_MOUNT]            = sys_mount,
    [SYS_UNMOUNT]          = sys_unmount,
    [SYS_PROCESS_SPAWN]    = sys_process_spawn,
    [SYS_PROCESS_INFO]     = sys_process_info,
    [SYS_PROCESS_KILL]     = sys_process_kill,
    [SYS_PIPE_CREATE]      = sys_pipe_create,
    [SYS_SYSTEM_POWER]     = sys_system_power,
    [SYS_SOCKET_CREATE]    = sys_socket_create,
    [SYS_SOCKET_BIND]      = sys_socket_bind,
    [SYS_SOCKET_CONNECT]   = sys_socket_connect,
    [SYS_SOCKET_LISTEN]    = sys_socket_listen,
    [SYS_SOCKET_ACCEPT]    = sys_socket_accept,
    [SYS_SOCKET_SEND]      = sys_socket_send,
    [SYS_SOCKET_RECEIVE]   = sys_socket_receive,
    [SYS_SOCKET_SHUTDOWN]  = sys_socket_shutdown,
    [SYS_SOCKET_SET_OPTION] = sys_socket_set_option,
    [SYS_SOCKET_INFO]      = sys_socket_info,
    [SYS_NET_INTERFACE_INFO] = sys_net_interface_info,
    [SYS_NET_CONFIGURE]    = sys_net_configure,
    [SYS_NET_RESOLVE]      = sys_net_resolve,
    [SYS_OBJECT_WAIT_MANY] = sys_object_wait_many,
    [SYS_CHANNEL_SEND_HANDLES] = sys_channel_send_handles,
    [SYS_CHANNEL_RECEIVE_HANDLES] = sys_channel_receive_handles,
    [SYS_SERVICE_REGISTER] = sys_service_register,
    [SYS_SERVICE_CONNECT]  = sys_service_connect,
    [SYS_DISPLAY_INFO]     = sys_display_info,
    [SYS_DISPLAY_ACQUIRE]  = sys_display_acquire,
    [SYS_INPUT_OPEN]       = sys_input_open,
    [SYS_INPUT_READ]       = sys_input_read,
};

uint64_t syscall_dispatch(uint64_t number, const uint64_t args[6])
{
    if (number >= SYS_COUNT || !table[number])
        return STATUS_NOT_SUPPORTED;
    return (uint64_t)table[number](args);
}
