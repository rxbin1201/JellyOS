# JellyOS System Call ABI

**ABI version:** 10 (`JELLY_SYSCALL_ABI_VERSION`). Version 1 has calls 0–22; version 2 adds the file calls 23–40 and status codes 16–20; version 3 adds programs, pipes and power (41–45), the `MANAGE` right and the file types `DEVICE` and `PIPE`; version 4 adds networking (46–58) and status codes 21–25; version 5 adds graphics and input (59–67); version 6 adds the desktop calls (68–70); version 7 adds audio devices (71–75) and the gamepad input events; version 8 adds the display driver calls (76–79) and the display flags `CURSOR`, `VBLANK` and `FLIP`; version 9 adds display modes and hot plug (80–82), the display flags `MODES` and `DISCONNECTED` and the fields `refresh_mhz` and `generation` of `jelly_display_info_t`. Version 10 adds switching the screen off (83) and the display flags `POWER` and `OFF`.
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
| `RDI` | Programs started with `SYS_PROCESS_SPAWN`: pointer to the `jelly_startup_t` block (below) |
| `RDI`, `RSI`, `RDX` | Programs started by the kernel tests directly: startup arguments 0–2 |
| `RSP` | Top of a 256 KiB stack, `RSP ≡ 8 (mod 16)` as after a `call` |
| `RFLAGS` | `IF` set, everything else clear |
| FPU/SSE | `FCW = 0x037F`, `MXCSR = 0x1F80` |

Threads created with `SYS_THREAD_CREATE` start the same way at `entry` with
`RDI = arg0` and `RSI = arg1`.

The startup block lies on the new stack, above it the argument and
environment strings and the `argv`/`envp` arrays (each ending with `NULL`):

```c
typedef struct {
    uint32_t       version;          /* JELLY_STARTUP_VERSION (1) */
    uint32_t       argc;
    char         **argv;
    uint32_t       envc;
    uint32_t       handle_count;
    char         **envp;
    jelly_handle_t handles[8];       /* 0 stdin, 1 stdout, 2 stderr by convention */
} jelly_startup_t;
```

libc's entry code (`userspace/libc/crt0.c`) turns this into
`main(argc, argv, envp)`, `environ` and `stdin`/`stdout`/`stderr`. Bare libos
programs (`userspace/libos/start.c`) call `main(arg0, arg1, arg2)`.

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

### Files (version 2)

Paths are passed as (pointer, length) without a terminating NUL, at most 1023
bytes, and must not contain NUL. Relative paths are resolved against the
process's working directory. See [storage.md](../architecture/storage.md) for
the semantics.

| # | Name | Arguments | Output | Rights / notes |
|---|---|---|---|---|
| 23 | `SYS_FILE_OPEN` | `path, length, flags, mode, jelly_handle_t *file` | handle (`READ`/`WRITE` per flags, `DUPLICATE`) | Flags `JELLY_OPEN_READ`, `WRITE`, `CREATE`, `EXCLUSIVE`, `TRUNCATE`, `APPEND`, `DIRECTORY`, `NOFOLLOW`; `mode` for new files |
| 24 | `SYS_FILE_READ` | `handle, buffer, size, size_t *done` | bytes read (0 at end of file) | Needs `READ` |
| 25 | `SYS_FILE_WRITE` | `handle, buffer, size, size_t *done` | bytes written (also reported on errors such as `NO_SPACE`) | Needs `WRITE` |
| 26 | `SYS_FILE_SEEK` | `handle, int64_t offset, whence, uint64_t *position` | new position | `JELLY_SEEK_SET`, `CURRENT`, `END`; may go past the end |
| 27 | `SYS_FILE_TRUNCATE` | `handle, uint64_t size` | | Needs `WRITE`; growing fills with zeros |
| 28 | `SYS_FILE_STAT` | `handle, jelly_stat_t *stat` | | |
| 29 | `SYS_DIRECTORY_READ` | `handle, jelly_dirent_t *entry` | next entry | Needs `READ`; `NOT_FOUND` after the last entry; never `.` or `..` |
| 30 | `SYS_PATH_STAT` | `path, length, flags, jelly_stat_t *stat` | | `JELLY_STAT_NOFOLLOW` |
| 31 | `SYS_PATH_MKDIR` | `path, length, mode` | | |
| 32 | `SYS_PATH_UNLINK` | `path, length` | | Files, symbolic links, empty directories |
| 33 | `SYS_PATH_RENAME` | `from, from_length, to, to_length` | | Replaces an existing target of the same kind; not across file systems |
| 34 | `SYS_PATH_SYMLINK` | `target, target_length, path, length` | | The target is stored as given |
| 35 | `SYS_PATH_READLINK` | `path, length, buffer, size, size_t *length` | target (not NUL-terminated) | `BUFFER_TOO_SMALL` reports the length |
| 36 | `SYS_CHDIR` | `path, length` | | Must be a directory |
| 37 | `SYS_GETCWD` | `buffer, size, size_t *length` | NUL-terminated path | `BUFFER_TOO_SMALL` reports the length |
| 38 | `SYS_FS_SYNC` | | | Flushes all mounted file systems and their devices |
| 39 | `SYS_MOUNT` | `path, length, device, device_length, type, type_length` | | Root only. `type_length` 0 probes all file system types |
| 40 | `SYS_UNMOUNT` | `path, length` | | Root only. `BUSY` while files are open |

