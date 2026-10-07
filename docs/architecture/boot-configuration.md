# JellyOS Boot Configuration

**File:** `/boot/boot.cfg` on the EFI system partition (source: [`boot/bootloader/boot.cfg`](../../boot/bootloader/boot.cfg))
**Parser:** [`boot/bootloader/config.c`](../../boot/bootloader/config.c)

## Format

INI style, ASCII. Lines starting with `#` or `;` are comments. Values may be
wrapped in double quotes. Paths are absolute on the boot volume and use `/`.

```ini
[boot]
default=JellyOS
timeout=5
menu=auto
recovery=Recovery
fallback_kernel=/boot/kernels/kernel-previous.elf
max_attempts=3

[entry JellyOS]
kernel=/boot/kernels/kernel-current.elf
initrd=/boot/initrd/current.img
module=/boot/modules/early-driver.ko
cmdline="loglevel=info"
```

### `[boot]`

| Key | Default | Meaning |
|---|---|---|
| `default` | `JellyOS` | Entry booted automatically. If it is missing, the first entry is used |
| `timeout` | `5` | Countdown in seconds (0–3600). `0` boots immediately |
| `menu` | `auto` | `auto`: countdown prompt, any key opens the menu. `always`: menu with countdown. `hidden`: no prompt |
| `recovery` | `Recovery` | Entry used for recovery requests and as the last automatic fallback |
| `fallback_kernel` | `/boot/kernels/kernel-previous.elf` | Previous known-good kernel for rollback. An empty value disables rollback |
| `max_attempts` | `3` | Failed boots (1–100) before rolling back, and again before recovery |
| `resolution` | `max` | Screen mode set before the kernel starts. `max`: the monitor's native resolution (from its EDID) if the firmware offers it, otherwise the largest mode the monitor can show (at most 1920x1080 if the monitor does not identify itself). `keep`: the mode the firmware chose. `WIDTHxHEIGHT`: exactly this mode, if it exists. Only modes of the firmware's graphics driver (GOP) are available; a warning names the largest one if the native resolution is not among them |

### `[entry NAME]`

| Key | Required | Meaning |
|---|---|---|
| `kernel` | yes | Kernel ELF |
| `initrd` | no | Initramfs, handed over as module `"initrd"` |
| `module` | no, repeatable (max. 8) | Early modules, handed over in configuration order |
| `cmdline` | no | Kernel command line (max. 1023 characters) |

Up to 16 entries are supported.

### Command line flags

The boot manager derives `boot_info_t.flags` from these exact tokens:

| Token | Flag |
|---|---|
| `debug=1` | `BOOT_FLAG_DEBUG`. The boot manager also prints debug logs and diagnostics |
| `safe_mode=1` | `BOOT_FLAG_SAFE_MODE` |
| `recovery=1` | `BOOT_FLAG_RECOVERY` |

## Error handling

A broken configuration never prevents booting (README section 4):

| Problem | Behavior |
|---|---|
| File missing, unreadable or larger than 64 KiB | Built-in defaults: `JellyOS`, `Recovery`, `SafeMode` |
| Invalid line, unknown key or section, invalid value | Warning with the line number. The line is ignored and the default stays in effect |
| Entry without `kernel`, duplicate entry name | Entry ignored |
| No usable entry left | Built-in entries |
| `default` or `recovery` names a missing entry | First entry, or no automatic recovery |

When loading an entry fails (for example, a missing file or an invalid ELF),
the boot manager releases all memory of that attempt and continues
automatically: current kernel → `fallback_kernel` → recovery entry → boot menu.
Entries the user picks in the menu return to the menu on failure.

## Boot menu

The menu lists the configured entries, then `<default> (previous kernel)`
(only if `fallback_kernel` is set), `Diagnostics`, `Reboot` and
`Exit to firmware`. Keys: Up/Down, Enter, `1`–`9`, `D` for diagnostics. It
works on the graphical console and over the serial port.

| Choice | Boot mode |
|---|---|
| Default entry | `normal` (counted for rollback) |
| Previous kernel | `previous kernel` (counted for rollback) |
| Recovery entry | `recovery` |
| Any other entry | `manual` |
