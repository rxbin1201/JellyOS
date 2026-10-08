# Testing JellyOS

`make test` runs, in this order:

1. the host unit tests (`make unit`)
2. the kernel self-tests in QEMU
3. the integration test in QEMU (shell, network, GUI)
4. the same system on a VirtIO GPU (frames, pointer, modes)

## Host unit tests (`make unit`)

Code without OS dependencies is also compiled with the host compiler,
AddressSanitizer and UBSan, and tested on the development machine:
[`tests/unit/canvas_test.c`](../../tests/unit/canvas_test.c) covers
`graphics/core` (rectangles, blending, fills and clipping, rounded corners,
blitting, UTF-8, text and the font).
[`tests/unit/sha256_test.c`](../../tests/unit/sha256_test.c) checks libc's
SHA-256 against the FIPS 180-4 test vectors, and the password hash of the
default account against `tools/image_builder/mkpasswd.py`.
[`tests/unit/hid_test.c`](../../tests/unit/hid_test.c) feeds
`drivers/input/hid.c` the report descriptors of a boot keyboard, a mouse,
QEMU's USB tablet and two gamepads (report IDs, hat switch, 16-bit axes) and
checks the events their reports become, plus malformed descriptors.
[`tests/unit/mixer_test.c`](../../tests/unit/mixer_test.c) covers
`audio/mixer`: sample-rate conversion up and down (a tone keeps its pitch,
chunked input gives the same result), mono to stereo, volume, mixing and
clipping.
[`tests/unit/edid_test.c`](../../tests/unit/edid_test.c) builds the EDID of
a monitor by hand, with every way of naming a mode in it, and checks what
`drivers/graphics/edid.c` reads: detailed timings first and with their own
numbers, CTA video codes, standard timings (also from a descriptor, and
the form 1366x768 takes) and established timings with every number of the
tables' timings, a mode named three times listed once, interlaced modes
and those without square pixels left out, reduced blanking for a flat
panel and the classic one for an analog monitor, and the order of the
sorted list.
[`tests/unit/hdmi_test.c`](../../tests/unit/hdmi_test.c) covers
`drivers/graphics/hdmi.c`: what the EDID of a DVI, an HDMI 1.4 and an HDMI
2.0 monitor says about its input and how fast a mode may be on each, the
bytes of the AVI info frame (checksum, RGB, range, picture shape, video
code, the codes that only an HDMI 2.0 signal may name), the talk with
a monitor's status and control registers before a scrambled signal,
against registers that exist only in the test, and the description of a
monitor's sound for an audio codec (ELD).

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