Device files (`/dev`) and pipes are **streams**: they have no position
(`SYS_FILE_SEEK` returns `NOT_SUPPORTED`), and a read returns what is
available, blocking only while nothing is. A read of 0 bytes means end of
file: a pipe without writers, or Ctrl-D on the console.

### Programs, pipes and power (version 3)

| # | Name | Arguments | Output | Rights / notes |
|---|---|---|---|---|
| 41 | `SYS_PROCESS_SPAWN` | `const jelly_spawn_t *request, jelly_handle_t *process` | process handle (`WAIT`, `MANAGE`, `DUPLICATE`) | Needs execute permission on the program file. The child inherits credentials and working directory. Startup handles are copied with the caller's rights |
| 42 | `SYS_PROCESS_INFO` | `handle, jelly_process_info_t *info` | pid, state, exit code, name | |
| 43 | `SYS_PROCESS_KILL` | `handle, int32_t exit_code` | | Needs `MANAGE`. Not for the caller itself or critical processes (`ACCESS_DENIED`) |
| 44 | `SYS_PIPE_CREATE` | `jelly_handle_t *read_end, *write_end` | two file handles (`READ` / `WRITE`, `DUPLICATE`) | 16 KiB buffer. Writing blocks while full and fails with `PEER_CLOSED` without readers |
| 45 | `SYS_SYSTEM_POWER` | `uint32_t action` | returns only on failure | Root only. `JELLY_POWER_OFF` (ACPI S5), `JELLY_POWER_REBOOT`. File systems are synced first |

```c
typedef struct {
    const char        *path;          /* program file, relative to the working directory */
    uint64_t           path_length;
    const char *const *argv;          /* NUL-terminated strings */
    uint32_t           argc;
    uint32_t           envc;
    const char *const *envp;          /* "NAME=value" */
    uint32_t           handle_count;  /* at most 8 */
    uint32_t           flags;         /* 0 */
    jelly_handle_t     handles[8];
} jelly_spawn_t;                      /* at most 256 arguments, 256 variables, 32 KiB of strings */

typedef struct {
    uint64_t pid;
    uint32_t state;                   /* JELLY_PROCESS_RUNNING, JELLY_PROCESS_EXITED */
    int32_t  exit_code;
    char     name[32];
} jelly_process_info_t;
```

### Networking (version 4)

Addresses are `jelly_sockaddr_in_t`, which has the same layout as BSD
`sockaddr_in`. Ports and addresses are in network byte order. See
[networking.md](../architecture/networking.md) for the protocol behavior.

