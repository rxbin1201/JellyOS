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
