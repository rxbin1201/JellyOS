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

## Rules

- `STATUS_SUCCESS` is 0. Every error is positive. Test errors with `STATUS_IS_ERROR(s)`.
- Values never change meaning and are never reused. New codes are appended.
- Functions that can fail return `status_t` and pass results through out
  parameters. Errors are never encoded in pointers or sizes.
- `status_name()` gives a stable name for logs and diagnostics.
- Unrecoverable kernel states use `panic()` or `ASSERT()`, not error codes.
- How syscalls encode `status_t` is defined with the syscall ABI (Phase 4).
