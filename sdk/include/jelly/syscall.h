/*
 * JellyOS system call ABI (README section 20).
 *
 * Shared by the kernel and userspace. See docs/abi/syscalls.md for the
 * calling convention and the full table of calls.
 *
 * Rules:
 *  - Every call returns status_t in RAX. Results are written through output
 *    pointers, which the kernel validates.
 *  - Numbers, argument meanings and constants never change; new calls are
 *    appended and JELLY_SYSCALL_ABI_VERSION is incremented.
 */

#ifndef JELLY_SYSCALL_H
#define JELLY_SYSCALL_H

#include <stdint.h>

#define JELLY_SYSCALL_ABI_VERSION 6

typedef uint32_t jelly_handle_t;
#define JELLY_HANDLE_INVALID 0u

enum {
    SYS_ABI_VERSION      = 0,  /* (uint32_t *version) */
    SYS_DEBUG_WRITE      = 1,  /* (const char *text, size_t length) */
    SYS_CLOCK_MONOTONIC  = 2,  /* (uint64_t *ns) */
    SYS_PROCESS_EXIT     = 3,  /* (int32_t code)                                  never returns */
    SYS_THREAD_CREATE    = 4,  /* (entry, stack_top, arg0, arg1, jelly_handle_t *thread) */
    SYS_THREAD_EXIT      = 5,  /* ()                                              never returns */
    SYS_THREAD_YIELD     = 6,  /* () */
    SYS_THREAD_SLEEP     = 7,  /* (uint64_t ns) */
    SYS_MEMORY_ALLOCATE  = 8,  /* (size_t size, uint32_t flags, uintptr_t *address) */
    SYS_MEMORY_UNMAP     = 9,  /* (uintptr_t address, size_t size) */
    SYS_HANDLE_CLOSE     = 10, /* (handle) */
    SYS_HANDLE_DUPLICATE = 11, /* (handle, uint32_t rights, jelly_handle_t *copy) */
    SYS_OBJECT_WAIT      = 12, /* (handle, uint64_t timeout_ns) */
    SYS_EVENT_CREATE     = 13, /* (uint32_t flags, jelly_handle_t *event) */
    SYS_EVENT_SIGNAL     = 14, /* (handle) */
    SYS_EVENT_RESET      = 15, /* (handle) */
    SYS_CHANNEL_CREATE   = 16, /* (jelly_handle_t *end0, jelly_handle_t *end1) */
    SYS_CHANNEL_SEND     = 17, /* (handle, const void *data, size_t size) */
    SYS_CHANNEL_RECEIVE  = 18, /* (handle, void *buffer, size_t size, size_t *actual) */
    SYS_SHM_CREATE       = 19, /* (size_t size, jelly_handle_t *shm) */
    SYS_SHM_MAP          = 20, /* (handle, uint32_t flags, uintptr_t *address) */
    SYS_FUTEX_WAIT       = 21, /* (const uint32_t *word, uint32_t expected, uint64_t timeout_ns) */
    SYS_FUTEX_WAKE       = 22, /* (const uint32_t *word, uint32_t count) */
    /* ABI version 2: files (paths are byte strings with explicit length) */
    SYS_FILE_OPEN        = 23, /* (path, length, uint32_t flags, uint32_t mode, jelly_handle_t *file) */
    SYS_FILE_READ        = 24, /* (handle, void *buffer, size_t size, size_t *done) */
    SYS_FILE_WRITE       = 25, /* (handle, const void *buffer, size_t size, size_t *done) */
    SYS_FILE_SEEK        = 26, /* (handle, int64_t offset, uint32_t whence, uint64_t *position) */
    SYS_FILE_TRUNCATE    = 27, /* (handle, uint64_t size) */
    SYS_FILE_STAT        = 28, /* (handle, jelly_stat_t *stat) */
    SYS_DIRECTORY_READ   = 29, /* (handle, jelly_dirent_t *entry)           NOT_FOUND at the end */
    SYS_PATH_STAT        = 30, /* (path, length, uint32_t flags, jelly_stat_t *stat) */
    SYS_PATH_MKDIR       = 31, /* (path, length, uint32_t mode) */
    SYS_PATH_UNLINK      = 32, /* (path, length) */
    SYS_PATH_RENAME      = 33, /* (from, from_length, to, to_length) */
    SYS_PATH_SYMLINK     = 34, /* (target, target_length, path, length) */
    SYS_PATH_READLINK    = 35, /* (path, length, char *buffer, size_t size, size_t *link_length) */
    SYS_CHDIR            = 36, /* (path, length) */
    SYS_GETCWD           = 37, /* (char *buffer, size_t size, size_t *length) */
    SYS_FS_SYNC          = 38, /* () */
    SYS_MOUNT            = 39, /* (path, length, device, device_length, type, type_length)   root only */
    SYS_UNMOUNT          = 40, /* (path, length)                                             root only */
    /* ABI version 3: programs, pipes, power */
    SYS_PROCESS_SPAWN    = 41, /* (const jelly_spawn_t *request, jelly_handle_t *process) */
    SYS_PROCESS_INFO     = 42, /* (handle, jelly_process_info_t *info) */
    SYS_PROCESS_KILL     = 43, /* (handle, int32_t exit_code) */
    SYS_PIPE_CREATE      = 44, /* (jelly_handle_t *read_end, jelly_handle_t *write_end) */
    SYS_SYSTEM_POWER     = 45, /* (uint32_t action)                                          root only */
    /* ABI version 4: networking */
    SYS_SOCKET_CREATE    = 46, /* (uint32_t domain, uint32_t type, uint32_t protocol, jelly_handle_t *socket) */
    SYS_SOCKET_BIND      = 47, /* (handle, const jelly_sockaddr_in_t *address) */
    SYS_SOCKET_CONNECT   = 48, /* (handle, const jelly_sockaddr_in_t *address) */
    SYS_SOCKET_LISTEN    = 49, /* (handle, uint32_t backlog) */
    SYS_SOCKET_ACCEPT    = 50, /* (handle, jelly_handle_t *connection, jelly_sockaddr_in_t *peer or NULL) */
    SYS_SOCKET_SEND      = 51, /* (handle, buffer, size, const jelly_sockaddr_in_t *to or NULL, flags, size_t *done) */
    SYS_SOCKET_RECEIVE   = 52, /* (handle, buffer, size, jelly_sockaddr_in_t *from or NULL, flags, size_t *done) */
    SYS_SOCKET_SHUTDOWN  = 53, /* (handle, uint32_t how) */
    SYS_SOCKET_SET_OPTION = 54, /* (handle, uint32_t option, uint64_t value) */
    SYS_SOCKET_INFO      = 55, /* (handle, jelly_socket_info_t *info) */
    SYS_NET_INTERFACE_INFO = 56, /* (uint32_t index, jelly_netif_info_t *info)                NOT_FOUND past the last */
    SYS_NET_CONFIGURE    = 57, /* (uint32_t index, const jelly_netif_config_t *config)      root only */
    SYS_NET_RESOLVE      = 58, /* (name, length, uint32_t *address)                          address in network order */
    /* ABI version 5: graphics and input */
    SYS_OBJECT_WAIT_MANY = 59, /* (const jelly_handle_t *handles, uint32_t count, uint64_t timeout_ns, uint32_t *index) */
    SYS_CHANNEL_SEND_HANDLES = 60, /* (handle, data, size, const jelly_handle_t *handles, uint32_t count)  moves them */
    SYS_CHANNEL_RECEIVE_HANDLES = 61, /* (handle, buffer, size, size_t *actual, jelly_handle_t *handles, uint32_t *count) */
    SYS_SERVICE_REGISTER = 62, /* (name, length, channel handle)                             root only */
    SYS_SERVICE_CONNECT  = 63, /* (name, length, jelly_handle_t *channel) */
    SYS_DISPLAY_INFO     = 64, /* (uint32_t index, jelly_display_info_t *info)                NOT_FOUND past the last */
    SYS_DISPLAY_ACQUIRE  = 65, /* (uint32_t index, jelly_handle_t *framebuffer)               root only, exclusive */
    SYS_INPUT_OPEN       = 66, /* (jelly_handle_t *input)                                     root only */
    SYS_INPUT_READ       = 67, /* (handle, jelly_input_event_t *events, size_t count, size_t *read)  WOULD_BLOCK if empty */
    /* ABI version 6: desktop */
    SYS_PROCESS_SPAWN_AS = 68, /* (const jelly_spawn_t *request, uint32_t uid, uint32_t gid, jelly_handle_t *process)  root */
    SYS_CLOCK_REALTIME   = 69, /* (uint64_t *ns)                      ns since 1970-01-01 UTC; NOT_SUPPORTED without RTC */
    SYS_SYSTEM_INFO      = 70, /* (jelly_system_info_t *info) */
    SYS_COUNT
};

