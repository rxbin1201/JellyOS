# JellyOS System Call ABI

**ABI version:** 1 (`JELLY_SYSCALL_ABI_VERSION`)
**Headers:** [`sdk/include/jelly/syscall.h`](../../sdk/include/jelly/syscall.h) (numbers, rights, flags), [`sdk/include/jelly/status.h`](../../sdk/include/jelly/status.h) (errors), [`sdk/include/jelly/os.h`](../../sdk/include/jelly/os.h) (libos wrappers)

## Calling convention (x86_64)

| Register | Use |
|---|---|
| `RAX` | System call number on entry, `status_t` on return |
| `RDI`, `RSI`, `RDX`, `R10`, `R8`, `R9` | Arguments 0–5 |
| `RCX`, `R11` | Clobbered by `SYSCALL` |
| All other registers | Preserved |

Rules:

- Every call returns a `status_t`. `0` is success and errors are positive (see
  [error-model.md](../architecture/error-model.md)).
- Results are written through output pointers. The kernel checks every user
  pointer: it must lie in user space and be mapped as user memory, and as
  writable for outputs. Invalid pointers return `INVALID_ARGUMENT`; they never
  fault the kernel.
- Outputs are only written when the call succeeds. Exception:
  `SYS_CHANNEL_RECEIVE` reports the required size together with
  `BUFFER_TOO_SMALL`.
- Unknown numbers return `NOT_SUPPORTED`.
- Numbers and meanings never change. New calls are appended and increment the
  ABI version.

## Program entry

The main thread starts at the ELF entry point with:

| Register | Value |
|---|---|
| `RDI`, `RSI`, `RDX` | Startup arguments 0–2 from the creator (scenario numbers, handles, ...) |
| `RSP` | Top of a 64 KiB stack, `RSP ≡ 8 (mod 16)` as after a `call` |
| `RFLAGS` | `IF` set, everything else clear |
| FPU/SSE | `FCW = 0x037F`, `MXCSR = 0x1F80` |

Threads created with `SYS_THREAD_CREATE` start the same way at `entry` with
`RDI = arg0` and `RSI = arg1`. libos's `_start` calls
`main(arg0, arg1, arg2)` and exits with its return value.

## Calls (version 1)

| # | Name | Arguments | Output | Rights / notes |
|---|---|---|---|---|
| 0 | `SYS_ABI_VERSION` | `uint32_t *version` | version | |
| 1 | `SYS_DEBUG_WRITE` | `const char *text, size_t length` | | Writes to the kernel log, at most 256 bytes per call. Development aid until consoles exist |
| 2 | `SYS_CLOCK_MONOTONIC` | `uint64_t *ns` | nanoseconds since boot | |
| 3 | `SYS_PROCESS_EXIT` | `int32_t code` | never returns | Ends all threads of the process |
| 4 | `SYS_THREAD_CREATE` | `entry, stack_top, arg0, arg1, jelly_handle_t *thread` | thread handle (`WAIT`, `DUPLICATE`) | The thread starts only after the handle was stored. Limit: 64 threads |
| 5 | `SYS_THREAD_EXIT` | | never returns | The process ends with code 0 when its last thread exits |
| 6 | `SYS_THREAD_YIELD` | | | |
| 7 | `SYS_THREAD_SLEEP` | `uint64_t ns` | | |
| 8 | `SYS_MEMORY_ALLOCATE` | `size, flags, uintptr_t *address` | address | Zeroed memory. `JELLY_MEMORY_WRITE` / `EXEC`, never both. Limit: 64 MiB per process |
| 9 | `SYS_MEMORY_UNMAP` | `address, size` | | Page aligned. Shared-memory mappings only as a whole |
| 10 | `SYS_HANDLE_CLOSE` | `handle` | | |
| 11 | `SYS_HANDLE_DUPLICATE` | `handle, rights, jelly_handle_t *copy` | new handle | Needs `DUPLICATE`. Rights can only be reduced |
| 12 | `SYS_OBJECT_WAIT` | `handle, timeout_ns` | | Needs `WAIT`. `0` polls, `JELLY_WAIT_FOREVER` blocks. Returns `TIMEOUT` |
| 13 | `SYS_EVENT_CREATE` | `flags, jelly_handle_t *event` | handle (`WAIT`, `SIGNAL`, `DUPLICATE`) | `JELLY_EVENT_AUTO_RESET`: a satisfied wait clears the event |
| 14 | `SYS_EVENT_SIGNAL` | `handle` | | Needs `SIGNAL` |
| 15 | `SYS_EVENT_RESET` | `handle` | | Needs `SIGNAL` |
| 16 | `SYS_CHANNEL_CREATE` | `jelly_handle_t *end0, *end1` | two handles (`READ`, `WRITE`, `WAIT`, `DUPLICATE`) | |
| 17 | `SYS_CHANNEL_SEND` | `handle, data, size` | | Needs `WRITE`. At most 64 KiB per message and 64 queued messages. `WOULD_BLOCK` when full, `PEER_CLOSED` |
| 18 | `SYS_CHANNEL_RECEIVE` | `handle, buffer, size, size_t *actual` | message, size | Needs `READ`. `WOULD_BLOCK` when empty. `BUFFER_TOO_SMALL` reports the size and keeps the message. `PEER_CLOSED` once drained |
| 19 | `SYS_SHM_CREATE` | `size, jelly_handle_t *shm` | handle (`MAP`, `WRITE`, `DUPLICATE`) | At most 64 MiB. Zeroed |
| 20 | `SYS_SHM_MAP` | `handle, flags, uintptr_t *address` | address | Needs `MAP`, plus `WRITE` for writable mappings. Never executable |
| 21 | `SYS_FUTEX_WAIT` | `const uint32_t *word, expected, timeout_ns` | | Blocks if `*word == expected`, otherwise `WOULD_BLOCK`. Word must be 4-byte aligned |
| 22 | `SYS_FUTEX_WAKE` | `const uint32_t *word, count` | | Wakes up to `count` waiters. Works across processes through shared memory |

## Handles

Handles are 32-bit values local to a process: bits 0–15 hold the slot and
bits 16–31 a generation, so a closed handle never becomes valid again by
accident. `0` is never a valid handle. Every handle carries rights:

| Right | Allows |
|---|---|
| `JELLY_RIGHT_READ` | Receiving from a channel |
| `JELLY_RIGHT_WRITE` | Sending to a channel, mapping shared memory writable |
| `JELLY_RIGHT_WAIT` | `SYS_OBJECT_WAIT` |
| `JELLY_RIGHT_SIGNAL` | Signaling or resetting an event |
| `JELLY_RIGHT_MAP` | Mapping shared memory |
| `JELLY_RIGHT_DUPLICATE` | Duplicating the handle (with equal or fewer rights) |

Waitable objects and their signaled state:

| Object | Signaled when |
|---|---|
| Process | It has exited |
| Thread | It has exited |
| Event | Signaled (until reset; auto-reset events clear on a satisfied wait) |
| Channel endpoint | A message is queued or the peer is closed |

Resource limits per process (initial values): 256 handles, 64 threads, 64 MiB
of memory.

## Faults

A CPU exception in user mode ends the process with exit code
`JELLY_EXIT_FAULT` (-1) and a reason such as
`page fault at 0x0: null pointer dereference (user instruction fetch) at rip 0x0`.
The kernel and all other processes keep running.