| # | Name | Arguments | Output | Rights / notes |
|---|---|---|---|---|
| 46 | `SYS_SOCKET_CREATE` | `domain, type, protocol, jelly_handle_t *socket` | handle (`READ`, `WRITE`, `WAIT`, `DUPLICATE`) | `JELLY_AF_INET`; `JELLY_SOCK_STREAM` (TCP), `JELLY_SOCK_DGRAM` (UDP, or ICMP echo with `JELLY_IPPROTO_ICMP`) |
| 47 | `SYS_SOCKET_BIND` | `handle, const jelly_sockaddr_in_t *address` | | Needs `WRITE`. Port 0 picks an ephemeral port (49152–65535). `ADDRESS_IN_USE` |
| 48 | `SYS_SOCKET_CONNECT` | `handle, address` | | Needs `WRITE`. TCP blocks until established (send timeout applies; `WOULD_BLOCK` if non-blocking). `CONNECTION_REFUSED`, `TIMEOUT`, `UNREACHABLE`. Datagram: sets the default peer (address 0 removes it) |
| 49 | `SYS_SOCKET_LISTEN` | `handle, backlog` | | Needs `WRITE`. TCP only, backlog 1–128 |
| 50 | `SYS_SOCKET_ACCEPT` | `handle, jelly_handle_t *connection, jelly_sockaddr_in_t *peer` | new socket handle, peer (if not NULL) | Needs `READ`. Blocks (receive timeout) |
| 51 | `SYS_SOCKET_SEND` | `handle, data, size, const jelly_sockaddr_in_t *to, flags, size_t *done` | bytes sent | Needs `WRITE`. Streams: at most 64 KiB per call, may block for buffer space; `PEER_CLOSED` after shutdown. Datagrams: `to` or the connected peer; broadcasts need `JELLY_SO_BROADCAST` |
| 52 | `SYS_SOCKET_RECEIVE` | `handle, buffer, size, jelly_sockaddr_in_t *from, flags, size_t *done` | bytes, sender | Needs `READ`. 0 bytes = end of stream. `JELLY_MSG_PEEK`, `JELLY_MSG_DONTWAIT`. Pending errors (`CONNECTION_REFUSED`, `CONNECTION_RESET`) are reported here |
| 53 | `SYS_SOCKET_SHUTDOWN` | `handle, how` | | `JELLY_SHUT_READ`, `WRITE` (TCP sends FIN), `BOTH` |
| 54 | `SYS_SOCKET_SET_OPTION` | `handle, option, uint64_t value` | | Needs `WRITE`. `RECEIVE_TIMEOUT`/`SEND_TIMEOUT` (ns, 0 = forever), `NONBLOCKING`, `BROADCAST`, `REUSE_ADDRESS`, `INTERFACE` (index + 1) |
| 55 | `SYS_SOCKET_INFO` | `handle, jelly_socket_info_t *info` | type, state, addresses, pending error, readable amount | |
| 56 | `SYS_NET_INTERFACE_INFO` | `index, jelly_netif_info_t *info` | name, flags, MAC, MTU, IPv4 configuration, counters | `NOT_FOUND` after the last interface |
| 57 | `SYS_NET_CONFIGURE` | `index, const jelly_netif_config_t *config` | | Root only. Address 0 removes the configuration. The netmask must be contiguous and the gateway on the subnet. Not for `lo` |
| 58 | `SYS_NET_RESOLVE` | `name, length, uint32_t *address` | IPv4 address | Dotted quads, `localhost`, otherwise DNS. `NOT_FOUND` (no such name), `TIMEOUT`, `UNREACHABLE` (no name server) |

Socket handles are waitable (readable, end of stream or error) and can be
used with `SYS_FILE_READ`/`SYS_FILE_WRITE`. Handles always fit a positive
`int` (bit 31 is clear), so libc uses them directly as socket descriptors.

New status codes: `CONNECTION_REFUSED` (21), `CONNECTION_RESET` (22),
`NOT_CONNECTED` (23), `ADDRESS_IN_USE` (24), `UNREACHABLE` (25).

### Graphics and input (version 5)

See [graphics.md](../architecture/graphics.md).