/* SYS_FILE_OPEN flags */
#define JELLY_OPEN_READ       (1u << 0)
#define JELLY_OPEN_WRITE      (1u << 1)
#define JELLY_OPEN_CREATE     (1u << 2) /* create the file if it does not exist */
#define JELLY_OPEN_EXCLUSIVE  (1u << 3) /* with CREATE: fail with ALREADY_EXISTS */
#define JELLY_OPEN_TRUNCATE   (1u << 4)
#define JELLY_OPEN_APPEND     (1u << 5) /* every write goes to the end */
#define JELLY_OPEN_DIRECTORY  (1u << 6) /* must be a directory (for SYS_DIRECTORY_READ) */
#define JELLY_OPEN_NOFOLLOW   (1u << 7) /* do not follow a symbolic link in the last component */

/* SYS_FILE_SEEK whence */
#define JELLY_SEEK_SET        0
#define JELLY_SEEK_CURRENT    1
#define JELLY_SEEK_END        2

/* SYS_PATH_STAT flags */
#define JELLY_STAT_NOFOLLOW   (1u << 0)

#define JELLY_FILE_TYPE_FILE      1
#define JELLY_FILE_TYPE_DIRECTORY 2
#define JELLY_FILE_TYPE_SYMLINK   3
#define JELLY_FILE_TYPE_DEVICE    4 /* character device (/dev/console, /dev/null, ...) */
#define JELLY_FILE_TYPE_PIPE      5

