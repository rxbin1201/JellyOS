# JellyOS Graphics, Windows and Input

**Code:** [`graphics/`](../../graphics/) (libraries), [`drivers/graphics/`](../../drivers/graphics/) (displays, framebuffer console), [`input/`](../../input/) (input manager), [`drivers/input/`](../../drivers/input/), [`userspace/services/display/`](../../userspace/services/display/) (display server), [`userspace/applications/`](../../userspace/applications/) (`guidemo`, `terminal`)
**ABI:** [../abi/syscalls.md](../abi/syscalls.md) (version 5)
**Input devices (PS/2, USB HID, gamepads) and key repeat:** [input.md](input.md)

Phase 9 (milestone M8): graphical applications run. The order follows
README section 33: UEFI GOP framebuffer, drawing, display abstraction,
surfaces, windows, compositor, input routing, GUI toolkit. There is no GPU
driver; everything is drawn in software.

```text
 application (guidemo, terminal)
   graphics/gui      toolkit: widgets, layout, themes, keyboard navigation
   graphics/window   window client library, protocol
        │  channel "display" + shared memory surfaces
        ▼
 displayd (userspace/services/display)
   graphics/compositor   windows, decorations, damage, pointer
   graphics/display      display abstraction (back buffer → framebuffer)
   keymap.c              keyboard layouts (us, de)
        │  SYS_DISPLAY_ACQUIRE           SYS_INPUT_OPEN / READ
        ▼                                 ▲
 kernel: drivers/graphics (GOP display)   input/ (input manager)
                                           ▲
                                  drivers/input/virtio_input.c
```

## Kernel

### Displays and the framebuffer console

- `display_init_boot_framebuffer()` turns the GOP framebuffer from
  `boot_info` into display 0. The framebuffer is mapped write-combining.
- The **framebuffer console** (`drivers/graphics/fb_console.c`) shows the
  kernel log and `/dev/console` on the screen with the built-in 8×16 font
  (Latin-1; UTF-8 input). On start it replays the log buffer. It keeps a
  character grid in memory and scrolls by a quarter screen, so it never
  reads the slow framebuffer. `fbconsole=0` on the kernel command line turns
  it off.
- All console output goes through `kconsole_write()`: the serial port plus
  this mirror.
- `SYS_DISPLAY_ACQUIRE` (root only) hands the framebuffer to a display
  server as a device-memory object (`shm_create_device`), which is mapped
  write-combining with `SYS_SHM_MAP`. A display has one owner at a time.
  While it is owned, the console stops drawing. When the owner closes the
  handle and unmaps, or exits, the console takes over and repaints.
- A **kernel panic** takes the screen back without asking: see "Panic"
  below.

### Display driver interface

A display is a framebuffer first; that is all the firmware gives. A graphics
driver adds to a display what its hardware can do by handing the display
layer a table of operations
([`drivers/graphics/display.h`](../../drivers/graphics/display.h)):

```c
typedef struct display_ops {
    status_t (*cursor_image)(display_t *, const uint32_t *pixels); /* 64x64, 0xAARRGGBB */
    void     (*cursor_move)(display_t *, int32_t x, int32_t y, bool visible);
    status_t (*wait_vblank)(display_t *, uint64_t timeout_ns);
    status_t (*flip)(display_t *, uint32_t buffer);                /* show framebuffer 0 or 1 */
    status_t (*set_mode)(display_t *, uint32_t mode, uint32_t *pitch); /* entry of the driver's list */
    void     (*panic)(display_t *);                                 /* framebuffer 0 on the monitor, now */
    status_t (*power)(display_t *, bool on);                        /* the screen off (standby) and on again */
} display_ops_t;

status_t display_set_framebuffer(index, phys, size, width, height, pitch);   /* the driver's own memory */
status_t display_set_driver(index, ops, driver_data, second_framebuffer_phys);
status_t display_set_modes(index, modes, count, current);                    /* what the monitor can show */
void     display_set_connected(index, connected);                            /* hot plug */
status_t display_set_power(index, on);                                       /* the screen off and on */
```

Every operation is optional. From the ones that are there the display layer
derives the flags userspace sees (`JELLY_DISPLAY_CURSOR`, `VBLANK`, `FLIP`,
`MODES`; flipping also needs the second framebuffer, mode switching a list
of modes), checks arguments and ownership, hands out the second
framebuffer and restores the screen when a display server goes away. The
operations run one at a time under the display's lock, which a driver also
takes when it touches its hardware on its own (`display_lock()`). The
system calls 76–83, the display library and the display server only know
this interface.

**Modes.** A driver that can switch modes allocates its framebuffers once,
large enough for every mode, and tells the display layer their size. A
mode switch then changes only width, height and pitch: the memory stays
where it is, and a display server keeps its mapping. `display_set_mode()`

1. takes the console off the screen if the console has it,
2. calls the driver, which shows framebuffer 0 in the new mode or puts the
   old mode back and returns an error,
3. records the new geometry, lets the console lay out its text again (at
   once, or when the display server gives the display back) and
4. signals the display's event (`SYS_DISPLAY_WATCH`), which is how a
   display server learns of it, whoever asked for the switch.

The same event is signaled when the driver gives a new list of modes
(another monitor) or reports the monitor gone or back.

**The screen off.** A driver with the `power` operation
(`JELLY_DISPLAY_POWER`) can stop the signal to the monitor, which then
goes to standby by itself; the mode, the framebuffers and what is in them
stay. `display_set_power(index, false)` (`SYS_DISPLAY_POWER`, root only)
switches off and sets `JELLY_DISPLAY_OFF`. How the screen comes back is
the display layer's business, the same for every driver and whoever owns
the display:

- **Input.** The input manager calls a hook for every event it is given.
  With a screen off, something the user does on purpose wakes a kernel
  thread that switches the screens on: a key or button going down, the
  mouse moving. A key going *up* does not (the Enter key that sent
  `display off` is released a moment later), nor the wobble of a gamepad's
  stick, and nothing at all in the first half second: the hand that clicked
  "off" is still on the mouse. A thread, because switching on takes a
  driver's time (a DisplayPort link is trained).
- A change of the mode, a display server that goes away, and a kernel
  panic switch it on, too.

