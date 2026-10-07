# JellyOS Input

**Code:** [`input/`](../../input/) (input manager), [`drivers/input/`](../../drivers/input/) (PS/2, USB HID, VirtIO), [`drivers/bus/usb/`](../../drivers/bus/usb/) (USB core, xHCI), routing in [`userspace/services/display/displayd.c`](../../userspace/services/display/displayd.c)
**ABI:** [../abi/syscalls.md](../abi/syscalls.md) · **Graphics:** [graphics.md](graphics.md)

README section 37. The input manager and the VirtIO driver came with Phase 9;
Phase 11 (milestone M10) adds PS/2, USB and gamepads.

```text
PS/2 (i8042)      USB HID (xHCI)      VirtIO input
      └──────────────┬──────────────────┘
               Input Manager            input/input.c: one queue per reader
                     │  KEY_DOWN  KEY_UP  MOUSE_MOVE  MOUSE_BUTTON  MOUSE_WHEEL
                     │  GAMEPAD_BUTTON  GAMEPAD_AXIS
                 displayd               pointer, focus, keyboard layout, key repeat
                     │
                 windows                wm_event_t (toolkit: gui_window_on_key, gui_window_on_gamepad)
```

Key codes use the evdev numbering (`<jelly/input.h>`); applications never see
the hardware.

## PS/2

`drivers/input/ps2.c` drives the i8042 controller: self-test, keyboard on
ISA IRQ 1, mouse on IRQ 12.

- **Keyboard:** scan code set 1 (translated by the controller) including the
  E0-prefixed keys. The keyboard's own typematic repeats arrive as KEY_DOWN
  with value 2.
- **Mouse:** 3-byte packets, or 4 bytes with a wheel after the IntelliMouse
  sequence; resynchronizes on the always-set bit of the first byte and drops
  packets with overflow.

## USB

```text
xHCI host controller driver ─▶ USB core ─▶ bus "usb" ─▶ class drivers (usb-hid)
```

- **xHCI** (`drivers/bus/usb/xhci.c`, PCI class 0C.03 interface 30): takes
  the controller from the firmware, sets up the command ring, the event ring
  and the device context array, and uses MSI-X or MSI. A thread per
  controller watches the root hub ports: reset, Enable Slot, Address Device,
  then the USB core takes over. Commands and control transfers block until
  their event arrives (a stalled endpoint is reset); interrupt transfers
  complete in the interrupt handler. Unplugging disables the slot and
  unbinds the class drivers.
- **USB core** (`usb.c`): reads the device and configuration descriptors,
  opens all endpoints (Configure Endpoint before SET_CONFIGURATION) and
  registers every interface as a device on the bus `usb`. Class drivers are
  ordinary drivers of the device model matching the interface class; the
  device model gained `device_unregister()` for devices that leave.
- **Not yet:** external hubs (devices must be on root ports), bulk and
  isochronous transfers (mass storage, audio), suspend. They belong to
  Phase 12.

## HID

`drivers/input/hid.c` is free of kernel dependencies and tested on the host.
It parses the **report descriptor** (global/local items, push/pop,
collections, report IDs, usage lists and ranges) into input fields and
decodes reports with it, so devices stay in report protocol and anything
they describe works:

| Application collection | Events |
| --- | --- |
| Keyboard, keypad | modifier bits and the key array → KEY_DOWN / KEY_UP (rollover reports are ignored) |
| Mouse, pointer | relative X/Y → MOUSE_MOVE; absolute X/Y (tablets) scaled to 0..65535; buttons 1-3; wheel |
| Gamepad, joystick | buttons → GAMEPAD_BUTTON (0-based); axes scaled to -32768..32767 → GAMEPAD_AXIS; hat switch → HAT_X / HAT_Y |

Gamepad axes: X/Y are the left stick. If the device has Rx and Ry they are
the right stick and Z/Rz the triggers; otherwise Z/Rz are the right stick and
Rx/Ry the triggers. The first report announces every axis; afterwards only
changes are sent.

`drivers/input/usb_hid.c` binds to USB interfaces of class 3, fetches the
report descriptor and keeps one interrupt transfer pending on the IN
endpoint. When the device is unplugged, everything still pressed is released.
Keyboard LEDs are not driven yet.

## Display server

- **Key repeat** is made by displayd for every keyboard alike (400 ms delay,
  30 per second); USB keyboards do not repeat by themselves, and the PS/2
  keyboard's own repeats are ignored.
- **Gamepad events** go to the window with the keyboard focus as
  `WM_EVENT_GAMEPAD_BUTTON` / `WM_EVENT_GAMEPAD_AXIS` (`key` = input device,
  `button` = button number or `JELLY_AXIS_*`, `x` = value). `/bin/gamepad`
  (launcher: Gamepad) shows sticks, triggers, directional pad and buttons.

## Open points

- QEMU has no gamepad device: gamepads are tested with HID reports in the
  host unit tests and the kernel tests, not with real hardware.
- No Bluetooth input (README lists it; it needs a Bluetooth stack).
- No consumer-control keys (volume keys on multimedia keyboards), no touch.