#define JELLY_NAME_MAX        255

typedef struct {
    uint32_t type;   /* JELLY_FILE_TYPE_* */
    uint32_t mode;   /* permission bits, 0777 */
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t inode;
} jelly_stat_t;

typedef struct {
    uint32_t type;
    uint32_t name_length;
    uint64_t inode;
    char     name[JELLY_NAME_MAX + 1]; /* NUL terminated */
} jelly_dirent_t;

/* --- Programs (ABI version 3) -------------------------------------------- */

#define JELLY_SPAWN_MAX_HANDLES 8
#define JELLY_SPAWN_MAX_STRINGS 256   /* argv and envp each */
#define JELLY_SPAWN_MAX_BYTES   32768 /* all argument and environment strings */

/* Conventional startup handle slots */
#define JELLY_STDIN  0
#define JELLY_STDOUT 1
#define JELLY_STDERR 2

typedef struct {
    const char        *path;
    uint64_t           path_length;
    const char *const *argv;          /* NUL-terminated strings */
    uint32_t           argc;
    uint32_t           envc;
    const char *const *envp;          /* "NAME=value" */
    uint32_t           handle_count;
    uint32_t           flags;         /* none defined, must be 0 */
    jelly_handle_t     handles[JELLY_SPAWN_MAX_HANDLES]; /* copied to the child with the same rights */
} jelly_spawn_t;

/* Passed to a spawned program's entry point in RDI (in its own memory, on its stack). */
#define JELLY_STARTUP_VERSION 1
typedef struct {
    uint32_t       version;
    uint32_t       argc;
    char         **argv;              /* argv[argc] == NULL */
    uint32_t       envc;
    uint32_t       handle_count;
    char         **envp;              /* envp[envc] == NULL */
    jelly_handle_t handles[JELLY_SPAWN_MAX_HANDLES];
} jelly_startup_t;

#define JELLY_PROCESS_RUNNING 1
#define JELLY_PROCESS_EXITED  2