While the screen is off the driver is asked for nothing else: flips,
pointer moves and waits for a frame return `BUSY`. The display library
keeps the frames it builds and shows the whole picture when the screen is
back (the display's event tells it). The display server drops the input
that arrives meanwhile, so the key that wakes the machine is not typed
into the window that happens to have the focus. A driver's hot plug
watcher leaves a port alone that is switched off.

| Driver | off | on |
| --- | --- | --- |
| `intel-gpu` | A DisplayPort monitor is told to sleep (DPCD 0x600); planes, pipe, transcoder, port and the port's PLL off (not PLL 0, which makes the display clock) | PLL on, DisplayPort link trained, the pipe with the mode it had |
| `amd-gpu` | The same for DisplayPort; stream, timing generator, and the transmitter by the video BIOS's table | DisplayPort: transmitter on, link trained; HDMI: PLL, a scrambled signal announced to the monitor anew; the pipe as it was. Only with the video BIOS's tables |
| `virtio-gpu` | The scanout shows no resource (the host's window says that the output is not active); no frames are sent | The resource on the scanout again, one frame |
| `bochs-gpu` | The VGA side of the card blanks the screen (attribute controller) | Unblanked |

Switching off after a time without input is not done yet: that is power
management (README section 39), which will use this.

**Panic.** A kernel panic must be readable on the screen also while a
desktop is on it; a frozen picture says nothing. The panic path
(`kernel/core/panic.c`) knows the display layer only as two functions
registered with `panic_set_screen()`:

1. *Before the text*, `display_panic_prepare()`: the console checks that its
   text grid fits the screen as it is now. If the mode changed while the
   display server had the screen, the grid is laid out again in the memory
   it has (a panic allocates nothing) and filled from the log. Nothing is
   drawn yet.
2. The panic is written. Every line goes to the serial port at once and,
   like all console output, into the console's grid.
3. *After the text*, `display_panic_show()`: the console draws its grid
   into framebuffer 0, and the driver's `panic` operation makes that
   framebuffer what the monitor shows, without the pointer.

The screen comes last on purpose: nothing on the way to it can cost the
report on the serial port, and a fault in there is a nested panic, which
the panic path answers by leaving the screen alone.

`panic` is the one operation that runs with interrupts off, for the last
time and perhaps in the middle of another one. It takes no locks (whoever
holds one will never run again):

| Driver | `panic` |
| --- | --- |
| `intel-gpu`, `amd-gpu` | Two register writes: the plane back to the first framebuffer, the cursor plane off. A screen that was switched off is switched on first, the same way as for input |
| `virtio-gpu` | One more frame from the first framebuffer. The thread that presents frames is gone and so are interrupts: a command that was still with the device is waited for by polling, then the frame is sent the same way; a scanout that was switched off gets its resource back first |
| `bochs-gpu` | The screen unblanked, if it was switched off; it shows the one framebuffer as it is |
| plain framebuffer | None needed |

Switching a screen on takes time: PLLs lock, a DisplayPort link is
trained. With interrupts off nothing would count that time, so the panic
path makes the clock go on without the timer interrupt
(`clock_poll_from_now()`): from then on every reading of the clock looks
at the timer's counter itself and counts a tick when it has started over,
and `thread_sleep()` waits by reading the clock. Code that waits by
sleeping or by polling against the clock therefore still works in a
panic; code that waits for a lock or an interrupt does not.

`echo panic > /dev/crash` (with `crashtest=device` on the kernel command
line, which is what makes the file exist; every user may write to it then; [`drivers/console/crash.c`](../../drivers/console/crash.c))
makes the kernel panic on request, to see this on a running system.

**A driver for another GPU family is one file**: find the device, set a
mode, call `display_set_framebuffer()`, fill in the operations its hardware
has, call `display_set_driver()` and `display_set_modes()`. Nothing above
the driver changes; what it leaves out is done in software as before. There
are four such drivers:

| Driver | Hardware | What it brings |
| --- | --- | --- |
| [`intel_gpu.c`](../../drivers/graphics/intel_gpu.c) | Intel graphics, generation 9 | Everything (below) |
| [`amd_gpu.c`](../../drivers/graphics/amd_gpu.c) | AMD graphics of Ryzen 4000/5000 G | Everything (below) |
| [`virtio_gpu.c`](../../drivers/graphics/virtio_gpu.c) | The VirtIO GPU of virtual machines | Everything (below), and covered by `make test` |
| [`bochs_gpu.c`](../../drivers/graphics/bochs_gpu.c) | The standard VGA card of QEMU | `set_mode` and nothing else: about 150 lines, the smallest example |

The kernel tests `display_driver_operations` and
`display_modes_can_be_switched` run the whole interface with drivers that
exist only in the tests.

### Input manager (README section 37)

- Drivers report events with `input_report()`, also from interrupt handlers.
- Standardized events (`jelly_input_event_t`): `KEY_DOWN` (value 1 press,
  2 repeat), `KEY_UP`, `MOUSE_MOVE` (relative, or absolute 0–65535 with
  `JELLY_INPUT_ABSOLUTE`), `MOUSE_BUTTON`, `MOUSE_WHEEL`. Key codes are in
  `<jelly/input.h>`; they follow the Linux evdev numbering of physical
  keys.
- `SYS_INPUT_OPEN` (root only) returns a queue object. Every open queue gets
  every event, holds up to 512, and drops the oldest when full. The queue is
  waitable; `SYS_INPUT_READ` takes the events out.
- **virtio-input** (`drivers/input/virtio_input.c`) supports QEMU's
  `virtio-keyboard-pci`, `virtio-mouse-pci` and `virtio-tablet-pci`. It
  translates evdev events in its MSI-X interrupt handler: keys and buttons
  directly, relative and absolute axes collected until `EV_SYN`.
- PS/2, USB HID and gamepads came in Phase 11 behind the same interface: [input.md](input.md).

### IPC additions

| Mechanism | Use |
|---|---|
| `SYS_OBJECT_WAIT_MANY` | One wait for up to 64 objects. A thread registers an *observer* on every object; `object_notify()` wakes observers as well as normal waiters |
| `SYS_CHANNEL_SEND_HANDLES` / `RECEIVE_HANDLES` | Up to 8 handles per message, **moved** with their rights (the sender loses them). Unread messages release their objects. A channel endpoint cannot be sent through its own channel |
| `SYS_SERVICE_REGISTER` / `CONNECT` | Named services. A server registers one end of a channel (root only). Each connect creates a new channel, and the server end arrives as a `connect` message with the handle. A registration whose server has gone away is replaced |

## Libraries (`graphics/`, linked into `libgraphics.a`)

| Directory | Contents |
|---|---|
| `graphics/core` | `canvas_t`: 32-bit 0xAARRGGBB pixels with a clip rectangle. Rectangles, alpha blending ("source over"), rounded rectangles with 4×4 supersampled corners, outlines, gradients, lines, blitting, 1-bit masks, UTF-8 text with the 8×16 font at integer scales. No OS dependencies (host unit tests) |
| `graphics/display` | Display abstraction for the server: acquire the display, draw into a back buffer in RAM, `display_present(rect)` names what changed, `display_commit()` shows the frame (converting the pixel format). Depending on the display's flags: a plain copy, a copy timed to the vertical blank, or a copy into the hidden framebuffer and a page flip. `display_pointer_image/move()` for a hardware pointer. `display->watch` is the display's event; after it `display_changed()` takes over a new size (a new back buffer; the framebuffers stay mapped) |
| `graphics/window` | Window protocol (`protocol.h`) and client library: connect, create windows, present, events |
| `graphics/compositor` | Window stack, decorations, damage rectangles, pointer, composition |
| `graphics/gui` | GUI toolkit |

The README's graphics API (device, context, buffer, texture, shader,
pipeline, command buffer, swapchain; section 34) belongs to GPU
acceleration and is not part of this phase. Surfaces and the present cycle
are its software forerunners.

## Window protocol

Version 2 (Phase 10) adds window kinds, resizing, minimize and maximize,
the window list for the taskbar, settings broadcasts, keyboard layouts and
notifications. See [desktop.md](desktop.md) for the window manager and the
toolkit additions.

Version 3 (Phase 12) adds `WM_SCREEN` (server: the screen has another size)
and `WM_SET_DISPLAY_MODE` (client: width, height and refresh rate of a mode
from `SYS_DISPLAY_MODES`).


Transport: a channel from `SYS_SERVICE_CONNECT("display")`. Every message is
a `wm_message_t`.

| Message | Direction | Meaning |
|---|---|---|
| `WM_HELLO` → `WM_WELCOME` | client → server | Protocol version and screen size |
| `WM_CREATE_WINDOW` → `WM_WINDOW_CREATED` | client → server | Size, flags, title. The reply carries the **surface**, a shared memory handle (width × height pixels, `stride` per row) |
| `WM_PRESENT` → `WM_PRESENTED` | client → server | The client drew a rectangle. The server composites it and answers once the frame is on the screen; only then does the client draw the next one |
| `WM_SET_TITLE`, `WM_DESTROY_WINDOW` | client → server | |
| `WM_EVENT` | server → client | Keys (with the character from the keyboard layout), pointer (content coordinates), wheel, focus, close request |

A window's surface is shared memory that both sides map. The server only
reads it while compositing, and the client only draws between `PRESENTED`
and its next `PRESENT`, so no frame is ever torn.

## Display server (`/sbin/displayd`)

Service `display` in `/etc/services.conf`, configured by `/etc/display.conf`:

```ini
keymap=de               # us | de (German QWERTZ with AltGr: @ € { [ ] } \ ~ | µ ² ³)
autostart=/bin/terminal # programs started with the server (repeatable)
mode=1920x1080@60       # screen mode, if the driver can switch; written when one is chosen
```

- **Screen modes:** the server waits on the display's event together with
  input and clients. When the size changed (chosen in Settings with
  `WM_SET_DISPLAY_MODE`, set with the `display` command, or another monitor
  was plugged in) it takes a back buffer of the new size and repaints.
  Windows without decorations keep the screen edges they were laid out
  along: the taskbar stays at the bottom and gets the new width, the login
  screen the new size. Maximized windows fill the new work area, the others
  are kept reachable. Every client gets `WM_SCREEN` with the new size
  (protocol version 3; `wm_screen_size()`, `gui_on_screen()`). A mode chosen
  through the server is written to `mode=` and set again at the next start.

- **Compositor:** a violet-blue gradient desktop with the JellyOS mark.
  Windows have a rounded title bar (violet when focused, grey otherwise),
  a 1-pixel border, a soft shadow and a round close button. Only damaged
  rectangles are repainted (at most 16; more are merged), bottom window
  first, the pointer last. With a hardware pointer the pointer is not
  painted at all, and a frame ends with `display_commit()`. New windows are placed in a cascade.
- **Input routing (README sections 35 and 37):**
  - Keys go to the focused window.
  - The pointer goes to the window under it; while a button is held, the
    window that got the press keeps the pointer (grab).
  - A press raises and focuses a window. Dragging the title bar moves it.
    The close button sends `WM_EVENT_CLOSE`. Alt+Tab brings the next window
    to the front.
  - Absolute pointers (tablet) are scaled to the screen; relative ones are
    added and clamped.
- **Waiting:** one `SYS_OBJECT_WAIT_MANY` on the input queue, the service
  channel and all clients. The server never polls.
- **Robustness:** a client that does not read its events loses them instead
  of blocking the server. When a client disconnects, its windows disappear.

## GUI toolkit (`graphics/gui`, README section 36)

| Control | Behavior |
|---|---|
| Window | `gui_window_create`, a widget tree as content, close handling, theme |
| Box layout | Vertical / horizontal, spacing, padding; `expand` children share the extra space |
| Label (text) | One line, optional large (double size) |
| Button | Normal or primary (accent color); mouse click, Enter or Space |
| Text input | One line, UTF-8, cursor, Home/End/arrows/Backspace/Delete, horizontal scrolling, placeholder; Enter submits |
| Checkbox | Toggles with click or Space |
| List | Selection with mouse, arrows, Home/End; wheel scrolling |
| Custom | Application-drawn area with its own events (the terminal) |

- **Themes:** light and dark; the JellyOS violet (`#7C3AED`) is the accent.
- **Accessibility scaling:** the theme's integer scale enlarges text and
  sizes.
- **Keyboard navigation:** Tab / Shift+Tab move the focus, which is shown as
  a ring.
- **Event loop:** `gui_run` waits on the display connection and any watched
  handles (`gui_watch`). It repaints dirty windows once the previous frame
  was presented.
- **Phase 10** added table, menu, dialogs, scroll view, icons, timers and
  resizable windows (see [desktop.md](desktop.md)). Not yet: transparency
  effects.

## Applications

| Program | |
|---|---|
| `/bin/terminal` | A shell in a window: `/bin/sh -i` on pipes, a reader thread signals an event, 80×25 with 500 lines of scrollback (mouse wheel). It edits the input line locally (no line discipline behind a pipe); Ctrl-D ends the input, closing the window ends the shell. Started by displayd |
| `/bin/guidemo` | All toolkit controls in one window. Prints every action to stdout, which the integration test follows |

The shell can start them from the console without waiting: `guidemo &`.

## QEMU

`make run` adds a VirtIO keyboard and tablet. The QEMU window shows the
desktop; the serial console stays on the terminal. QEMU's standard VGA card
gets the driver `bochs-gpu`, so the screen mode can be changed there, too.
`make run GPU=virtio` gives the machine a VirtIO GPU instead (`-vga none
-device virtio-vga`), with the driver `virtio-gpu`.

## The `display` command and the Settings page

`display` lists the displays, what their drivers offer and their modes;
`display 1920x1080`, `display 2560x1440@60` or `display 3` (a number from
the list) switches display 0 (root only). It talks to the kernel directly
and so also works on the text console; a running display server follows.
The page **Display** of the Settings program shows the same list; choosing
an entry asks the display server, which also remembers the choice.

`display off` switches the screen off, so that the monitor goes to
standby, until a key is pressed or the mouse moves (`display on` does it
from a serial console). Root asks the kernel; other users ask the display
server (`WM_SET_DISPLAY_POWER`), which is also what the button **Switch
the screen off** on the Settings page does. Both are there only where the
graphics driver can do it.

## The monitor's modes (EDID)

How a driver gets the EDID is its own business (I2C on the DDC lines, or
over the DisplayPort AUX channel); what is in it is the same for every GPU
and read by [`drivers/graphics/edid.c`](../../drivers/graphics/edid.c). A
monitor names its modes in four ways:

| In the EDID | What it says | Where the timing comes from |
| --- | --- | --- |
| Detailed timings (base block and CTA extension) | Every number: pixel clock, sizes, blanking, sync | The EDID itself. These are the modes the monitor prefers |
| CTA video codes (CTA-861 extension: televisions, HDMI monitors) | A number, such as 16 for 1920x1080 at 60 Hz | A table of that standard's timings (1280x720, 1920x1080 and 3840x2160 at 24 to 120 Hz, 640x480) |
| Standard timings (8 in the base block, 6 more in a descriptor) | Width, aspect ratio, refresh rate | VESA's list of monitor timings (DMT). Where it has two for a mode, a flat panel gets the one with reduced blanking, a monitor with an analog input the classic one |
| Established timings | One bit each for a handful of old modes (640x480 to 1280x1024) | The same list |

The detailed timings come first; of the other three kinds a mode is left
out if the list already has its size at its refresh rate, so the monitor's
own numbers win. A mode no table has is left out, too: no timing is made
up by formula. Not taken: interlaced modes, and 720x480 and 720x576, whose
pixels are not square.

The drivers keep what their connection carries (pixel clock, link) and
sort: larger first, at the same size the faster one, but every mode of 48
Hz and more before the slower ones. The first of the list is the mode
`igpu=native` and `amdgpu=native` switch to, and 3840x2160 at 30 Hz is not
what a desktop wants when 1920x1080 at 60 Hz is there. A display has room
for 32 modes.

## Intel graphics driver (Phase 12)

[`drivers/graphics/intel_gpu.c`](../../drivers/graphics/intel_gpu.c) handles the
display part of Intel's integrated graphics of generation 9 (Skylake to
Comet Lake, HD/UHD Graphics 5xx/6xx). It was ported from the previous
JellyOS implementation and exists for one purpose so far: showing the
monitor's own resolution where the firmware only offers a small mode.