| # | Name | Arguments | Output | Rights / notes |
|---|---|---|---|---|
| 59 | `SYS_OBJECT_WAIT_MANY` | `const jelly_handle_t *handles, count, timeout_ns, uint32_t *index` | index of a signaled object | Every handle needs `WAIT`; at most 64. `TIMEOUT` |
| 60 | `SYS_CHANNEL_SEND_HANDLES` | `handle, data, size, const jelly_handle_t *handles, count` | | Needs `WRITE`. Up to 8 handles, **moved** with their rights: they are closed for the sender on success. Not the channel's own endpoints |
| 61 | `SYS_CHANNEL_RECEIVE_HANDLES` | `handle, buffer, size, size_t *actual, jelly_handle_t handles[8], uint32_t *count` | message, new handles | Needs `READ`. `BUFFER_TOO_SMALL` reports the size and keeps the message. `SYS_CHANNEL_RECEIVE` closes attached handles |
| 62 | `SYS_SERVICE_REGISTER` | `name, length, channel` | | Root only. The registry takes over the channel end (closed for the caller). `ALREADY_EXISTS` while the previous server lives |
| 63 | `SYS_SERVICE_CONNECT` | `name, length, jelly_handle_t *channel` | channel to the server | The server receives the other end in a `connect` message. `NOT_FOUND` |
| 64 | `SYS_DISPLAY_INFO` | `index, jelly_display_info_t *info` | size, pitch, pixel format, flags | `NOT_FOUND` after the last display |
| 65 | `SYS_DISPLAY_ACQUIRE` | `index, jelly_handle_t *framebuffer` | memory handle (`MAP`, `WRITE`) | Root only. Exclusive (`BUSY`); map it with `SYS_SHM_MAP` (write-combining). The kernel console resumes when the object is released |
| 66 | `SYS_INPUT_OPEN` | `jelly_handle_t *input` | input queue (`READ`, `WAIT`) | Root only. Each queue receives every event (512 buffered, oldest dropped) |
| 67 | `SYS_INPUT_READ` | `handle, jelly_input_event_t *events, count, size_t *read` | events | `WOULD_BLOCK` when empty |

### Desktop (version 6)

See [desktop.md](../architecture/desktop.md).

| # | Name | Arguments | Output | Rights / notes |
|---|---|---|---|---|
| 68 | `SYS_PROCESS_SPAWN_AS` | `const jelly_spawn_t *request, uid, gid, jelly_handle_t *process` | process handle | Root only: like `SYS_PROCESS_SPAWN`, but the child runs with the given uid and gid (login starts sessions with it) |
| 69 | `SYS_CLOCK_REALTIME` | `uint64_t *ns` | nanoseconds since 1970-01-01 UTC | From the CMOS RTC read at boot plus the monotonic clock. `NOT_SUPPORTED` without an RTC |
| 70 | `SYS_SYSTEM_INFO` | `jelly_system_info_t *info` | version, uptime, memory total/free, live processes, ABI version, wall-clock time | |

### Audio (version 7)

See [audio.md](../architecture/audio.md). These calls are for the audio
server; applications use the audio API (`audio/client/audio.h`). Frames are
interleaved signed 16-bit samples in the device's format.

| # | Name | Arguments | Output | Rights / notes |
|---|---|---|---|---|
| 71 | `SYS_AUDIO_INFO` | `index, jelly_audio_info_t *info` | name, directions (and `JELLY_AUDIO_MONITOR`: the sound of a monitor), rate, channels, period, buffer size, frame and underrun/overrun counters | `NOT_FOUND` after the last device |
| 72 | `SYS_AUDIO_OPEN` | `index, jelly_handle_t *device` | device handle (`READ`, `WRITE`, `WAIT`) | Root only. Exclusive (`BUSY`). Signaled while playback runs with at most two periods queued, or a period of recorded frames waits. Closing stops the device |
| 73 | `SYS_AUDIO_WRITE` | `handle, const int16_t *frames, count, size_t *written` | frames queued | Needs `WRITE`. Never blocks: takes what fits |
| 74 | `SYS_AUDIO_READ` | `handle, int16_t *frames, count, size_t *read` | recorded frames | Needs `READ`. Never blocks: 0 frames if none wait |
| 75 | `SYS_AUDIO_CONTROL` | `handle, command, value, uint64_t *result` | | Needs `WRITE`. `JELLY_AUDIO_PLAYBACK_ENABLE` / `CAPTURE_ENABLE` (value 1 or 0; disabling empties the buffer), `PLAYBACK_QUEUED` / `CAPTURE_QUEUED` (frames, in `*result`) |

