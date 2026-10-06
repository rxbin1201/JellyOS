# JellyOS Error Model

**Header:** [`sdk/include/jelly/status.h`](../../sdk/include/jelly/status.h)

One status type, `status_t`, is used by the kernel, system calls and userspace
libraries (README section 21). The header lives in the SDK because userspace
uses exactly the same definitions.

| Code | Value | Meaning |
|---|---|---|
| `STATUS_SUCCESS` | 0 | Operation completed |
| `STATUS_INVALID_ARGUMENT` | 1 | Caller passed an invalid value |
| `STATUS_NOT_FOUND` | 2 | Object, file or device does not exist |
| `STATUS_ACCESS_DENIED` | 3 | Caller lacks permission |
| `STATUS_OUT_OF_MEMORY` | 4 | Allocation failed |
| `STATUS_BUSY` | 5 | Resource in use, retry later |
| `STATUS_NOT_SUPPORTED` | 6 | Hardware or operation not supported |
| `STATUS_IO_ERROR` | 7 | Transfer failed |
| `STATUS_TIMEOUT` | 8 | Operation did not complete in time |
| `STATUS_DEVICE_ERROR` | 9 | Device misbehaved |
| `STATUS_WOULD_BLOCK` | 10 | Non-blocking operation cannot proceed now (empty or full queue, futex value changed) |
| `STATUS_BUFFER_TOO_SMALL` | 11 | Output buffer too small; the required size is reported |
| `STATUS_PEER_CLOSED` | 12 | The other end of a channel is gone |
| `STATUS_BAD_HANDLE` | 13 | Handle invalid, closed or of the wrong type |
| `STATUS_LIMIT_EXCEEDED` | 14 | A resource limit (handles, threads, memory) was reached |
| `STATUS_INTERRUPTED` | 15 | A wait ended because the thread is being terminated |

Codes 0–9 are the initial set from the README. Codes 10–15 were appended with
syscall ABI version 1.

## Rules

- `STATUS_SUCCESS` is 0. Every error is positive. Test errors with `STATUS_IS_ERROR(s)`.
- Values never change meaning and are never reused. New codes are appended.
- Functions that can fail return `status_t` and pass results through out
  parameters. Errors are never encoded in pointers or sizes.
- `status_name()` gives a stable name for logs and diagnostics.
- Unrecoverable kernel states use `panic()` or `ASSERT()`, not error codes.
- System calls return `status_t` in `RAX` (see [../abi/syscalls.md](../abi/syscalls.md)).
