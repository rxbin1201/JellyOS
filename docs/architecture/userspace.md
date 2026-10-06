# JellyOS Userspace: libc, init, Service Manager and Shell

**Code:** [`userspace/`](../../userspace/), headers in [`sdk/include/`](../../sdk/include/), image in [`tools/image_builder/`](../../tools/image_builder/)
**ABI:** [../abi/syscalls.md](../abi/syscalls.md) (version 3)

Phase 7 (milestone M6) boots into a command line:

```text
kernel ──spawn──▶ /init ──spawn──▶ /sbin/servicemanager ──spawn──▶ services
 (critical)                         reads /etc/services.conf        motd (oneshot)
                                                                     shell: /bin/sh ──spawn──▶ /bin/ls, ...
```

All programs are static ELF64 executables linked at 4 MiB
(`userspace/libos/user.ld`). They come from the initramfs (see
[storage.md](storage.md)).

## Libraries (README sections 28 and 30)

| Library | Role | Location |
|---|---|---|
| libos | Native JellyOS interface: one thin wrapper per system call, `status_t` results | `userspace/libos`, `<jelly/os.h>` |
| libc | Standard C interface on top of libos | `userspace/libc`, `<stdio.h>`, `<stdlib.h>`, ... |

Programs link `crt0.o`, then `libc.a` and `libos.a`. The SDK headers replace
the host's C library; GCC supplies only its freestanding headers
(`stddef.h`, `stdint.h`, `stdarg.h`, `stdbool.h`). User code is built with
`-nostdinc -isystem <gcc include>`.

### libc

| Header | Contents |
|---|---|
| `stdio.h` | `FILE` streams on file handles: `fopen` (`r w a` `+ b x`), buffered read/write, `fgets`, `ungetc`, `fseek`/`ftell`, the full `printf` family, `remove`, `rename`, `perror`; `fdopen_handle()` and `fhandle()` connect streams and handles |
| `stdlib.h` | `malloc`/`calloc`/`realloc`/`free`, `exit`/`atexit`/`abort`, `getenv`/`setenv`/`unsetenv`/`environ`, `strto*`/`ato*`, `qsort` (heap sort), `bsearch`, `rand` |
| `string.h` | Memory and string functions, `strtok_r`, `strdup`, `strerror` |
| `ctype.h` | ASCII character classes |
| `errno.h` | `errno` holds `status_t` values. `ENOENT` is `STATUS_NOT_FOUND`, and so on |
| `time.h` | `time`, `clock`, `timespec_get` on the monotonic clock (there is no wall clock yet) |
| `threads.h` | C11 `thrd_*` on JellyOS threads (64 KiB stacks), `mtx_*` on futexes |
| `dirent.h` | `opendir`/`readdir`/`closedir` |
| `process.h` | JellyOS: `process_spawn` (with `$PATH` search and explicit stdio handles), `process_wait`, `process_run`, `process_find` |
| `assert.h`, `limits.h` | as usual |

Design notes:

- **Startup:** `crt0` receives the `jelly_startup_t` block, sets `argv`,
  `environ` and the standard streams (startup handles 0–2), runs
  `main(argc, argv, envp)` and passes its result to `exit()`. `exit()` runs
  the `atexit` handlers and flushes all streams.
- **Streams:** stdout is line buffered on the console or a pipe and fully
  buffered into a file. stderr is unbuffered. Before reading from the console
  or a pipe, stdout is flushed so prompts are visible.
- **Heap:** small blocks come from 256 KiB arenas in an address-ordered free
  list (first fit, neighbors merge on free). Blocks of 128 KiB and more get a
  mapping of their own. A futex mutex protects the heap.
- **No signals and no fork:** `abort()` exits with code 134. Programs start
  other programs with `process_spawn()`.
- **errno** is process-wide until there is thread-local storage.

## init (README section 29)

`/init` ([`userspace/init/init.c`](../../userspace/init/init.c)) is the first
process. The kernel marks it critical, so its exit would panic the kernel.

1. Checks the file system skeleton: creates `/tmp` and `/etc` if they are
   missing and warns if `/dev/console` is missing.
2. Reads the kernel command line from `JELLY_CMDLINE`.
3. Normal boot: starts `/sbin/servicemanager` and restarts it if it exits.
   After 5 exits within 10 seconds it falls back to a rescue shell.