typedef struct {
    uint64_t pid;
    uint32_t state;      /* JELLY_PROCESS_* */
    int32_t  exit_code;  /* valid once exited; JELLY_EXIT_FAULT after a CPU fault */
    char     name[32];
} jelly_process_info_t;

/* --- Networking (ABI version 4) ------------------------------------------- */

/* Addresses and ports in jelly_sockaddr_in_t are in network byte order (as in BSD sockaddr_in). */
#define JELLY_AF_INET         2

#define JELLY_SOCK_STREAM     1 /* TCP */
#define JELLY_SOCK_DGRAM      2 /* UDP; with protocol JELLY_IPPROTO_ICMP: ICMP echo ("ping socket") */

#define JELLY_IPPROTO_ICMP    1
#define JELLY_IPPROTO_TCP     6
#define JELLY_IPPROTO_UDP     17

typedef struct {
    uint16_t family;   /* JELLY_AF_INET */
    uint16_t port;     /* network order */
    uint32_t address;  /* network order */
    uint8_t  zero[8];
} jelly_sockaddr_in_t;

/* SYS_SOCKET_SHUTDOWN */
#define JELLY_SHUT_READ       0
#define JELLY_SHUT_WRITE      1
#define JELLY_SHUT_BOTH       2

/* SYS_SOCKET_SEND / RECEIVE flags */
#define JELLY_MSG_PEEK        (1u << 0) /* receive without removing the data */
#define JELLY_MSG_DONTWAIT    (1u << 1) /* WOULD_BLOCK instead of blocking */

/* SYS_SOCKET_SET_OPTION */
#define JELLY_SO_RECEIVE_TIMEOUT 1 /* ns, 0 = forever */
#define JELLY_SO_SEND_TIMEOUT    2 /* ns, 0 = forever; also limits connect */
#define JELLY_SO_NONBLOCKING     3 /* 0/1 */
#define JELLY_SO_BROADCAST       4 /* 0/1: allow sending to 255.255.255.255 */
#define JELLY_SO_REUSE_ADDRESS   5 /* 0/1: bind to a port still in TIME_WAIT */
#define JELLY_SO_INTERFACE       6 /* interface index + 1, 0 = any: send and receive only there */

#define JELLY_SOCKET_STATE_UNCONNECTED 0
#define JELLY_SOCKET_STATE_LISTENING   1
#define JELLY_SOCKET_STATE_CONNECTING  2
#define JELLY_SOCKET_STATE_CONNECTED   3
#define JELLY_SOCKET_STATE_CLOSING     4 /* the peer or we have shut down a direction */
#define JELLY_SOCKET_STATE_CLOSED      5

typedef struct {
    uint32_t            type;      /* JELLY_SOCK_* */
    uint32_t            protocol;  /* JELLY_IPPROTO_* */
    uint32_t            state;     /* JELLY_SOCKET_STATE_* */
    uint32_t            pending_error; /* status_t of a failed connection, else 0 */
    jelly_sockaddr_in_t local;
    jelly_sockaddr_in_t remote;
    uint64_t            readable;  /* bytes (stream) or datagrams queued */
} jelly_socket_info_t;

#define JELLY_NETIF_NAME_MAX    16
#define JELLY_NETIF_UP          (1u << 0)
#define JELLY_NETIF_LOOPBACK    (1u << 1)
#define JELLY_NETIF_LINK        (1u << 2) /* carrier detected */
#define JELLY_NETIF_CONFIGURED  (1u << 3) /* has an IPv4 address */

typedef struct {
    uint32_t index;
    uint32_t flags;                    /* JELLY_NETIF_* */
    char     name[JELLY_NETIF_NAME_MAX];
    uint8_t  mac[6];
    uint16_t mtu;
    uint32_t address;                  /* IPv4, network order; 0 = none */
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    uint64_t rx_packets, rx_bytes, rx_dropped;
    uint64_t tx_packets, tx_bytes, tx_dropped;
} jelly_netif_info_t;

typedef struct {
    uint32_t address;                  /* network order; 0 removes the configuration */
    uint32_t netmask;
    uint32_t gateway;                  /* 0: none */
    uint32_t dns;                      /* 0: none */
} jelly_netif_config_t;