### Display drivers (ABI version 8)

What a display can do beyond being a framebuffer is told by the flags in
`jelly_display_info_t`: `JELLY_DISPLAY_CURSOR`, `JELLY_DISPLAY_VBLANK`,
`JELLY_DISPLAY_FLIP`. They come from the display's driver in the kernel; a
display without one (the UEFI framebuffer) has none of them and every call
below answers `NOT_SUPPORTED`. The calls are the same for every graphics
driver. All four are root only and need the display to be acquired.

| Nr | Name | Arguments | Result | Notes |
|---|---|---|---|---|
| 76 | `SYS_DISPLAY_CURSOR` | `index, const jelly_cursor_t *cursor` | | Hardware pointer. With `JELLY_CURSOR_IMAGE`, `pixels` is a new image of 64×64 pixels, 0xAARRGGBB; `x`, `y` is the top left corner of the image on the screen (may be negative or beyond the edge); shown only with `JELLY_CURSOR_VISIBLE` |
| 77 | `SYS_DISPLAY_VBLANK` | `index, timeout_ns` | | Sleeps until the next frame begins; after a flip, until the new framebuffer is the one being shown. `TIMEOUT` |
| 78 | `SYS_DISPLAY_BUFFER` | `index, buffer, jelly_handle_t *memory` | memory handle (`MAP`, `WRITE`) | The second framebuffer (`buffer` = 1) of a display with `FLIP`; same size and format as the first |
| 79 | `SYS_DISPLAY_FLIP` | `index, buffer` | | Shows framebuffer 0 or 1 from the next frame on. Returns at once; wait with `SYS_DISPLAY_VBLANK`. When the owner releases the display, buffer 0 is shown again and the pointer hidden |

```c
typedef struct {
    uint32_t        flags;  /* JELLY_CURSOR_IMAGE | JELLY_CURSOR_VISIBLE */
    int32_t         x, y;
    const uint32_t *pixels; /* with JELLY_CURSOR_IMAGE */
} jelly_cursor_t;
```

### Display modes and hot plug (ABI version 9)

A display with the flag `JELLY_DISPLAY_MODES` has a driver that can switch
modes. The framebuffer memory does not move when the mode changes: its
size (`jelly_display_info_t.size`) is enough for every mode, and only
`width`, `height`, `pitch` and `refresh_mhz` change. A mapping made before
stays valid. `JELLY_DISPLAY_DISCONNECTED` is set while the driver sees no
monitor. `generation` counts every such change.

| Nr | Name | Arguments | Result | Notes |
|---|---|---|---|---|
| 80 | `SYS_DISPLAY_MODES` | `index, jelly_display_mode_t *modes, max, uint32_t *count` | up to `max` modes; `*count` = how many there are | For everyone. A display without a driver has one mode: what it shows. At most 32 |
| 81 | `SYS_DISPLAY_SET_MODE` | `index, mode` | | Root only. `mode` = position in the list. Framebuffer 0 is shown afterwards. `NOT_SUPPORTED` without `MODES`, `DEVICE_ERROR` if the mode does not come up (the one before is back) |
| 82 | `SYS_DISPLAY_WATCH` | `index, jelly_handle_t *event` | event handle (`WAIT`, `SIGNAL`) | Root only. Signaled after every change of mode, list of modes or connection, also by another program or by the driver itself (another monitor). The watcher resets it (`SYS_EVENT_RESET`) and reads `SYS_DISPLAY_INFO` again |

### The screen off and on (ABI version 10)

A display with the flag `JELLY_DISPLAY_POWER` has a driver that can stop
the signal to the monitor, which then goes to standby. While the screen is
off, `JELLY_DISPLAY_OFF` is set, and flips, pointer moves and waits for a
frame return `BUSY`; the framebuffers stay mapped and keep their contents.
The kernel switches the screen on again when a key or a button goes down
or the mouse moves (not in the first half second), when the mode is
changed, when the display's owner goes away, and for a panic. Both
changes signal the event of `SYS_DISPLAY_WATCH` and count in `generation`.

