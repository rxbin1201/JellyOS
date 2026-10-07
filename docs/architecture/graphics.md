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
| `graphics/display` | Display abstraction for the server: acquire the display, draw into a back buffer in RAM, `display_present(rect)` copies to the framebuffer and converts the pixel format |
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
```

- **Compositor:** a violet-blue gradient desktop with the JellyOS mark.
  Windows have a rounded title bar (violet when focused, grey otherwise),
  a 1-pixel border, a soft shadow and a round close button. Only damaged
  rectangles are repainted (at most 16; more are merged), bottom window
  first, the pointer last. New windows are placed in a cascade.
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
desktop; the serial console stays on the terminal.