Without an option it changes nothing and reports what it finds (`dmesg igpu`):
the pipe that shows the boot framebuffer, the port and kind of connection,
the timings, the DisplayPort link the firmware trained, and the modes
from the monitor's EDID (read over the AUX channel for DisplayPort, over
GMBUS for HDMI).

With `igpu=native` or `igpu=WIDTHxHEIGHT[@HZ]` on the kernel command line (the
default boot entry has `igpu=native`; the entry `FirmwareGrafik` leaves the
screen as the firmware set it up) it allocates a framebuffer of that
size, enters it into the global graphics translation table and switches:

| Situation | What happens |
| --- | --- |
| The firmware already drives the monitor at that timing and scales a smaller picture up | The pipe scaler is turned off and the plane gets the full size; the pipe keeps running |
| DisplayPort, another timing | Pipe off, new timings and M/N values, pipe on. A mode that needs a faster link than the one that is up gets one first (below), if the monitor takes it; the display clock bounds every mode |
| HDMI, another timing | Pipe, port and PLL off, PLL reprogrammed for the pixel clock (up to 300 MHz), on again. A monitor whose EDID calls it an HDMI one is driven as HDMI, with the AVI info frame of the mode in the transcoder's packet memory ([`hdmi.c`](../../drivers/graphics/hdmi.c), as in the AMD driver); others as DVI. `igpuhdmi=off` keeps every monitor at DVI. The scrambled signal of HDMI 2.0 is beyond the ports of this generation |

