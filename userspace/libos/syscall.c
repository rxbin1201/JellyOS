/*
 * libos system call wrappers (x86_64 calling convention, docs/abi/syscalls.md):
 * RAX = number, RDI, RSI, RDX, R10, R8, R9 = arguments, RAX = status_t.
 * The SYSCALL instruction clobbers RCX and R11.
 */

#include <jelly/os.h>

uint64_t jelly_syscall(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                       uint64_t a5)
{
    register uint64_t r10 __asm__("r10") = a3;
    register uint64_t r8 __asm__("r8") = a4;
    register uint64_t r9 __asm__("r9") = a5;
    uint64_t result;

    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(a0), "S"(a1), "d"(a2), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return result;
}

#define SYSCALL0(n)                   jelly_syscall((n), 0, 0, 0, 0, 0, 0)
#define SYSCALL1(n, a)                jelly_syscall((n), (uint64_t)(a), 0, 0, 0, 0, 0)
#define SYSCALL2(n, a, b)             jelly_syscall((n), (uint64_t)(a), (uint64_t)(b), 0, 0, 0, 0)
#define SYSCALL3(n, a, b, c)          jelly_syscall((n), (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), 0, 0, 0)
#define SYSCALL4(n, a, b, c, d)       jelly_syscall((n), (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), 0, 0)
#define SYSCALL5(n, a, b, c, d, e)    jelly_syscall((n), (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), (uint64_t)(e), 0)
#define SYSCALL6(n, a, b, c, d, e, f) \
    jelly_syscall((n), (uint64_t)(a), (uint64_t)(b), (uint64_t)(c), (uint64_t)(d), (uint64_t)(e), (uint64_t)(f))

uint32_t jelly_abi_version(void)
{
    uint32_t version = 0;
    SYSCALL1(SYS_ABI_VERSION, &version);
    return version;
}

status_t jelly_debug_write(const char *text, size_t length)
{
    return (status_t)SYSCALL2(SYS_DEBUG_WRITE, text, length);
}

void jelly_print(const char *text)
{
    size_t length = 0;
    while (text[length])
        length++;
    jelly_debug_write(text, length);
}

uint64_t jelly_clock_ns(void)
{
    uint64_t ns = 0;
    SYSCALL1(SYS_CLOCK_MONOTONIC, &ns);
    return ns;
}

void jelly_process_exit(int32_t code)
{
    SYSCALL1(SYS_PROCESS_EXIT, (int64_t)code);
    __builtin_unreachable();
}

/* New threads start here: RDI = function, RSI = argument. */
static __attribute__((noreturn)) void thread_start(void (*entry)(void *), void *arg)
{
    entry(arg);
    jelly_thread_exit();
}

status_t jelly_thread_create(void (*entry)(void *), void *arg, void *stack_top, jelly_handle_t *thread)
{
    return (status_t)SYSCALL5(SYS_THREAD_CREATE, thread_start, stack_top, entry, arg, thread);
}

void jelly_thread_exit(void)
{
    SYSCALL0(SYS_THREAD_EXIT);
    __builtin_unreachable();
}

void jelly_thread_yield(void)
{
    SYSCALL0(SYS_THREAD_YIELD);
}

status_t jelly_thread_sleep(uint64_t ns)
{
    return (status_t)SYSCALL1(SYS_THREAD_SLEEP, ns);
}

status_t jelly_memory_allocate(size_t size, uint32_t flags, void **address)
{
    return (status_t)SYSCALL3(SYS_MEMORY_ALLOCATE, size, flags, address);
}

status_t jelly_memory_unmap(void *address, size_t size)
{
    return (status_t)SYSCALL2(SYS_MEMORY_UNMAP, address, size);
}

status_t jelly_handle_close(jelly_handle_t handle)
{
    return (status_t)SYSCALL1(SYS_HANDLE_CLOSE, handle);
}

status_t jelly_handle_duplicate(jelly_handle_t handle, uint32_t rights, jelly_handle_t *copy)
{
    return (status_t)SYSCALL3(SYS_HANDLE_DUPLICATE, handle, rights, copy);
}

status_t jelly_wait(jelly_handle_t handle, uint64_t timeout_ns)
{
    return (status_t)SYSCALL2(SYS_OBJECT_WAIT, handle, timeout_ns);
}

status_t jelly_event_create(uint32_t flags, jelly_handle_t *event)
{
    return (status_t)SYSCALL2(SYS_EVENT_CREATE, flags, event);
}

status_t jelly_event_signal(jelly_handle_t event)
{
    return (status_t)SYSCALL1(SYS_EVENT_SIGNAL, event);
}

status_t jelly_event_reset(jelly_handle_t event)
{
    return (status_t)SYSCALL1(SYS_EVENT_RESET, event);
}

