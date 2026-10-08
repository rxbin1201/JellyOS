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

#define JELLY_SYSCALL_ABI_VERSION 10

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
    /* ABI version 7: audio (and gamepad input events) */
    SYS_AUDIO_INFO       = 71, /* (uint32_t index, jelly_audio_info_t *info)                 NOT_FOUND past the last */
    SYS_AUDIO_OPEN       = 72, /* (uint32_t index, jelly_handle_t *device)                   root only, exclusive */
    SYS_AUDIO_WRITE      = 73, /* (handle, const int16_t *frames, size_t count, size_t *done)   never blocks */
    SYS_AUDIO_READ       = 74, /* (handle, int16_t *frames, size_t count, size_t *done)         never blocks */
    SYS_AUDIO_CONTROL    = 75, /* (handle, uint32_t command, uint64_t value, uint64_t *result) */
    /* ABI version 8: what a graphics driver adds to a display (root only; NOT_SUPPORTED without it) */
    SYS_DISPLAY_CURSOR   = 76, /* (uint32_t index, const jelly_cursor_t *cursor)             hardware pointer */
    SYS_DISPLAY_VBLANK   = 77, /* (uint32_t index, uint64_t timeout_ns)                      wait for the next frame */
    SYS_DISPLAY_BUFFER   = 78, /* (uint32_t index, uint32_t buffer, jelly_handle_t *memory)  the second framebuffer (1) */
    SYS_DISPLAY_FLIP     = 79, /* (uint32_t index, uint32_t buffer)                          show buffer 0 or 1 from the next frame */
    /* ABI version 9: display modes and hot plug */
    SYS_DISPLAY_MODES    = 80, /* (uint32_t index, jelly_display_mode_t *modes, uint32_t max, uint32_t *count) */
    SYS_DISPLAY_SET_MODE = 81, /* (uint32_t index, uint32_t mode)                            root only */
    SYS_DISPLAY_WATCH    = 82, /* (uint32_t index, jelly_handle_t *event)                    root only; signaled on changes */
    /* ABI version 10: the screen off and on */
    SYS_DISPLAY_POWER    = 83, /* (uint32_t index, uint32_t on)                              root only */
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
/* ABI version 8: capabilities a graphics driver adds */
#define JELLY_DISPLAY_CURSOR      (1u << 1) /* hardware pointer: SYS_DISPLAY_CURSOR */
#define JELLY_DISPLAY_VBLANK      (1u << 2) /* SYS_DISPLAY_VBLANK */
#define JELLY_DISPLAY_FLIP        (1u << 3) /* two framebuffers: SYS_DISPLAY_BUFFER, SYS_DISPLAY_FLIP */
/* ABI version 9 */
#define JELLY_DISPLAY_MODES       (1u << 4) /* the mode can be changed: SYS_DISPLAY_SET_MODE */
#define JELLY_DISPLAY_DISCONNECTED (1u << 5) /* the driver sees no monitor */
/* ABI version 10 */
#define JELLY_DISPLAY_POWER       (1u << 6) /* the screen can be switched off: SYS_DISPLAY_POWER */
#define JELLY_DISPLAY_OFF         (1u << 7) /* it is off: no signal to the monitor until it is switched on or input comes */

#define JELLY_DISPLAY_MODE_MAX    32
#define JELLY_MODE_CURRENT        (1u << 0) /* the mode being shown */
#define JELLY_MODE_PREFERRED      (1u << 1) /* the monitor's best mode */

typedef struct {
    uint32_t width, height;   /* pixels */
    uint32_t refresh_mhz;     /* frames per 1000 seconds (60000 = 60 Hz); 0 if unknown */
    uint32_t flags;           /* JELLY_MODE_* */
} jelly_display_mode_t;

#define JELLY_CURSOR_SIZE         64        /* hardware pointer images are 64x64, 0xAARRGGBB */
#define JELLY_CURSOR_IMAGE        (1u << 0) /* `pixels` holds a new image */
#define JELLY_CURSOR_VISIBLE      (1u << 1)