/* --- Graphics and input (ABI version 5) -------------------------------------- */

#define JELLY_WAIT_MANY_MAX       64
#define JELLY_CHANNEL_MAX_HANDLES 8
#define JELLY_SERVICE_NAME_MAX    63

#define JELLY_DISPLAY_ACQUIRED    (1u << 0) /* a display server owns it */

typedef struct {
    uint32_t index;
    uint32_t flags;           /* JELLY_DISPLAY_* */
    uint32_t width, height;   /* pixels */
    uint32_t pitch;           /* bytes per line */
    uint32_t bpp;             /* 32 */
    uint8_t  red_shift, red_size;
    uint8_t  green_shift, green_size;
    uint8_t  blue_shift, blue_size;
    uint8_t  reserved[2];
    uint64_t size;            /* bytes of the framebuffer mapping */
} jelly_display_info_t;

/* Standardized input events (README section 37). Key codes: <jelly/input.h>. */
#define JELLY_INPUT_KEY_DOWN     1 /* code = key, value = 1 (press) or 2 (repeat) */
#define JELLY_INPUT_KEY_UP       2
#define JELLY_INPUT_MOUSE_MOVE   3 /* dx, dy relative; with JELLY_INPUT_ABSOLUTE also x, y in 0..65535 */
#define JELLY_INPUT_MOUSE_BUTTON 4 /* code = JELLY_BUTTON_*, value = 1 pressed / 0 released */
#define JELLY_INPUT_MOUSE_WHEEL  5 /* value = steps, positive away from the user */

#define JELLY_INPUT_ABSOLUTE     (1u << 0)

typedef struct {
    uint64_t time_ns;
    uint32_t type;
    uint32_t code;
    int32_t  value;
    int32_t  dx, dy;
    int32_t  x, y;
    uint32_t flags;
    uint32_t device;          /* source device number */
    uint32_t reserved;
} jelly_input_event_t;

/* --- Desktop (ABI version 6) ------------------------------------------------- */

typedef struct {
    char     version[32];     /* kernel version, e.g. "0.10.0" */
    uint64_t uptime_ns;
    uint64_t memory_total;    /* bytes of RAM */
    uint64_t memory_free;
    uint32_t processes;       /* live processes */
    uint32_t abi_version;
    uint64_t realtime_ns;     /* wall clock, 0 if unknown */
} jelly_system_info_t;

/* SYS_SYSTEM_POWER actions */
#define JELLY_POWER_OFF    1
#define JELLY_POWER_REBOOT 2

/* Handle rights */
#define JELLY_RIGHT_READ      (1u << 0) /* receive from a channel */
#define JELLY_RIGHT_WRITE     (1u << 1) /* send to a channel, write shared memory */
#define JELLY_RIGHT_WAIT      (1u << 2) /* SYS_OBJECT_WAIT */
#define JELLY_RIGHT_SIGNAL    (1u << 3) /* signal or reset an event */
#define JELLY_RIGHT_MAP       (1u << 4) /* map shared memory */
#define JELLY_RIGHT_DUPLICATE (1u << 5) /* SYS_HANDLE_DUPLICATE */
#define JELLY_RIGHT_MANAGE    (1u << 6) /* SYS_PROCESS_KILL */
#define JELLY_RIGHTS_ALL      0x7Fu
/* Sockets: READ receive/accept, WRITE send/connect/bind/listen/options, WAIT readable */

/* SYS_MEMORY_ALLOCATE / SYS_SHM_MAP flags (memory is always readable) */
#define JELLY_MEMORY_WRITE    (1u << 0)
#define JELLY_MEMORY_EXEC     (1u << 1) /* never together with WRITE */

/* SYS_EVENT_CREATE flags */
#define JELLY_EVENT_AUTO_RESET (1u << 0) /* a satisfied wait resets the event */

/* Timeouts */
#define JELLY_WAIT_FOREVER    UINT64_MAX
#define JELLY_NO_WAIT         0

/* Exit code of a process terminated because of a CPU fault */
#define JELLY_EXIT_FAULT      (-1)

#endif