status_t jelly_channel_create(jelly_handle_t *end0, jelly_handle_t *end1)
{
    return (status_t)SYSCALL2(SYS_CHANNEL_CREATE, end0, end1);
}

status_t jelly_channel_send(jelly_handle_t channel, const void *data, size_t size)
{
    return (status_t)SYSCALL3(SYS_CHANNEL_SEND, channel, data, size);
}

status_t jelly_channel_receive(jelly_handle_t channel, void *buffer, size_t size, size_t *actual)
{
    return (status_t)SYSCALL4(SYS_CHANNEL_RECEIVE, channel, buffer, size, actual);
}

status_t jelly_shm_create(size_t size, jelly_handle_t *shm)
{
    return (status_t)SYSCALL2(SYS_SHM_CREATE, size, shm);
}

status_t jelly_shm_map(jelly_handle_t shm, uint32_t flags, void **address)
{
    return (status_t)SYSCALL3(SYS_SHM_MAP, shm, flags, address);
}

status_t jelly_futex_wait(const uint32_t *word, uint32_t expected, uint64_t timeout_ns)
{
    return (status_t)SYSCALL3(SYS_FUTEX_WAIT, word, expected, timeout_ns);
}

status_t jelly_futex_wake(const uint32_t *word, uint32_t count)
{
    return (status_t)SYSCALL2(SYS_FUTEX_WAKE, word, count);
}

/* --- Files ---------------------------------------------------------------------- */

static size_t length_of(const char *s)
{
    size_t n = 0;
    while (s && s[n])
        n++;
    return n;
}

status_t jelly_open(const char *path, uint32_t flags, uint32_t mode, jelly_handle_t *file)
{
    return (status_t)SYSCALL5(SYS_FILE_OPEN, path, length_of(path), flags, mode, file);
}

status_t jelly_read(jelly_handle_t file, void *buffer, size_t size, size_t *done)
{
    return (status_t)SYSCALL4(SYS_FILE_READ, file, buffer, size, done);
}

status_t jelly_write(jelly_handle_t file, const void *buffer, size_t size, size_t *done)
{
    return (status_t)SYSCALL4(SYS_FILE_WRITE, file, buffer, size, done);
}

/* position may be NULL here; the system call itself always needs somewhere to write. */
status_t jelly_seek(jelly_handle_t file, int64_t offset, uint32_t whence, uint64_t *position)
{
    uint64_t ignored;
    return (status_t)SYSCALL4(SYS_FILE_SEEK, file, offset, whence, position ? position : &ignored);
}

status_t jelly_truncate(jelly_handle_t file, uint64_t size)
{
    return (status_t)SYSCALL2(SYS_FILE_TRUNCATE, file, size);
}

status_t jelly_fstat(jelly_handle_t file, jelly_stat_t *stat)
{
    return (status_t)SYSCALL2(SYS_FILE_STAT, file, stat);
}

status_t jelly_readdir(jelly_handle_t directory, jelly_dirent_t *entry)
{
    return (status_t)SYSCALL2(SYS_DIRECTORY_READ, directory, entry);
}

status_t jelly_stat(const char *path, uint32_t flags, jelly_stat_t *stat)
{
    return (status_t)SYSCALL4(SYS_PATH_STAT, path, length_of(path), flags, stat);
}

status_t jelly_mkdir(const char *path, uint32_t mode)
{
    return (status_t)SYSCALL3(SYS_PATH_MKDIR, path, length_of(path), mode);
}

status_t jelly_unlink(const char *path)
{
    return (status_t)SYSCALL2(SYS_PATH_UNLINK, path, length_of(path));
}

status_t jelly_rename(const char *from, const char *to)
{
    return (status_t)SYSCALL4(SYS_PATH_RENAME, from, length_of(from), to, length_of(to));
}

status_t jelly_symlink(const char *target, const char *path)
{
    return (status_t)SYSCALL4(SYS_PATH_SYMLINK, target, length_of(target), path, length_of(path));
}

status_t jelly_readlink(const char *path, char *buffer, size_t size, size_t *length)
{
    return (status_t)SYSCALL5(SYS_PATH_READLINK, path, length_of(path), buffer, size, length);
}

status_t jelly_chdir(const char *path)
{
    return (status_t)SYSCALL2(SYS_CHDIR, path, length_of(path));
}

status_t jelly_getcwd(char *buffer, size_t size, size_t *length)
{
    return (status_t)SYSCALL3(SYS_GETCWD, buffer, size, length);
}

status_t jelly_sync(void)
{
    return (status_t)SYSCALL0(SYS_FS_SYNC);
}

status_t jelly_mount(const char *path, const char *device, const char *type)
{
    return (status_t)SYSCALL6(SYS_MOUNT, path, length_of(path), device, length_of(device), type, length_of(type));
}