4. `recovery=1` (boot entry *Recovery*): starts no services and keeps a rescue
   shell (`/bin/sh`) running on the console.
5. `safe_mode=1` (boot entry *SafeMode*): passes `--safe-mode` to the service
   manager.

## Service manager (README section 31)

`/sbin/servicemanager`
([`userspace/services/servicemanager/servicemanager.c`](../../userspace/services/servicemanager/servicemanager.c))
reads `/etc/services.conf`:

```ini
[service shell]
description=Interactive shell on the console
command=/bin/sh             # program and arguments, separated by blanks
type=simple                 # simple (default) | oneshot
restart=always              # always | on-failure (default) | never
depends=motd                # names, separated by blanks or commas
control=yes                 # pass a control channel as startup handle 3
essential=yes               # started in safe mode
```

| Feature | Behavior |
|---|---|
| Start | In dependency order. A `simple` dependency must be running, a `oneshot` dependency must have finished with code 0 |
| Dependencies | Unknown names and cycles are found at load time; such services are marked failed |
| Failure detection | Exited processes are noticed within 20 ms |
| Restart | By policy, with a delay of 250 ms that doubles per quick failure (at most 8 s). After 5 failures within 10 s of running time the service is marked failed. Dependents of a failed service are not started |
| Stop | `SYS_PROCESS_KILL` with exit code 143; the service is not restarted |
| Safe mode | Only `essential` services and their dependencies |
| Logging | `servicemanager: ...` lines on the console |

There is no system call yet that waits for any of several objects, so the
manager polls its processes and control channels every 20 ms.

**Control protocol:** services with `control=yes` (the shell) receive one end
of a channel as startup handle 3. Requests are text messages, and each gets
one text reply:

| Request | Reply |
|---|---|
| `list` | One line per service: name, state (`stopped`, `running`, `done`, `waiting`, `failed`), PID, description or failure reason |
| `status NAME` | The service's line plus its last exit code |
| `start NAME` / `stop NAME` / `restart NAME` | Confirmation, or `error: ...` |

## Shell

`/bin/sh` ([`userspace/shell/shell.c`](../../userspace/shell/shell.c)) runs
interactively on the console (prompt `jelly:<cwd># `), runs a script
(`sh FILE`), or runs one line (`sh -c LINE`).

| Syntax | Meaning |
|---|---|
| `a \| b \| c` | Pipeline (up to 8 commands). The shell waits for all of them; the last one's exit code counts |
| `a ; b` | Command list |
| `< f`, `> f`, `>> f`, `2> f` | Redirect stdin, stdout (truncate / append), stderr |
| `'...'`, `"..."`, `\x` | Quoting; variables are expanded inside `"..."` but not inside `'...'` |
| `$NAME`, `${NAME}`, `$?` | Environment variables, exit code of the last command |
| `# ...` | Comment |

Builtins: `cd`, `pwd`, `exit`, `help`, `env`, `export`, `unset`, `status`,
`svc` (service control), `sync`, `poweroff`, `reboot`. A nonzero exit code is
shown as `[exit N]`, and unknown commands give 127. Ctrl-D on an empty line
ends the shell; the service manager then starts a new one.

### Programs (`/bin`)

`cat`, `cp`, `echo`, `false`, `ls` (`-a`, `-l`), `mkdir` (`-p`), `mv`, `rm`
(`-r`, `-f`), `sleep` (fractions allowed), `touch`, `true` in
[`userspace/applications/coreutils/`](../../userspace/applications/coreutils/).

## Power

`SYS_SYSTEM_POWER` (root only) syncs all file systems and then:

- **Power off:** reads the `\_S5_` package from the DSDT (FADT → DSDT,
  `_S5_` name followed by a package; byte, zero and one encodings) and writes
  `SLP_TYPa | SLP_EN` to `PM1a_CNT` (and `PM1b_CNT` if present).
- **Reboot:** the FADT reset register (I/O port), then port `0xCF9`, then the
  keyboard controller (`0x64` ← `0xFE`).

## Adding a program

1. Write `userspace/<area>/<name>/<name>.c` with `int main(int argc, char **argv)`.
2. Add `<name>:<install path>:<source>` to `PROGRAMS` in the Makefile.
3. `make` rebuilds the initramfs and copies it to `build/esp/boot/initrd/current.img`.
