/*
 * JellyOS global error model (README section 21).
 *
 * One status type for the kernel, system calls and userspace libraries.
 * See docs/architecture/error-model.md.
 *
 * Rules:
 *  - STATUS_SUCCESS is 0; every error is a positive value.
 *  - Values never change meaning and are never reused; new codes are appended.
 */

#ifndef JELLY_STATUS_H
#define JELLY_STATUS_H

typedef enum {
    STATUS_SUCCESS          = 0,
    STATUS_INVALID_ARGUMENT = 1,
    STATUS_NOT_FOUND        = 2,
    STATUS_ACCESS_DENIED    = 3,
    STATUS_OUT_OF_MEMORY    = 4,
    STATUS_BUSY             = 5,
    STATUS_NOT_SUPPORTED    = 6,
    STATUS_IO_ERROR         = 7,
    STATUS_TIMEOUT          = 8,
    STATUS_DEVICE_ERROR     = 9,
    /* appended in syscall ABI version 1 */
    STATUS_WOULD_BLOCK      = 10,
    STATUS_BUFFER_TOO_SMALL = 11,
    STATUS_PEER_CLOSED      = 12,
    STATUS_BAD_HANDLE       = 13,
    STATUS_LIMIT_EXCEEDED   = 14,
    STATUS_INTERRUPTED      = 15,
    /* appended in syscall ABI version 2 (file systems) */
    STATUS_ALREADY_EXISTS   = 16,
    STATUS_NOT_DIRECTORY    = 17,
    STATUS_IS_DIRECTORY     = 18,
    STATUS_NOT_EMPTY        = 19,
    STATUS_NO_SPACE         = 20,
} status_t;

#define STATUS_IS_ERROR(s) ((s) != STATUS_SUCCESS)

static inline const char *status_name(status_t status)
{
    switch (status) {
    case STATUS_SUCCESS:          return "SUCCESS";
    case STATUS_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case STATUS_NOT_FOUND:        return "NOT_FOUND";
    case STATUS_ACCESS_DENIED:    return "ACCESS_DENIED";
    case STATUS_OUT_OF_MEMORY:    return "OUT_OF_MEMORY";
    case STATUS_BUSY:             return "BUSY";
    case STATUS_NOT_SUPPORTED:    return "NOT_SUPPORTED";
    case STATUS_IO_ERROR:         return "IO_ERROR";
    case STATUS_TIMEOUT:          return "TIMEOUT";
    case STATUS_DEVICE_ERROR:     return "DEVICE_ERROR";
    case STATUS_WOULD_BLOCK:      return "WOULD_BLOCK";
    case STATUS_BUFFER_TOO_SMALL: return "BUFFER_TOO_SMALL";
    case STATUS_PEER_CLOSED:      return "PEER_CLOSED";
    case STATUS_BAD_HANDLE:       return "BAD_HANDLE";
    case STATUS_LIMIT_EXCEEDED:   return "LIMIT_EXCEEDED";
    case STATUS_INTERRUPTED:      return "INTERRUPTED";
    case STATUS_ALREADY_EXISTS:   return "ALREADY_EXISTS";
    case STATUS_NOT_DIRECTORY:    return "NOT_DIRECTORY";
    case STATUS_IS_DIRECTORY:     return "IS_DIRECTORY";
    case STATUS_NOT_EMPTY:        return "NOT_EMPTY";
    case STATUS_NO_SPACE:         return "NO_SPACE";
    }
    return "UNKNOWN";
}

#endif