typedef struct {
    uint32_t        flags;   /* JELLY_CURSOR_* */
    int32_t         x, y;    /* top left corner of the image on the screen (may be negative) */
    const uint32_t *pixels;  /* JELLY_CURSOR_SIZE * JELLY_CURSOR_SIZE pixels with JELLY_CURSOR_IMAGE */
} jelly_cursor_t;

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
    uint64_t size;            /* bytes of the framebuffer mapping (stays when the mode changes) */
    /* ABI version 9 */
    uint32_t refresh_mhz;     /* frames per 1000 seconds; 0 if unknown */
    uint32_t generation;      /* counts changes of geometry, modes, connection and power (see SYS_DISPLAY_WATCH) */
} jelly_display_info_t;

/* Standardized input events (README section 37). Key codes: <jelly/input.h>. */
#define JELLY_INPUT_KEY_DOWN     1 /* code = key, value = 1 (press) or 2 (repeat) */
#define JELLY_INPUT_KEY_UP       2
#define JELLY_INPUT_MOUSE_MOVE   3 /* dx, dy relative; with JELLY_INPUT_ABSOLUTE also x, y in 0..65535 */
#define JELLY_INPUT_MOUSE_BUTTON 4 /* code = JELLY_BUTTON_*, value = 1 pressed / 0 released */
#define JELLY_INPUT_MOUSE_WHEEL  5 /* value = steps, positive away from the user */
/* ABI version 7: gamepads */
#define JELLY_INPUT_GAMEPAD_BUTTON 6 /* code = button number (0-based), value = 1 pressed / 0 released */
#define JELLY_INPUT_GAMEPAD_AXIS   7 /* code = JELLY_AXIS_*, value = -32768..32767 (hat: -1, 0, 1 scaled the same) */

#define JELLY_AXIS_LEFT_X    0
#define JELLY_AXIS_LEFT_Y    1
#define JELLY_AXIS_RIGHT_X   2
#define JELLY_AXIS_RIGHT_Y   3
#define JELLY_AXIS_TRIGGER_L 4
#define JELLY_AXIS_TRIGGER_R 5
#define JELLY_AXIS_HAT_X     6
#define JELLY_AXIS_HAT_Y     7

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

/* --- Audio (ABI version 7) --------------------------------------------------- */

/*
 * An audio device plays and records interleaved signed 16-bit little-endian
 * frames at the rate and channel count it reports (JellyOS drivers use
 * 48000 Hz stereo). The audio server owns the device; applications talk to
 * the server.
 */
#define JELLY_AUDIO_PLAYBACK (1u << 0)
#define JELLY_AUDIO_CAPTURE  (1u << 1)
#define JELLY_AUDIO_MONITOR  (1u << 2) /* the sound of a monitor (HDMI, DisplayPort): it may have no loudspeakers */

typedef struct {
    uint32_t index;
    uint32_t flags;            /* JELLY_AUDIO_* directions the device supports */
    uint32_t rate;             /* frames per second */
    uint32_t channels;
    uint32_t period;           /* frames the hardware takes or delivers at a time */
    uint32_t buffer;           /* frames the kernel buffers per direction */
    char     name[32];
    uint64_t played_frames;    /* frames sent to the hardware so far */
    uint64_t captured_frames;
    uint64_t underruns;        /* periods played (partly) as silence for lack of data */
    uint64_t overruns;         /* times recorded frames were dropped because nobody read them */
} jelly_audio_info_t;

/*
 * SYS_AUDIO_CONTROL commands. The device handle is signaled (JELLY_RIGHT_WAIT)
 * while playback is enabled and at most two periods are queued, or while
 * recorded frames of at least one period are waiting.
 */
#define JELLY_AUDIO_PLAYBACK_ENABLE 1 /* value 1 starts the output (silence while nothing is queued), 0 stops and empties it */
#define JELLY_AUDIO_CAPTURE_ENABLE  2 /* value 1 starts recording, 0 stops it and empties the buffer */
#define JELLY_AUDIO_PLAYBACK_QUEUED 3 /* result: frames queued and not yet played */
#define JELLY_AUDIO_CAPTURE_QUEUED  4 /* result: recorded frames waiting to be read */

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
