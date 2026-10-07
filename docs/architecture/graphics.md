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
} display_ops_t;

status_t display_set_framebuffer(index, phys, size, width, height, pitch);   /* the driver's own memory */
status_t display_set_driver(index, ops, driver_data, second_framebuffer_phys);
status_t display_set_modes(index, modes, count, current);                    /* what the monitor can show */
void     display_set_connected(index, connected);                            /* hot plug */
```

Every operation is optional. From the ones that are there the display layer
derives the flags userspace sees (`JELLY_DISPLAY_CURSOR`, `VBLANK`, `FLIP`,
`MODES`; flipping also needs the second framebuffer, mode switching a list
of modes), checks arguments and ownership, hands out the second
framebuffer and restores the screen when a display server goes away. The
operations run one at a time under the display's lock, which a driver also
takes when it touches its hardware on its own (`display_lock()`). The
system calls 76–82, the display library and the display server only know
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

**A driver for another GPU family is one file**: find the device, set a
mode, call `display_set_framebuffer()`, fill in the operations its hardware
has, call `display_set_driver()` and `display_set_modes()`. Nothing above
the driver changes; what it leaves out is done in software as before. There
are two such drivers: [`intel_gpu.c`](../../drivers/graphics/intel_gpu.c)
(everything, below) and [`bochs_gpu.c`](../../drivers/graphics/bochs_gpu.c)
for the standard VGA card of QEMU, which has `set_mode` and nothing else —
about 150 lines, and the reason mode switching is covered by `make test`.
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

## The `display` command and the Settings page

`display` lists the displays, what their drivers offer and their modes;
`display 1920x1080`, `display 2560x1440@60` or `display 3` (a number from
the list) switches display 0 (root only). It talks to the kernel directly
and so also works on the text console; a running display server follows.
The page **Display** of the Settings program shows the same list; choosing
an entry asks the display server, which also remembers the choice.

## Intel graphics driver (Phase 12)

[`drivers/graphics/intel_gpu.c`](../../drivers/graphics/intel_gpu.c) handles the
display part of Intel's integrated graphics of generation 9 (Skylake to
Comet Lake, HD/UHD Graphics 5xx/6xx). It was ported from the previous
JellyOS implementation and exists for one purpose so far: showing the
monitor's own resolution where the firmware only offers a small mode.

Without an option it changes nothing and reports what it finds (`dmesg igpu`):
the pipe that shows the boot framebuffer, the port and kind of connection,
the timings, the DisplayPort link the firmware trained, and the detailed
timings from the monitor's EDID (read over the AUX channel for DisplayPort,
over GMBUS for HDMI).

With `igpu=native` or `igpu=WIDTHxHEIGHT[@HZ]` on the kernel command line (the
default boot entry has `igpu=native`; the entry `FirmwareGrafik` leaves the
screen as the firmware set it up) it allocates a framebuffer of that
size, enters it into the global graphics translation table and switches:

| Situation | What happens |
| --- | --- |
| The firmware already drives the monitor at that timing and scales a smaller picture up | The pipe scaler is turned off and the plane gets the full size; the pipe keeps running |
| DisplayPort, another timing | Pipe off, new timings and M/N values, pipe on; the link stays as the firmware trained it. Only modes that this link and the display clock can carry |
| HDMI, another timing | Pipe, port and PLL off, PLL reprogrammed for the pixel clock (up to 300 MHz), on again |

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
to HDMI and back while the system runs. HDMI monitors on a port brought up
this way are driven with DVI signalling (no info frames).

In both cases the EDID is read again. If it is another monitor, the display
gets its list of modes, and if the mode on the screen is not in it, the
driver switches to the new monitor's best one. The display server and the
console follow as with any mode switch.

**Link training** (`dp_link_train()`): the port sends training pattern 1
until the monitor has recovered the clock, then pattern 2 or 3 until every
lane is equalized and the lanes are aligned; after each look at the signal
the monitor asks for another voltage swing and pre-emphasis (DPCD
0x206/0x207), which go into the port's buffer control and back to the
monitor. Rate and number of lanes are the ones the firmware had chosen: the
PLL is not touched.

Not yet: changing the display clock, several screens at once, the embedded
panel of a notebook, lane reversal and other board wiring that only the
firmware's video BIOS table knows, acceleration.


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
measured refresh rate. With `amdgpu=on` (the default boot entry; the entry
`FirmwareGrafik` leaves it out) it offers:

| | |
| --- | --- |
| Hardware pointer | The cursor of the HUBP and DPP, 64x64 ARGB. The registers hold the place of the hot spot; an image that hangs over the left or top edge is shown by moving the hot spot into the image |
| Vertical blank | The timing generator's frame counter, polled every millisecond. Interrupts of this GPU arrive through a ring buffer (IH) that is not set up yet |
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

A faster link than the firmware's is not possible yet: the port's PHY is
programmed through the firmware's AtomBIOS tables on this hardware. A
3440x1440 monitor on a link trained for 60 Hz (2.7 Gbit/s per lane) cannot
be switched to 100 Hz. Also not yet: HDMI modes, hot plug, several screens,
acceleration.
