# Testing JellyOS

## Kernel self-tests (`make test`)

Kernel tests live in [`tests/kernel/`](../../tests/kernel/) and are linked
into the kernel. A test is a function registered with `KTEST`:

```c
#include "tests/kernel/ktest.h"

KTEST(heap_calloc)
{
    uint32_t *values = kcalloc(1000, sizeof(uint32_t));
    KASSERT(values != NULL);     /* failure stops this test */
    KEXPECT(values[999] == 0);   /* failure is recorded, the test continues */
    kfree(values);
}
```

`make test` boots the kernel in QEMU with a separate ESP and the command line
`selftest=exit`. The kernel runs every test after initialization and reports
the result through QEMU's `isa-debug-exit` device:

| QEMU exit status | Meaning |
|---|---|
| 1 | All tests passed |
| 3 | At least one test failed |
| 124 | Timeout (the kernel hung or panicked) |

`make test KVM=1` runs the same tests on the host CPU.

On a normal boot, `selftest=1` runs the tests and keeps the system running.

### User-mode tests

[`tests/userspace/usertest.c`](../../tests/userspace/usertest.c) is a static
user program built on libos. It is embedded in the kernel
(`tests/kernel/user_images.S`) and started by
[`tests/kernel/process_tests.c`](../../tests/kernel/process_tests.c) with a
scenario number and startup handles. Exit code 0 means success; any other code
is the source line of the failed check. Fault scenarios must be killed with
`JELLY_EXIT_FAULT`.

The process tests cover ring 3 execution, isolation (faults, foreign address
spaces, bad pointers), threads with a futex mutex, FPU state across
preemption, sleep, memory limits, handle rights and generations, channels,
shared memory, and the release of all resources after exit.

### Driver tests

`make test` adds the QEMU devices `edu` (1234:11e8) and `e1000e` (8086:10d3)
and passes the modules from [`tests/drivers/`](../../tests/drivers/) as boot
modules:

| Module | Purpose |
|---|---|
| `edu.ko` | Probe verifies MMIO, an MSI interrupt and DMA in both directions; supports suspend/resume |
| `e1000e_msix.ko` | Probe verifies MSI-X delivery (link status change cause through IVAR "other") |
| `bad_api.ko`, `bad_dependency.ko`, `bad_symbol.ko` | Must be rejected (API version, missing dependency, unexported symbol) |

[`tests/kernel/driver_tests.c`](../../tests/kernel/driver_tests.c) checks
ACPI, IOAPIC routing (PIT channel 0 through ISA IRQ 0), PCI enumeration and
BARs, resource conflicts, module loading/rejection/unloading/reloading,
dependency tracking, and suspend/resume including PCI D3hot.

### Storage tests

Before every run, `make test` builds a fresh 64 MiB disk image with
`tools/image_builder/mkdisk.sh`: GPT, one FAT32 partition formatted by
`mformat`, with the files from [`tests/storage/disk/`](../../tests/storage/disk/)
plus a generated 200000-byte `pattern.bin`. The image is attached as a VirtIO
disk and appears as `/volumes/virtio0p1`.

[`tests/kernel/storage_tests.c`](../../tests/kernel/storage_tests.c) covers
GPT parsing, path normalization, ramfs (files, directories, renames, symbolic
links, permissions) and FAT32: reading files mtools wrote (long names,
case-insensitive lookup, multi-cluster), writing with holes and truncation,
directory growth, renames with `..` updates, open-file protection and
persistence across unmount/remount. User-mode file access, with and without
root rights, is tested through `usertest` (scenarios `FILES` and
`UNPRIVILEGED`).

After QEMU exits, the host reads `/jellyos/written.txt` from the image with
`mtype`. The file must contain exactly what the kernel's FAT32 driver wrote.

### Userspace tests (Phase 7)

[`tests/kernel/userspace_tests.c`](../../tests/kernel/userspace_tests.c)
covers devfs (`null`, `zero`, nodes cannot be created by name), pipes (data,
end of file without writers, `PEER_CLOSED` without readers), the initramfs
unpacker (a cpio archive built in the test, plus damaged archives), and spawn
checks (missing file, no execute permission, not an ELF file, a directory).