If the pipe does not come up, the firmware's mode is restored. On success
the driver calls `display_set_framebuffer()`: display 0 gets the new memory
and size, the kernel console lays its text grid out again, and the display
server later acquires the new framebuffer like the old one.

`igpu=native` takes the largest mode the monitor names and, at that size,
the highest refresh rate the connection carries. Two modes count as the same
only if size, totals and pixel clock agree.

QEMU has no such device; the driver runs on real hardware only. On a Core
i5-8400T (UHD Graphics 630) with a 3440x1440 monitor it switches to 100 Hz
over DisplayPort (a real mode switch on the firmware's link) and stays at the
firmware's 50 Hz over HDMI, where 300 MHz pixel clock is the limit and only
the scaler is turned off.

After a mode switch the driver also offers the three things of the display
driver interface (above):

- **Hardware pointer:** the cursor plane of the pipe, 64×64 ARGB, from memory
  entered into the graphics translation table. Moving the mouse writes two
  registers and repaints nothing.
- **Vertical blank:** the pipe's vertical blank interrupt (MSI) counts frames
  and wakes waiters. The driver checks at start that interrupts really
  arrive; if not, it offers neither this nor page flipping.
- **Page flipping:** two framebuffers side by side in the translation table.
  A flip writes the plane's surface address, which the hardware takes over
  at the next vertical blank; the wait ends when the live surface register
  shows the new one.

The display server then draws each frame into the framebuffer that is not
shown and swaps: no tearing and no half-drawn windows, at the refresh rate
of the monitor. `dmesg igpu`, `dmesg displayd` and the `display` command say what is in use.

**Modes while running.** The driver gives the display layer the list of the
monitor's modes that the connection can carry, the largest and fastest
first, and switches between them with the same steps as at the start (pipe
off, timings, M/N values or PLL, pipe on). Its two framebuffers are
allocated once for the largest mode (with more than 1 GiB of free memory
for 3840x2160, so that a larger monitor plugged in later fits) and stay in
place. If a mode does not come up, the mode before is put back.

**Hot plug.** A kernel thread looks at the connection once a second:

| Connection | Monitor there? | What happens when it comes back |
| --- | --- | --- |
| DisplayPort | It answers on the AUX channel (the read is its link status) | The link is trained again: a monitor that was unplugged or switched off has lost it, and the picture stays black without. The same happens when the monitor reports the link lost while staying connected |
| HDMI | The port's hot plug pin (live state in the PCH) | Nothing is needed for the same monitor |

While no monitor is on the port in use, the hot plug pins of the other
ports (B, C, D) are looked at. A monitor there gets the picture: the old
port is switched off completely, and the new one is brought up from
nothing: its power well, the table of signal levels, a PLL (for HDMI at the
pixel clock, for DisplayPort at the best link rate the monitor takes, with
slower ones tried if training fails), for DisplayPort a trained link. Then
the monitor's best mode is set. So the cable can be moved from DisplayPort
to HDMI and back while the system runs.

In both cases the EDID is read again. If it is another monitor, the display
gets its list of modes, and if the mode on the screen is not in it, the
driver switches to the new monitor's best one. The display server and the
console follow as with any mode switch.

**Link training** is the shared code of [`dp_aux.c`](../../drivers/graphics/dp_aux.c)
(`dp_link_train()`, also used by the AMD driver): the monitor recovers the
clock from training pattern 1, then every lane is equalized and the lanes
are aligned with pattern 2 or 3; after each look at the signal the monitor
asks for another voltage swing and pre-emphasis. The driver brings the
port's side as two functions: which pattern the port sends (`DP_TP_CTL`),
and the lanes' level as an entry of the port's table (`DDI_BUF_CTL`).

**A faster link.** The firmware trains the link as fast as its own mode
needs. The list of modes goes by the fastest link the monitor takes (DPCD
0x001, at most 5.4 Gbit/s per lane); a mode beyond what the link carries
as it is makes the driver switch the port and its clock off, set the
port's PLL to the next rate that is enough, train the link there and
compute the mode's M and N for it. PLL 0 also makes the display clock and
is never reprogrammed: a port the firmware ran from it moves to PLL 1. If
training fails, the old rate and the old mode come back. A monitor plugged
in later that takes less than the link has gets the link at its own best
rate.

`igpulink=162000` (or `270000`) on the kernel command line makes the link
slower than the firmware trained it, right after the driver took the
screen, if the mode on the screen fits. It is for trying the above on a
machine whose firmware always trains the fastest link.

**Sound.** A monitor whose EDID says it plays sound (basic audio) gets it
in the signal, on HDMI (not on a port driven as DVI) and on DisplayPort.
The samples come from the HD Audio codec inside the GPU (see
[audio.md](audio.md#the-sound-of-a-monitor)); the display engine has the
other end of it, a set of registers per transcoder (after i915's
`hsw_audio_codec_enable()`):

| | |
| --- | --- |
| Sound on | A bit per transcoder. With it the codec's pin for the port reports "something plugged in", which is how the audio driver finds the pin to play to |
| The description | What the monitor is, in the form HD Audio codecs want (*ELD*): written word by word into the codec's memory for it, then marked valid. [`hdmi.c`](../../drivers/graphics/hdmi.c) makes it for every driver: manufacturer and product from the EDID, HDMI or DisplayPort, two channels of PCM at 32, 44.1 and 48 kHz |
| The clock | HDMI: the hardware picks N by the pixel clock and measures CTS; only at 297 MHz the N the standard wants is given. DisplayPort: M and N for 48 kHz are given, 512 * 48000 to the link rate |

It is switched on whenever the pipe starts and off before it stops. A
firmware that drives an HDMI monitor as DVI at the very mode that is
wanted would leave nothing to switch: then the port is started again once,
as HDMI. `igpusound=off` leaves the sound out.

Not yet: changing the display clock, several screens at once, the embedded
panel of a notebook, lane reversal and other board wiring that only the
firmware's video BIOS table knows, more than two channels of sound,
acceleration.

### The engines (the GT)

A GPU of this kind is two machines. The display engine, above, puts a
picture on a monitor. The other one executes commands, and it is what
can draw the picture: Intel calls it the GT.
[`drivers/graphics/intel_gt.c`](../../drivers/graphics/intel_gt.c) drives
it (after i915's `gt/` directory): it is woken, two of its engines are
brought up, commands can be given to them and waited for, memory can be
entered into the address space they draw in, and the blitter fills and
copies rectangles. **Nothing draws with it yet.** Hardware acceleration
is the last step of the README's order for graphics and a priority of
Phase 14; this is what it will stand on.
The display driver starts it once the screen is its own and lends it the
registers, room in the graphics address space and its interrupt;
`igpugt=off` leaves it out.

| | |
| --- | --- |
| Awake | The GT sleeps whenever nobody uses it, and its registers are then not there. A driver holds it awake by setting a bit per power domain ("forcewake") and waiting for the acknowledgement. Set once and kept: no power saving of the GT yet |
| Engines | Each executes one kind of commands from a ring buffer: the **render** engine (3D; the only one that can blend) and the **blitter** (copies and fills rectangles). Each is reset first, since the firmware never used it |
| Memory | The GPU reads everything through translation tables. The global one (GGTT) is the display driver's: the framebuffers are in its second quarter, the engines' status pages, contexts and rings in its upper half. What the engines draw in and read from is in an address space of the contexts' own (PPGTT), four levels of tables like the CPU's, kept by `intel_gt_map()` and `intel_gt_unmap()`. Memory is entered in pieces of 2 MiB of address space, so each piece has its last-level table to itself and the table goes when the piece goes; whatever is not entered leads to one scratch page. Memory a display reads is entered as "not cached", so that what the GPU writes is in memory when the display engine fetches it |
| Contexts | An engine runs a *context*: its ring and its complete register state, in an image that the hardware loads and saves (2 pages for the blitter, 22 for the render engine). The image begins with register writes at fixed places. The first load is told to leave the engine's state as it is; the save that follows fills the image with the hardware's own values, which later loads restore |
| Submission | A context is handed to an engine by writing its descriptor to the engine's submit port ("execlists"). The engine runs it until its ring is empty, switches it out and says so in a small buffer of status events. Only then is the image the driver's again: the next commands go into the ring, the new tail into the image, the descriptor to the port. The tables are read anew with every load, so what was entered into the address space since is there |
| Batches | Commands that draw are not in the ring but in a buffer of their own in the contexts' address space; the ring only says "go there". Commands in the ring are privileged, those in a batch are not |
| Done | Every submission ends with a command that writes a sequence number into the engine's status page, and with an interrupt. A caller sleeps until the number is there and the context is switched out. The interrupt is the display driver's MSI, whose handler passes on what the master register names as the engines'. If none arrives during the tries at the start, the engines are looked at every two milliseconds instead |
| Rectangles | The blitter's two commands: fill with a colour, copy from a source, for pixels of 32 bits (`intel_gt_blit()`). A submission of the blitter ends with "flush, then store", so that the pixels are in memory when the sequence number is |

At its start each engine is tried out: about 700 submissions, among them a
command that stores a value, once around the whole ring, and a batch. Then
the blitter is tried on the second framebuffer, which nothing shows yet: a
pattern written by the CPU, a rectangle filled and a piece of the pattern
copied by the blitter, and every pixel compared with what should be there,
the untouched ones around them included. Then its speed is measured:
filling, copying from a framebuffer and from ordinary memory. The result
is in the log (`dmesg igpu`); an engine that does not get through is
reported with its registers and not used, and the display goes on as
before.

| | |
| --- | --- |
| Clock | The GT's clock can run at a range of rates, and the firmware leaves it at the slowest (100 MHz on the ThinkCentre, where 350 to 1050 MHz are possible). The highest rate is asked for once (as the old JellyOS driver did); no scaling with the load yet |
| Speed | On the ThinkCentre (UHD 630, 3440x1440) at 1050 MHz: filling 11 GB/s, copying 5 GB/s from a framebuffer and 4 GB/s from ordinary memory, a copy of the whole screen in 4 ms. At the firmware's 100 MHz it was a ninth of that |

**Why the blitter does not draw for the desktop.** It was tried: with page
flipping, the rectangles the frame before changed were copied by the
blitter from the framebuffer on the screen to the hidden one, in place of
the CPU's copy from the back buffer. At the firmware's clock moving a
large window was no longer smooth (a copy of half the screen took longer
than a frame), and at the full clock the blitter is about as fast as the
CPU, which the display server would then only wait for. The gain is for
the render engine, which can put the windows together, shadows and
transparency included; the old JellyOS composited its desktop that way,
about twice as fast as with the CPU on the same machine.

### Copying and blending on the render engine

The first part of that, ported from the old JellyOS (`igd_rcs.c`,
`igd_comp.c`): the render engine copies rectangles and blends them with
their alpha, times an opacity, over a destination. Nothing uses it for the
desktop yet.

Not the 3D pipeline but the GPGPU one. The render engine starts a hardware
thread on the execution units (EUs) for each block of 8 x 8 pixels, and
each runs a small program that reads its block of the source (and for
blending of the destination) with "media block read" messages, computes,
and writes the block back. The hardware does not cut a block at the edge
of a surface, so a rectangle is cut into up to four pieces whose blocks
fit exactly: 8 x 8 inside, 1 x 8 on the right, 8 x 1 at the bottom, 1 x 1
in the corner.

| | |
| --- | --- |
| Programs | [`intel_kernels.c`](../../drivers/graphics/intel_kernels.c): a small assembler for the EUs' instruction format (generation 8 and 9, 128 bits per instruction) and the eight programs, copy and blend for each block shape. Blending per byte: (source x a + destination x (255 - a)) / 255, rounded, where a = the source's alpha x opacity / 256. No hardware in this file: the unit test checks that the assembler makes the fill program of Intel's test suite (IGT) bit for bit, that the pieces cover a rectangle exactly once, and the formula |
| A batch | [`intel_render.c`](../../drivers/graphics/intel_render.c): `PIPELINE_SELECT` (GPGPU), `STATE_BASE_ADDRESS`, `MEDIA_VFE_STATE`, then per piece its constants (the corners in both surfaces), its interface descriptor (program, binding table with the two surface states), `GPGPU_WALKER` with one thread per block, and a `PIPE_CONTROL` that waits for every thread and flushes the data cache, so that the next piece sees what this one wrote. Up to 32 rectangles per batch |
| Surfaces | Seen as 8-bit surfaces (x counts bytes), up to 4096 pixels wide. One that a display reads is written past the GPU's caches, by its entry in the render engine's caching table (which the driver sets as the old JellyOS did); others go through the cache the CPU shares |
| Fresh | Before every batch a `PIPE_CONTROL` in the ring makes the engine forget its translations and its caches of states, constants and programs (as i915 does); the blitter's batches get the same from `MI_FLUSH_DW` |

At its start it is tried on surfaces of its own: an odd rectangle copied
(all four shapes), two blends that overlap, the second at half opacity,
and a copy into a surface written past the caches; every pixel is compared
with the CPU's result. Then on the framebuffers: the whole screen copied,
1024 x 1024 pixels of ordinary memory blended over it, a part compared
with the CPU, and the time of both in the log. On the ThinkCentre at
1050 MHz: the whole screen (3440x1440) in 4.6 ms, blending at 4 GB/s.


## AMD graphics driver (Phase 12)

[`drivers/graphics/amd_gpu.c`](../../drivers/graphics/amd_gpu.c) is the third
driver behind the display driver interface: the display part of AMD's
integrated graphics with the display engine DCN 2.1 (Ryzen 4000 and 5000
processors with Radeon Graphics, for example the Ryzen 5 5600G).

One screen is driven by a chain of blocks, each of which exists several
times: a HUBP reads the framebuffer from memory, the DPP of the same number
converts it and mixes the cursor in, and a timing generator (OTG) makes
sync and blanking. The driver finds the HUBP whose surface address is the
boot framebuffer and, through it, the timing generator.

Without an option it only reports what it finds (`dmesg amdgpu`): the video
memory, the planes, the timing registers, the kind of connection and the
measured refresh rate. With `amdgpu=native` (the default boot entry; the
entry `FirmwareGrafik` leaves it out) it takes the display over and switches
to the monitor's best mode at once; `amdgpu=on` does the same but keeps the
firmware's mode until another one is chosen. It offers:

| | |
| --- | --- |
| Hardware pointer | The cursor of the HUBP and DPP, 64x64 ARGB. The registers hold the place of the hot spot; an image that hangs over the left or top edge is shown by moving the hot spot into the image |
| Vertical blank | An interrupt of the timing generator ("a frame starts"), see below; if it does not arrive, the frame counter is polled every millisecond |
| Page flipping | The HUBP's surface address, which the hardware takes over at the next frame; the wait ends when the "flip pending" bit is gone |

The second framebuffer and the pointer image lie in the GPU's video memory
behind the firmware's framebuffer; the CPU reaches them through BAR 0, the
GPU through its own address of that memory (`DCN_VM_FB_LOCATION_BASE`).

Over the AUX channel the driver reads what the monitor can do, which link
the firmware trained, and the EDID. The AUX protocol (retries, DPCD, I2C
for the EDID) and the EDID's timings are code shared by the drivers
([`dp_aux.c`](../../drivers/graphics/dp_aux.c),
[`edid.c`](../../drivers/graphics/edid.c)); a driver brings one function
that sends a single AUX message with its hardware.

**Switching modes** works on DisplayPort, for the monitor's modes that the
link carries as the firmware trained it:

1. the video stream to the monitor, the HUBP and the timing generator are
   stopped;
2. the new timing goes into the timing generator, the pixel clock into the
   DisplayPort DTO (its phase register is the clock in Hz), the picture size
   into plane, scaler and output, and the timing the monitor is told (MSA)
   into the encoder;
3. the parameters that tell the HUBP when to fetch data are scaled from the
   firmware's values: times in reference clock cycles with the time a line
   takes, the clock ratio with the pixel clock. Linux computes them with its
   display mode library; the scaled values are an approximation;
4. everything is started again. If fewer than three frames come in a
   quarter of a second, or the pipe reports that it ran out of data, all
   registers are written back.

The framebuffers stay in place with their line length; a smaller mode shows
their top left part.

**A faster link.** The firmware trains the link only as fast as its own
mode needs: for 3440x1440 at 60 Hz that is 2.7 Gbit/s per lane, and 100 Hz
needs 5.4. On this hardware the port's transmitter (PHY) is not programmed
through registers a driver is told about, but by a program in the video
BIOS: an *AtomBIOS command table*, byte code that reads and writes
registers. So the driver

1. takes the video BIOS from the ACPI table `VFCT` (an integrated GPU has
   no ROM of its own);
2. runs its table `DIG1TransmitterControl` with the interpreter in
   [`atom.c`](../../drivers/graphics/atom.c): transmitter off, on at the new
   rate, and during training once per change of signal levels;
3. raises the display clock, which must be at least the pixel clock, with a
   message to the system management unit;
4. trains the link. Training itself is shared code
   ([`dp_aux.c`](../../drivers/graphics/dp_aux.c), `dp_link_train()`): what
   is said to the monitor is the same for every GPU, and a driver brings two
   functions, "send training pattern n" and "drive the lanes at these
   levels".

If the new rate cannot be trained, the old one is brought back and the mode
stays.

The interpreter knows nothing about the hardware (registers are reached
through functions of the driver), which is what makes it testable: a unit
test runs it on a small image built by hand
([`tests/unit/atom_test.c`](../../tests/unit/atom_test.c)).

**Interrupts.** The GPU has one interrupt for everything. Whatever happens
in one of its blocks is written as an entry of 32 bytes (who, what, when)
into a ring by the *interrupt handler* block (IH), which then raises the
GPU's MSI; the driver reads the entries up to the write pointer and writes
back how far it got, which arms the interrupt again. The ring lies in main
memory (the IH is told that its address is a bus address), so none of the
GPU's own memory management is needed for it. After Linux's `vega10_ih.c`.

The driver asks the timing generator for two events: *VSTARTUP*, the start
of a new frame inside the vertical blank, which is what `wait_vblank`
waits for, and *VUPDATE*, the moment the double buffered registers take
their new values, which is when a flip has happened. A wait after a flip
is woken by the first, finds the flip still pending and is woken again by
the second a few lines later.

Nothing of this can be tested without the hardware, so the driver checks
it itself: after switching the interrupts on it counts them for a quarter
of a second against the frame counter. If none or far too many arrive, or
if later three waits in a row time out while frames go by, everything is
switched off again and frames are timed by polling as before; `dmesg
amdgpu` shows the ring's registers in that case. An interrupt that cannot
be quietened (more than 20000 a second) is switched off in the handler.
`amdgpu=native,noirq` leaves the interrupts out.

**Hot plug.** As in the Intel driver, a thread reads the monitor's link
status over the AUX channel once a second. No answer: the monitor is gone.
When it answers again, or reports the link lost while staying connected (it
was switched off and on), the link is trained again, at the rate it had or
the fastest the monitor takes. The EDID is read again; another monitor gets
its own list of modes and, if the mode on the screen is not among them, its
best one.

**HDMI.** Three things differ from DisplayPort:

| | DisplayPort | HDMI, DVI |
| --- | --- | --- |
| Pixel clock | The DTO: a register holds it in Hz | The PLL of the port's PHY, set by the video BIOS's table `SetPixelClock`; the clock on the screen is found by matching the timing with the monitor's EDID |
| EDID | I2C over the AUX channel | The DDC line, with the hardware I2C engine |
| Monitor there? | It answers on the AUX channel | The hot plug pin |

A mode switch on HDMI stops the timing generator, then the transmitter,
sets the PLL, writes the timing and starts both again.

The firmware drives an HDMI connector like a DVI one: pixels and nothing
else. The driver does the same for a monitor whose EDID does not call it
an HDMI one. For an HDMI monitor it sends HDMI proper, with the parts that
are the same for every GPU in [`hdmi.c`](../../drivers/graphics/hdmi.c):

| | |
| --- | --- |
| The sink | What the CTA extension of the EDID says about the monitor's input: the HDMI vendor block (it understands packets), the HDMI Forum's block (HDMI 2.0: how fast a signal, and that it has status and control registers, *SCDC*), whether it follows the colour range a source names |
| Packets | Between the pixels: general control and null packets, and in every frame the **AVI info frame**, which says what the pixels are: RGB, full range, made by a computer, the picture's shape, and which of CTA-861's timings this is, if any. It goes into the memory of the encoder's generic packet 0 |
| HDMI 2.0 | Above 340 MHz the data is scrambled and the clock lane runs at a quarter of its rate; the PHY makes signals up to 600 MHz. Before such a signal starts the monitor is told so in its SCDC registers, over the DDC line (I2C address 0x54), and when the signal goes back to a plain one. With this a mode like 3440x1440 at 100 Hz (536 MHz) works over HDMI |
| Did it arrive? | A fast signal that the cable or the monitor does not carry gives a black screen while the pipe runs perfectly. So after the start the driver asks the monitor, for half a second, whether it found the clock and locked onto the three data lanes (SCDC status); if it says no, the mode counts as failed and the one before comes back |

**Sound.** A monitor whose EDID says it plays sound (basic audio) gets it
in the signal, on HDMI and on DisplayPort. The samples come from the HD
Audio codec inside the GPU (see [audio.md](audio.md#the-sound-of-a-monitor));
the display engine has the other end of that codec, *audio endpoints*, and
the encoders take their samples from one of them. Three parts, after
Linux's `dce_audio.c` and `dcn10_stream_encoder.c`:

| | |
| --- | --- |
| The endpoint | Endpoint 0, which is the codec's first pin. It is told what the monitor takes (two channels of PCM at 32, 44.1 and 48 kHz, 16 bits; HDMI or DisplayPort) and that a monitor is there |
| The clock | 24 MHz for the sound, made by a DTO from the pixel clock (HDMI) or from DisplayPort's reference clock. Which DTO and its source are set before the numbers: the other order gives no sound |
| The encoder | Takes the endpoint's samples and sends them between the pixels, with what a monitor needs to play them: on HDMI clock regeneration packets (N as the standard has it for "any other pixel clock"; the hardware measures CTS) and the audio info frame; on DisplayPort time stamps and the info frame |

One more thing has to be right, and it cost the most time: **the memories
of the encoders' HDMI parts**. A firmware that shows its picture as DVI
has no use for them and leaves them held off (`DIO_MEM_PWR_CTRL`). Then
everything about the sound looks right from every side: the codec has the
stream, the endpoint counts its samples, the encoder reports sound and
sends clock packets with the right numbers. The monitor stays silent. The
driver releases them at its start, as Linux's `dcn10_init_hw()` does.

`amdgpu=native,nosound` leaves the sound out. The state of the sound
(the codec's stream at the endpoint, whether the encoder plays) goes to
the log at debug level whenever it changes (`dmesg amdgpu`).

The video BIOS's tables are told "HDMI" as the kind of signal, and its
table for an encoder's stream side is run with each mode (as Linux does).
A monitor that was switched off or unplugged forgets what it was told:
when it is back, a scrambled signal is started again from the SCDC write
on. `amdgpu=native,dvi` keeps an HDMI monitor at DVI signalling, up to 340
MHz, as before.

**Other connectors.** The video BIOS lists the board's connectors (kind,
encoder, DDC line, hot plug pin); on this hardware connector *n* uses PHY,
encoder, AUX channel or DDC line and hot plug pin *n*. While no monitor is
on the connector in use, the hot plug pins of the others are looked at. A
monitor there gets the picture:

1. The pads of the new connector are set to what it is (AUX channel or I2C
   line; the firmware does that only for the connector it lights), and the
   monitor's capabilities and EDID are read.
2. Stream and timing generator stop, and the video BIOS's table switches
   the old transmitter off.
3. The new encoder gets the connector's kind of signal and hot plug pin,
   the timing generator its source of the pixel clock (DTO or PHY).
4. DisplayPort: the transmitter is switched on and the link trained, at
   the monitor's best rate. HDMI: the transmitter goes on with the mode.
5. The monitor's best mode is set.

So the cable can be moved between DisplayPort and HDMI while the system
runs. Two rules came out of making this work on real hardware:

- **The video BIOS owns the encoder's "on" state.** Its transmitter table
  marks the encoder's back end as on and connects the front end to it when
  it switches a transmitter on, and undoes both when it switches it off. A
  driver that sets the mark itself gets a table that returns at once ("on
  already") without touching the PHY.
- **The front end of an encoder is clocked by its PHY.** Reading one that
  is connected to a PHY that does not run does not return nonsense, it
  stops the register bus: from then on every register of the display engine
  reads as all ones. So the driver asks the PHY for its power state first
  (`transmitter_runs()`) and leaves the front end alone otherwise.

`amdgpu=on,noflip`, `nopointer`, `novblank`, `noirq`, `dvi`, `nosound` leave single
parts of the driver out (for finding the cause of a problem). `amdgpu=native,trace` logs every
register access of the video BIOS's tables (reads that repeat while a table
waits are counted); together with `logfile=` (the boot entry `JellyOSLog`)
this shows on another computer what a table did before the screen went
dark.

Not yet: several screens at once, more than two channels of sound, more
than 8 bits per colour, acceleration.

## VirtIO GPU driver (Phase 12)

[`drivers/graphics/virtio_gpu.c`](../../drivers/graphics/virtio_gpu.c)
drives the paravirtual graphics card of QEMU and other virtual machines
(`virtio-vga`, PCI 1af4:1050), in its 2D mode. It is the one graphics driver
whose every operation runs in `make test`.

The card differs from real ones in one thing: **it has no framebuffer the
host looks at.** The picture lives in a *resource* on the host. The guest
keeps the pixels in its own memory (the resource's *backing*) and says when
to copy them over and when to show them. Commands and their answers travel
over a virtqueue ([`drivers/bus/virtio`](../../drivers/bus/virtio/virtio.h),
shared with the block, network and input drivers).

| Display interface | What the driver does |
| --- | --- |
| Framebuffers | Two, in guest memory (enough for 1920x1080 each), both the backing of one resource: the second lies behind the first |
| Frames | A thread copies the framebuffer on the screen to the host and shows it, 60 times a second (`TRANSFER_TO_HOST_2D`, `RESOURCE_FLUSH`): the card's "vertical blank" |
| `flip` | The other framebuffer is copied from the next frame on (the same resource, read from another offset of its backing). The host only ever sees whole frames |
| `wait_vblank` | Until the next frame has been presented; after a flip, the frame that shows the new framebuffer |
| Pointer | A second resource of 64x64 pixels, placed by the host through the cursor queue (`UPDATE_CURSOR`, `MOVE_CURSOR`). It is not part of the frame |
| `set_mode` | A resource of the new size with the same backing, put on the scanout; the old one is given back. The framebuffers do not move |
| Modes | The size the host prefers (its window) and common sizes that fit the framebuffers. The card itself takes any size |

**Taking over.** As `virtio-vga` the card is also a VGA card: the firmware
shows its picture through that side, and the boot framebuffer is the VGA
memory. The driver creates its resources, moves the display (and with it
the kernel console) into its own framebuffer, and puts the resource on
scanout 0; from that command on the host shows the VirtIO side.

**The host's window.** When it gets another size, or the output goes away
or comes back, the device sets an event in its configuration. The frame
thread looks at it once a second, asks for the output's state and passes it
on as for another monitor: a new list of modes (the window's size is the
preferred one) or "monitor disconnected". The mode on the screen stays.

Limits:

- No 3D (virgl), one scanout, and no card without the VGA side
  (`virtio-gpu-pci`): there the firmware gives no boot framebuffer to take
  over.
- A change of the host's window cannot be made in a test without a window;
  that path is not covered by `make test`.