| Nr | Name | Arguments | Result | Notes |
|---|---|---|---|---|
| 83 | `SYS_DISPLAY_POWER` | `index, on` | | Root only. `on` = 0: off, else on. `NOT_SUPPORTED` without `POWER`; an error for "off" means the screen is still on |

```c
typedef struct {
    uint32_t width, height;
    uint32_t refresh_mhz;   /* frames per 1000 seconds (60000 = 60 Hz); 0 if unknown */
    uint32_t flags;         /* JELLY_MODE_CURRENT, JELLY_MODE_PREFERRED */
} jelly_display_mode_t;
```

Version 7 also adds two input event types (`SYS_INPUT_READ`):
`JELLY_INPUT_GAMEPAD_BUTTON` (`code` = 0-based button, `value` = 1/0) and
`JELLY_INPUT_GAMEPAD_AXIS` (`code` = `JELLY_AXIS_*`, `value` =
-32768..32767). See [input.md](../architecture/input.md).

```c
typedef struct {
    uint64_t time_ns;
    uint32_t type;          /* JELLY_INPUT_KEY_DOWN, KEY_UP, MOUSE_MOVE, MOUSE_BUTTON, MOUSE_WHEEL, GAMEPAD_BUTTON, GAMEPAD_AXIS */
    uint32_t code;          /* JELLY_KEY_* (<jelly/input.h>, evdev numbering), JELLY_BUTTON_* */
    int32_t  value;         /* key: 1 press, 2 repeat; button: 1/0; wheel: steps */
    int32_t  dx, dy;        /* relative motion */
    int32_t  x, y;          /* 0..65535 with JELLY_INPUT_ABSOLUTE */
    uint32_t flags;
    uint32_t device;
    uint32_t reserved;
} jelly_input_event_t;
```

```c
typedef struct {
    uint32_t type;   /* JELLY_FILE_TYPE_FILE, _DIRECTORY, _SYMLINK, _DEVICE, _PIPE */
    uint32_t mode;   /* permission bits 0777 */
    uint32_t uid, gid;
    uint64_t size;
    uint64_t inode;
} jelly_stat_t;

typedef struct {
    uint32_t type;
    uint32_t name_length;
    uint64_t inode;
    char     name[256]; /* NUL terminated */
} jelly_dirent_t;
```

## Handles

Handles are 32-bit values local to a process: bits 0–15 hold the slot and
bits 16–30 a generation (1–32767, bit 31 always clear), so a closed handle
never becomes valid again by accident. `0` is never a valid handle. Every handle carries rights:

| Right | Allows |
|---|---|
| `JELLY_RIGHT_READ` | Receiving from a channel |
| `JELLY_RIGHT_WRITE` | Sending to a channel, mapping shared memory writable |
| `JELLY_RIGHT_WAIT` | `SYS_OBJECT_WAIT` |
| `JELLY_RIGHT_SIGNAL` | Signaling or resetting an event |
| `JELLY_RIGHT_MAP` | Mapping shared memory |
| `JELLY_RIGHT_DUPLICATE` | Duplicating the handle (with equal or fewer rights) |
| `JELLY_RIGHT_MANAGE` | Killing a process (`SYS_PROCESS_KILL`) |

Sockets: `READ` to receive and accept, `WRITE` to send, connect, bind,
listen and set options.

Waitable objects and their signaled state:

| Object | Signaled when |
|---|---|
| Process | It has exited |
| Thread | It has exited |
| Event | Signaled (until reset; auto-reset events clear on a satisfied wait) |
| Channel endpoint | A message is queued or the peer is closed |
| File | Not waitable |

Resource limits per process (initial values): 256 handles, 64 threads, 64 MiB
of memory.

## Faults

A CPU exception in user mode ends the process with exit code
`JELLY_EXIT_FAULT` (-1) and a reason such as
`page fault at 0x0: null pointer dereference (user instruction fetch) at rip 0x0`.
The kernel and all other processes keep running.