status_t jelly_unmount(const char *path)
{
    return (status_t)SYSCALL2(SYS_UNMOUNT, path, length_of(path));
}

/* --- Programs, pipes and power (ABI version 3) ------------------------------------ */

status_t jelly_spawn(const jelly_spawn_t *request, jelly_handle_t *process)
{
    return (status_t)SYSCALL2(SYS_PROCESS_SPAWN, request, process);
}

status_t jelly_process_info(jelly_handle_t process, jelly_process_info_t *info)
{
    return (status_t)SYSCALL2(SYS_PROCESS_INFO, process, info);
}

status_t jelly_process_kill(jelly_handle_t process, int32_t code)
{
    return (status_t)SYSCALL2(SYS_PROCESS_KILL, process, (int64_t)code);
}

status_t jelly_pipe_create(jelly_handle_t *read_end, jelly_handle_t *write_end)
{
    return (status_t)SYSCALL2(SYS_PIPE_CREATE, read_end, write_end);
}

status_t jelly_system_power(uint32_t action)
{
    return (status_t)SYSCALL1(SYS_SYSTEM_POWER, action);
}

/* --- Networking (ABI version 4) ---------------------------------------------------- */

status_t jelly_socket(uint32_t domain, uint32_t type, uint32_t protocol, jelly_handle_t *socket)
{
    return (status_t)SYSCALL4(SYS_SOCKET_CREATE, domain, type, protocol, socket);
}

status_t jelly_bind(jelly_handle_t socket, const jelly_sockaddr_in_t *address)
{
    return (status_t)SYSCALL2(SYS_SOCKET_BIND, socket, address);
}

status_t jelly_connect(jelly_handle_t socket, const jelly_sockaddr_in_t *address)
{
    return (status_t)SYSCALL2(SYS_SOCKET_CONNECT, socket, address);
}

status_t jelly_listen(jelly_handle_t socket, uint32_t backlog)
{
    return (status_t)SYSCALL2(SYS_SOCKET_LISTEN, socket, backlog);
}

status_t jelly_accept(jelly_handle_t socket, jelly_handle_t *connection, jelly_sockaddr_in_t *peer)
{
    return (status_t)SYSCALL3(SYS_SOCKET_ACCEPT, socket, connection, peer);
}

status_t jelly_send(jelly_handle_t socket, const void *data, size_t size, const jelly_sockaddr_in_t *to,
                    uint32_t flags, size_t *done)
{
    return (status_t)SYSCALL6(SYS_SOCKET_SEND, socket, data, size, to, flags, done);
}

status_t jelly_receive(jelly_handle_t socket, void *buffer, size_t size, jelly_sockaddr_in_t *from, uint32_t flags,
                       size_t *done)
{
    return (status_t)SYSCALL6(SYS_SOCKET_RECEIVE, socket, buffer, size, from, flags, done);
}

status_t jelly_shutdown(jelly_handle_t socket, uint32_t how)
{
    return (status_t)SYSCALL2(SYS_SOCKET_SHUTDOWN, socket, how);
}

status_t jelly_socket_option(jelly_handle_t socket, uint32_t option, uint64_t value)
{
    return (status_t)SYSCALL3(SYS_SOCKET_SET_OPTION, socket, option, value);
}

status_t jelly_socket_info(jelly_handle_t socket, jelly_socket_info_t *info)
{
    return (status_t)SYSCALL2(SYS_SOCKET_INFO, socket, info);
}

status_t jelly_net_interface_info(uint32_t index, jelly_netif_info_t *info)
{
    return (status_t)SYSCALL2(SYS_NET_INTERFACE_INFO, index, info);
}

status_t jelly_net_configure(uint32_t index, const jelly_netif_config_t *config)
{
    return (status_t)SYSCALL2(SYS_NET_CONFIGURE, index, config);
}

status_t jelly_net_resolve(const char *name, uint32_t *address)
{
    return (status_t)SYSCALL3(SYS_NET_RESOLVE, name, length_of(name), address);
}

/* --- Graphics and input (ABI version 5) ---------------------------------------------- */

status_t jelly_wait_many(const jelly_handle_t *handles, uint32_t count, uint64_t timeout_ns, uint32_t *index)
{
    return (status_t)SYSCALL4(SYS_OBJECT_WAIT_MANY, handles, count, timeout_ns, index);
}

status_t jelly_channel_send_handles(jelly_handle_t channel, const void *data, size_t size,
                                    const jelly_handle_t *handles, uint32_t count)
{
    return (status_t)SYSCALL5(SYS_CHANNEL_SEND_HANDLES, channel, data, size, handles, count);
}

status_t jelly_channel_receive_handles(jelly_handle_t channel, void *buffer, size_t size, size_t *actual,
                                       jelly_handle_t handles[JELLY_CHANNEL_MAX_HANDLES], uint32_t *count)
{
    return (status_t)SYSCALL6(SYS_CHANNEL_RECEIVE_HANDLES, channel, buffer, size, actual, handles, count);
}