**NVMe and AHCI (Phase 12).** Two more images with the same contents are
attached as an NVMe namespace and as a second SATA disk on the machine's
AHCI controller (the first one is QEMU's boot disk).
[`tests/kernel/disk_tests.c`](../../tests/kernel/disk_tests.c) writes 150 KiB
into the unused sectors before the partition and reads them back whole and
in pieces around the page and bounce-buffer boundaries, checks that a small
write changes exactly its sectors, and reads a file from the mounted FAT32
partition of each disk. The integration test copies a file onto both disks
from the shell; the host reads it back with `mtype`.

**Intel Ethernet (Phase 12).** The integration run has an 82574L
(`-device e1000e`) as a second card on its own user network, 10.0.3.0/24.
The steps check the driver's log lines, that both cards got their DHCP
lease, ping the gateway of the second network and fetch a small and a
300 000-byte file over it. The kernel test run boots with `nodriver=e1000`:
there the same card belongs to the MSI-X test module.

**USB hubs and mass storage (Phase 12).** Both QEMU runs have a USB stick
on a root port (SuperSpeed) and one behind a hub (full speed); in the
integration run the tablet sits behind the hub as well, so every GUI step
goes through the hub driver. The kernel test `usb_storage_disks` waits for
both sticks and runs the same raw and file tests as for NVMe and AHCI. The
integration test copies a file onto both sticks (the host reads them back),
then removes the stick behind the hub through QMP while the system runs:
its volume must be unmounted and gone, the other stick must still work.
Removing a whole hub with a mounted stick and a tablet behind it, and
plugging a hub and a mouse back in, was checked by hand.

**exFAT (Phase 12).** `tools/image_builder/mkexfat.py` writes a 16 MiB exFAT
volume from the specification, independently of the driver: contiguous and
fragmented files, a root directory in scattered clusters, nested
directories, a file with a valid data length below its size, a deleted
file, and names with umlauts and with UTF-16 surrogates. It is attached as
a second VirtIO disk without a partition table (`/volumes/virtio1`).
[`tests/kernel/exfat_tests.c`](../../tests/kernel/exfat_tests.c) reads every
file and compares the contents, reads across sector and cluster borders,
lists the root directory and checks that nothing can be changed. No host
tool for exFAT is available in the build environment, so the generated
volume has not been cross-checked with another implementation.

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

### Graphics and input tests (Phase 9)

[`tests/kernel/graphics_tests.c`](../../tests/kernel/graphics_tests.c)
covers:

- `object_wait_many` (index, timeouts, auto-reset events, no observers left
  behind)
- objects in channel messages (references, rights, refused self-transfer,
  release of unread messages)
- named services (connect, the `connect` message with the server end,
  duplicate registration, disappearing servers)
- input queues (event fields, overflow drops the oldest)
- display acquisition (geometry, exclusive ownership, release)

`make test` attaches a VirtIO keyboard and tablet in both QEMU runs.

The kernel test `display_framebuffer_can_be_replaced` does what a graphics
driver does after a mode switch (Phase 12, Intel graphics): it gives display 0
another framebuffer and size, checks that the console repaints into it and
that a display server would get the new memory, and puts the real screen
back. The Intel driver itself cannot run in QEMU.

`display_driver_operations` registers a driver made for the test with
`display_set_driver()` and checks the display driver interface: which flags
appear for which operations, hardware pointer, vertical blank wait, the
second framebuffer and flipping, and that the first framebuffer is shown
again and the pointer hidden when the display server goes away. In QEMU the
display server has no vertical blank or second framebuffer, so every GUI
step of the integration test covers the software path of `display_commit()`.

The test for modes (next) also calls what a panic calls for the screen
(`display_panic_prepare()`, `display_panic_show()`) while a display server
owns the display and after the mode changed under it: the console draws
over the whole new screen and not beyond it, and the driver's `panic`
operation is called once.

`display_modes_can_be_switched` does the same for modes: a list from a
driver made for the test, switching (geometry, refresh rate, the mode
marked as current), the event for watchers, a mode that does not come up, a
switch while a display server owns the display (its mapping keeps its
size; the console lays itself out again afterwards), hot plug and a new
list of modes.

`display_screen_goes_off_and_comes_back` covers switching the screen off
with a driver made for the test: the flag and the event, that input brings
the screen back (a key going down; not the mouse in the first half second,
not a key or button going up), a driver that cannot switch off, and that a
change of the mode and a display server that goes away switch it on.

In QEMU the driver `bochs-gpu` gives the standard VGA card real mode
switching. The integration test uses it with the desktop running: the
`display` command lists the modes, a mode is chosen with the keyboard on
the Display page of Settings (1920x1080; the display server writes it to
`/etc/display.conf`), another one with `display 1024x768`, and `display 0`
goes back. After each switch a screenshot must have the new size, with the
maximized window filling it and the taskbar at the new bottom edge. Then
`display off`: the screenshot is black all over, moving the mouse switches
the screen on again, and the desktop is back.

A third boot checks the driver `virtio-gpu`: the same system with
`-vga none -device virtio-vga`, and `shell_test.py --gpu` (log:
`build/gpu-test.log`). Its sound card is a VirtIO one, the only one of the
machine, playing into `build/audio-test3.wav` (`--wav3`): `volume` names
it, a tone and two tones at once are played, and the file must hold them
afterwards, as long and as loud as written, mixed, and without a gap (a
card that ran dry between its buffers would break a tone into pieces).
Recording is only covered by the kernel test: QEMU's `wav` backend records
nothing.

The screenshots QEMU takes there are of the host's
side of the card, so they show what the driver presented:

- the driver took the card, and the display has a pointer, frame timing,
  page flipping and modes
- the login screen and, after logging in with the keyboard, the desktop
  arrive on the host
- the pointer is the card's: a screenshot is the same with the pointer on
  a spot and away from it (with the standard VGA card, where the display
  server draws the pointer, this check fails), and clicks arrive where it
  is (the launcher opens, the terminal starts and shows its window)
- modes: 1024x768, 1920x1080 (larger than the firmware's) and back; after
  each the host's picture has the new size and the taskbar is at its
  bottom edge
- no command to the card failed
- the screen off (`display off`): the host's picture is not the desktop
  any more; a key switches it on again, and the desktop is back
- a kernel panic while the desktop has the screen and the screen is
  switched off (`display off`, then `echo panic > /dev/crash`, in a mode
  other than the one the system started in): the screenshot afterwards has
  only the two colours of the kernel console, with a report of several
  lines. The panic ends this run; the script stops QEMU

Not covered by any automated test, because QEMU has no such device: the
Intel and AMD drivers' mode switching, DisplayPort link training, hot
plug and switching the screen off and on, and the sound of a monitor on AMD and Intel graphics (by hand: `volume output
N` for "Monitor sound", then `tone -f 440 -d 3000`). Of the VirtIO GPU driver, a change of the host's window is not
covered (the tests run without one).

[`tests/kernel/desktop_tests.c`](../../tests/kernel/desktop_tests.c)
(Phase 10) checks the RTC wall clock (a plausible date that advances with
the monotonic clock) and the live process count of `SYS_SYSTEM_INFO`.

### Input and audio tests (Phase 11)

[`tests/kernel/input_tests.c`](../../tests/kernel/input_tests.c):

- PS/2: scan codes and mouse packets are injected through the i8042
  controller (commands 0xD2/0xD3) and must arrive as input events: make,
  typematic repeat, break, E0-prefixed keys, movement with sign bits,
  buttons, wheel
- HID reports of a gamepad travel through the input manager as
  GAMEPAD_BUTTON / GAMEPAD_AXIS events
- USB: the xHCI driver enumerates QEMU's USB keyboard and tablet and the HID
  driver binds to both

[`tests/kernel/audio_tests.c`](../../tests/kernel/audio_tests.c):

- the device layer with a driver that exists only in the test: exclusive
  open, order of frames across the end of the ring, the low-water signal,
  underruns as silence, capture overruns dropping the oldest frames, closing
  stops the device
- every sound card in real time, which is the HD Audio driver and the
  VirtIO sound driver: playback takes frames at 48 000 per second (compared
  with the monotonic clock), stops when disabled, and recording delivers
  frames at the same rate

The kernel test run attaches VirtIO and USB input devices, and an Intel HDA
card and a VirtIO sound card with the `none` audio backend.

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

**Graphics and desktop (milestones M8 and M9).** QEMU also gets a QMP
socket. The script drives the GUI with QMP `input-send-event` (keys as
physical QEMU key codes for the German layout, absolute tablet coordinates),
follows the programs' output on the console (they print their actions) and
checks QMP screenshots:

- login: a wrong password is refused, the right one starts the session as
  uid 1000, the taskbar appears
- the launcher starts the terminal; a command typed into it creates a file
  owned by uid 1000 (checked with `ls -l` on the serial shell)
- guidemo (started from the serial shell): focus routing (title bar colors),
  typing, buttons, a checkbox switching to the dark theme, Tab and Space,
  dragging by the title bar, the close button, the focus returning
- the file manager opens the viewer on a double-click
- settings switch to the dark theme and the file manager follows; maximize
  fills the screen above the taskbar
- the gamepad viewer starts from the launcher
- `notify` shows a notification; logging out returns to the login

Window positions follow the compositor's cascade (terminal, guidemo, files,
viewer, settings in this order).

Since Phase 11 keyboard and pointer of this run are **USB devices** on an
xHCI controller (the kernel test run keeps the VirtIO devices as well), so
every GUI step also exercises xHCI, the USB core and the HID driver.

**Sound (milestone M10).** The machine has an Intel HDA card whose output
QEMU writes to `build/audio-test.wav` (`-audiodev wav`); a second codec
records silence in real time. On the shell: `svc status audio`, `volume`,
`tone` (alone; two at once, one of them mono at 22 050 Hz), `play` of the
generated chime, a tone at volume 50, a muted tone, `record` (file size and
frame count) and `play` of the recording, and a file that is not a WAV file.
After QEMU has exited, the script analyzes the WAV file in windows of 50 ms
with the Goertzel algorithm:

- a 440 Hz tone of about one second with the amplitude the program wrote
- 1000 Hz and 2500 Hz at the same time (two programs mixed), then 2500 Hz
  alone
- the chime's two notes (sample-rate and channel conversion)
- the tone at volume 50 has a quarter of the amplitude
- the muted tone does not appear

Durations are checked with tolerance: under emulation the mixer may run
late and lose a few windows.

A second HDA card that can only play (as the sound of a monitor) writes to
`build/audio-test2.wav`. With it the steps for the output device run:
`volume` lists both devices, `volume output 1` sends a tone to the second
card, a tone that is playing is moved there half way through, a device that
does not exist is refused, and a recording is made while the sound goes out
on the card that cannot record. The script then checks both files: the tone
for the second card is in its file and not in the first's, the moved tone
begins in the first and continues in the second, and nothing meant for the
first card is in the second's file.

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
| `device` | No fault at the start. Instead `/dev/crash` exists: `echo panic > /dev/crash` (or `assert`) panics a running system. Every user may write to it: the option is the permission |

## AtomBIOS interpreter

[`tests/unit/atom_test.c`](../../tests/unit/atom_test.c) (host, `make unit`)
tests [`drivers/graphics/atom.c`](../../drivers/graphics/atom.c), the
interpreter for the programs in the video BIOS of AMD graphics. There is no
such BIOS in QEMU, so the test builds a small image by hand: the headers,
command tables that use arithmetic, partial moves into registers, compare
and jump, switch, calls with parameters passed on, data tables, delays and
indirect register access, and tables that must be stopped (an unknown
instruction, an endless loop). The registers are an array.

What the real tables do on a Ryzen's GPU (switching the DisplayPort
transmitter to another link rate) can only be seen on that hardware.
