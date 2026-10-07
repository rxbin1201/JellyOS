# JellyOS Desktop

**Code:** [`userspace/desktop/`](../../userspace/desktop/) (login, desktop shell), [`userspace/applications/`](../../userspace/applications/) (files, settings, viewer, terminal, notify), window management in [`userspace/services/display/displayd.c`](../../userspace/services/display/displayd.c) and [`graphics/compositor/`](../../graphics/compositor/), toolkit in [`graphics/gui/`](../../graphics/gui/)
**ABI:** [../abi/syscalls.md](../abi/syscalls.md) (version 6) · **Graphics:** [graphics.md](graphics.md)

Phase 10 (milestone M9): JellyOS has a usable graphical desktop.

```text
displayd ──autostart──▶ /sbin/login (root)
                          │ name + password checked against /etc/passwd
                          ▼  SYS_PROCESS_SPAWN_AS (uid 1000, gid 1000), HOME, USER, cwd = home
                        /bin/desktop (the user's session)
                          ├── taskbar: launcher · window buttons · status · clock
                          ├── notifications (top right)
                          └── programs from /etc/apps/*.app: terminal, files, settings, GUI demo
                                files ──▶ viewer
```

## Login and session

- **Accounts:** `/etc/passwd` has lines `name:hash:uid:gid:home:shell`. The
  hash is `sha256$SALT$HEX`, SHA-256 iterated 10 000 times over the salt and
  the password (`password_hash()` in libc's `<sha256.h>`,
  `tools/image_builder/mkpasswd.py` on the host). `!` locks an account:
  root cannot log in graphically. The image has the account `jelly` with the
  password `jelly` and `/home/jelly` owned by uid 1000
  (`tools/image_builder/initramfs.owners`).
- **Login** (`/sbin/login`, root, started by displayd): a full-screen window
  with name and password fields. A wrong password is refused and only the
  password field is cleared. Restart and Power off are available before
  anyone logs in.
- **Session:** login starts `/bin/desktop` with `SYS_PROCESS_SPAWN_AS`
  (root only, ABI 6), the user's uid and gid, the home directory as working
  directory, and `HOME`, `USER`, `PATH` and `SHELL`. The session receives a
  control channel as startup handle 3 and sends `logout`, `reboot` or
  `poweroff` through it; only root (login) may switch the machine off.
  When the session ends, the login screen returns.
- **Logout** ends the programs the desktop started. Programs started from a
  terminal shell keep running, because there are no process groups yet.

## Desktop shell (`/bin/desktop`)

| Part | Behavior |
|---|---|
| Taskbar | A panel window (`WM_WINDOW_PANEL | WM_WINDOW_RESERVE`) at the bottom, always in the dark theme. Maximized windows leave it free |
| Launcher | The JellyOS button opens a menu with the programs from `/etc/apps/*.app` (`name=`, `exec=`, `icon=`), then Log out, Restart and Power off |
| Window buttons | One per normal window, in the order they were opened (from the window list, `WM_SUBSCRIBE_WINDOWS`). Clicking focuses or restores the window; clicking the active window minimizes it |
| System status | The network button shows the IPv4 address (or "offline"). Clicking it shows version, uptime, memory, processes, network and user (`SYS_SYSTEM_INFO`) |
| Clock | Date and time in UTC from the RTC (`SYS_CLOCK_REALTIME`), updated every second |
| Notifications | Any program sends `WM_NOTIFY` (e.g. `/bin/notify TITLE TEXT`). displayd passes it to the desktop, which shows it in the top-right corner for 6 seconds (up to 4 at once) |
| Settings | Applies the user's keyboard layout at start and when it changes |

## Window manager (displayd)

- **Kinds:** normal windows; panels (taskbar, notifications: no focus, above
  windows); popups (menus, status: above everything, focused, closed when
  they lose the focus); windows at a fixed position (login).
- **Title bar buttons:** minimize (teal), maximize (violet, resizable windows
  only) and close (pink). On hover they show their symbol and act on release.
- **Resizing:** the grip in the bottom-right corner shows an outline while
  dragging. On release the client gets `WM_EVENT_RESIZE` and asks for a new
  surface (`WM_RESIZE_WINDOW`).
- **Maximize:** the window fills the work area (screen minus reserved
  panels); maximizing again restores the previous size and place.
- **Minimize** hides the window; the taskbar or Alt+Tab brings it back.
- **Focus:** a click raises and focuses (panels excepted). When the focused
  window disappears, the topmost visible window gets the focus.
- **Settings broadcast:** `WM_SETTINGS_CHANGED` from the settings program
  becomes `WM_EVENT_SETTINGS` for every client; the toolkit reloads the
  settings and re-themes all windows that did not choose a fixed theme.

## Programs

| Program | |
|---|---|
| **Files** (`/bin/files [FOLDER]`) | Table with icons (name, size, type, folders first). Double-click or Enter opens folders, and files in the viewer. Toolbar: Up, Home, path field, New folder, Rename, Copy, Paste (recursive, with "name (2)" on conflicts), Delete (recursive, with confirmation), Refresh. Keys: Backspace up, F2 rename, F5 refresh, Delete, Ctrl+C/Ctrl+V. Works on the ramfs and the FAT volumes under `/volumes` |
| **Settings** (`/bin/settings`) | Appearance (dark mode, large text = scale 2), Keyboard (US, Deutsch; with a field to try it), Network (interfaces, addresses, counters), System (version, uptime, memory, processes, date). Saved to `~/.config/desktop.conf` (defaults in `/etc/desktop.conf`) and applied everywhere at once |
| **Terminal** (`/bin/terminal`) | Now resizable: the character grid follows the window. The prompt shows `$` for users and `#` for root |
| **Viewer** (`/bin/viewer FILE`) | Read-only text, resizable, scrolling with wheel and keys |
| **notify** (`/bin/notify TITLE [TEXT]`) | Sends a desktop notification |
| **GUI demo** | The toolkit showcase from Phase 9 |

## Toolkit additions (graphics/gui)

| Addition | |
|---|---|
| Table | Columns with titles and widths, an icon per row, selection, scrolling, Page Up/Down, double-click or Enter to activate |
| Menu | `gui_menu_show`: popup with icons and separators; mouse, arrow keys, Enter, Escape |
| Dialogs | `gui_dialog_message` (up to 3 buttons) and `gui_dialog_input` (the suggestion is selected and replaced by typing) |
| Scroll view | Vertical scrolling of a taller child, with a scroll bar |
| Icons | `graphics/core/icons.c`: folder, file, text, program, terminal, files, settings, power, restart, logout, network, info, the JellyOS jellyfish; drawn from shapes at any size |
| Others | Password fields, separators, flat and selected buttons with icons, dim and centered labels, box backgrounds, visibility and minimum sizes, `gui_box_clear` |
| Windows | Resizable, panels, popups, fixed position; keyboard shortcuts per window; destruction deferred to the end of the event |
| Application | Timers (`gui_add_timer`), the window list and notifications (desktop shell), settings change hook |

## Not yet

Process groups (logout cannot end programs started from a shell), wallpaper
and desktop icons, drag and drop, a text editor, real time zones (the clock
shows UTC), multiple users at once.