`spawn_runs_libc_program` writes
[`tests/userspace/spawntest.c`](../../tests/userspace/spawntest.c), a libc
program embedded in the kernel, into `/tmp` and starts it through
`process_spawn`. It gets arguments, an environment, `/dev/zero` as stdin, a
pipe as stdout/stderr and `/tmp` as its working directory. The program
reports what it received and exercises the libc: heap, `printf` formats,
number conversion, sorting, environment, C11 threads with a mutex, and
buffered file I/O with seeking.

### Network tests (Phase 8)

`make test` attaches a virtio-net card on QEMU's user network.
[`tests/kernel/net_tests.c`](../../tests/kernel/net_tests.c) covers:

- the Internet checksum (RFC 1071 example) and address parsing
- ping, UDP (ports, peek, truncation, broadcast permission, ICMP port
  unreachable, timeouts) and TCP over loopback: connect/accept, data both
  ways, half close, end of stream, connection refused
- a 1 MiB transfer between two kernel threads, whose slow reader forces the
  sender into a full window
- the DNS resolver against a fake server thread on 127.0.0.1:5353
  (case-insensitive names, cache, NXDOMAIN, numeric names, `localhost`,
  invalid names, query encoding)
- the VirtIO NIC: static configuration, ARP and ping to the user network's
  gateway 10.0.2.2, and a TCP reset from a closed host port

## Integration test: the shell (milestone M6)

After the kernel tests pass, `make test` boots a second time, normally this
time: boot manager, kernel, initramfs, init, service manager and shell, with
the test disk attached. [`tests/integration/shell_test.py`](../../tests/integration/shell_test.py)
drives QEMU's serial console. It waits for each prompt (`jelly:/path# `),
types a command and checks the output up to the next prompt:

- Builtins, programs, pipes (`a | b | c`), `<`, `>`, `>>`, `2>`, `;`, `$?`,
  variables and quoting
- Exit codes (`[exit 127]` for unknown commands)
- File tools on the ramfs (`mkdir -p`, `touch`, `mv`, `rm -r`, `ls`)
- `svc` talking to the service manager over its control channel
- Reading from and copying onto the FAT32 test disk, then `sync`
- `cat` reading the console until Ctrl-D
- `poweroff`, after which QEMU must exit by itself (ACPI S5)

Afterwards the host checks `::/motd.txt` on the test disk with `mtype`. The
console log of the run is saved to `build/shell-test.log`.

**Network (milestone M7).** The script starts an HTTP server and TCP/UDP
echo servers on the host's 127.0.0.1. JellyOS reaches them as 10.0.2.2
through QEMU's user network. The steps check:

- the DHCP lease from `networkd` (`ifconfig eth0`, retried until it appears)
- `ping` to the gateway and `nslookup`
- `http` downloads (to stdout and with `-o`), a 404 with its exit code
- `nc` over TCP and UDP (the echo servers answer in upper case, which proves
  both directions)
- "connection refused" for a closed port, and the state of the `network`
  service

Access to the real Internet is not part of `make test`, so the tests do not
depend on the host's connectivity. Check it by hand with `make run`, then
`nslookup example.com` and `http http://example.com/`.

Requirements on the host: `sgdisk` (gdisk) and `mtools`.

To confirm that a test can actually fail, break the code it covers once. For
example, disabling `FXSAVE`/`FXRSTOR` in `kernel/arch/x86_64/thread.c` makes
`preemption_preserves_fpu_state` fail.

## Fault tests

`crashtest=<kind>` on the kernel command line triggers a fault after
initialization, to check exception handling and panic output:

| Kind | Expected result |
|---|---|
| `divide` | Panic: divide error |
| `pagefault` | Panic: page fault, unmapped address |
| `invalid-opcode` | Panic: invalid opcode |
| `write-rodata` | Panic: write to read-only page |
| `stack-overflow` | Panic: double fault, kernel stack overflow (guard page hit) |
| `breakpoint` | Logged, execution continues |
| `panic` / `assert` | Panic with message / failed assertion |