status_t jelly_service_register(const char *name, jelly_handle_t channel)
{
    return (status_t)SYSCALL3(SYS_SERVICE_REGISTER, name, length_of(name), channel);
}

status_t jelly_service_connect(const char *name, jelly_handle_t *channel)
{
    return (status_t)SYSCALL3(SYS_SERVICE_CONNECT, name, length_of(name), channel);
}

status_t jelly_display_info(uint32_t index, jelly_display_info_t *info)
{
    return (status_t)SYSCALL2(SYS_DISPLAY_INFO, index, info);
}

status_t jelly_display_acquire(uint32_t index, jelly_handle_t *framebuffer)
{
    return (status_t)SYSCALL2(SYS_DISPLAY_ACQUIRE, index, framebuffer);
}

status_t jelly_input_open(jelly_handle_t *input)
{
    return (status_t)SYSCALL1(SYS_INPUT_OPEN, input);
}

status_t jelly_input_read(jelly_handle_t input, jelly_input_event_t *events, size_t count, size_t *read)
{
    return (status_t)SYSCALL4(SYS_INPUT_READ, input, events, count, read);
}

/* --- Desktop (ABI version 6) ---------------------------------------------------------- */

status_t jelly_spawn_as(const jelly_spawn_t *request, uint32_t uid, uint32_t gid, jelly_handle_t *process)
{
    return (status_t)SYSCALL4(SYS_PROCESS_SPAWN_AS, request, uid, gid, process);
}

status_t jelly_clock_realtime(uint64_t *ns)
{
    return (status_t)SYSCALL1(SYS_CLOCK_REALTIME, ns);
}

status_t jelly_system_info(jelly_system_info_t *info)
{
    return (status_t)SYSCALL1(SYS_SYSTEM_INFO, info);
}

/* --- Audio devices (ABI version 7) ------------------------------------------------------ */

status_t jelly_audio_info(uint32_t index, jelly_audio_info_t *info)
{
    return (status_t)SYSCALL2(SYS_AUDIO_INFO, index, info);
}

status_t jelly_audio_open(uint32_t index, jelly_handle_t *device)
{
    return (status_t)SYSCALL2(SYS_AUDIO_OPEN, index, device);
}

status_t jelly_audio_write(jelly_handle_t device, const int16_t *frames, size_t count, size_t *written)
{
    return (status_t)SYSCALL4(SYS_AUDIO_WRITE, device, frames, count, written);
}

status_t jelly_audio_read(jelly_handle_t device, int16_t *frames, size_t count, size_t *read)
{
    return (status_t)SYSCALL4(SYS_AUDIO_READ, device, frames, count, read);
}

status_t jelly_audio_control(jelly_handle_t device, uint32_t command, uint64_t value, uint64_t *result)
{
    return (status_t)SYSCALL4(SYS_AUDIO_CONTROL, device, command, value, result);
}

/* --- Graphics driver features of a display (ABI version 8) ------------------------------ */

status_t jelly_display_cursor(uint32_t index, const jelly_cursor_t *cursor)
{
    return (status_t)SYSCALL2(SYS_DISPLAY_CURSOR, index, cursor);
}

status_t jelly_display_vblank(uint32_t index, uint64_t timeout_ns)
{
    return (status_t)SYSCALL2(SYS_DISPLAY_VBLANK, index, timeout_ns);
}

status_t jelly_display_buffer(uint32_t index, uint32_t buffer, jelly_handle_t *memory)
{
    return (status_t)SYSCALL3(SYS_DISPLAY_BUFFER, index, buffer, memory);
}

status_t jelly_display_flip(uint32_t index, uint32_t buffer)
{
    return (status_t)SYSCALL2(SYS_DISPLAY_FLIP, index, buffer);
}

/* --- Display modes and hot plug (ABI version 9) ------------------------------------------ */

status_t jelly_display_modes(uint32_t index, jelly_display_mode_t *modes, uint32_t max, uint32_t *count)
{
    return (status_t)SYSCALL4(SYS_DISPLAY_MODES, index, modes, max, count);
}

status_t jelly_display_set_mode(uint32_t index, uint32_t mode)
{
    return (status_t)SYSCALL2(SYS_DISPLAY_SET_MODE, index, mode);
}

status_t jelly_display_watch(uint32_t index, jelly_handle_t *event)
{
    return (status_t)SYSCALL2(SYS_DISPLAY_WATCH, index, event);
}

/* --- The screen off and on (ABI version 10) ---------------------------------------------- */

status_t jelly_display_power(uint32_t index, bool on)
{
    return (status_t)SYSCALL2(SYS_DISPLAY_POWER, index, on ? 1 : 0);
}
