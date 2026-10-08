# JellyOS Hardware Compatibility

README section 48: every kind of device has a status, so that "works on my
PC" is not confused with hardware support.

| Status | Meaning |
| --- | --- |
| SUPPORTED | Has a driver, covered by `make test` in QEMU, and seen working on real hardware |
| PARTIALLY_SUPPORTED | Works with stated limits, or only verified in QEMU |
| EXPERIMENTAL | Driver exists, little or no testing on real devices |
| UNSUPPORTED | No driver |

"Real hardware" so far means one machine: a Lenovo ThinkCentre (Intel Core
i5-8400T, 16 GB RAM), booted from a USB stick. Entries without a note about
it have only been tested in QEMU.

## Platform

| Component | Status | Notes |
| --- | --- | --- |
| x86_64 PC with UEFI | PARTIALLY_SUPPORTED | Boots on the ThinkCentre and in QEMU/OVMF. One CPU core is used. Secure Boot must be off |
| Legacy BIOS boot | UNSUPPORTED | |
| Interrupts | PARTIALLY_SUPPORTED | PCI devices need MSI or MSI-X; legacy INTx routing (ACPI `_PRT`) is not implemented |
| Timers | PARTIALLY_SUPPORTED | LAPIC timer calibrated with the PIT, the ACPI PM timer or CPUID |
| Power | PARTIALLY_SUPPORTED | Power off and restart; no suspend, no CPU frequency control |

## Storage

| Device | Status | Notes |
| --- | --- | --- |
| VirtIO block | PARTIALLY_SUPPORTED | QEMU's virtual disk |
| NVMe SSDs | PARTIALLY_SUPPORTED | One I/O queue, 512-byte blocks only. QEMU only so far |
| SATA disks (AHCI) | PARTIALLY_SUPPORTED | 48-bit LBA disks with 512-byte sectors; no ATAPI, NCQ or hot plug. Works on the ThinkCentre |
| SATA in Intel RAID (RST) mode | EXPERIMENTAL | Treated like AHCI; untested. RAID volumes are not assembled |
| SATA in IDE mode | UNSUPPORTED | Switch the controller to AHCI in the firmware setup |
| USB mass storage (sticks, card readers, external disks) | PARTIALLY_SUPPORTED | SCSI over bulk-only transport, first unit, 512-byte blocks; no UAS. Works on the ThinkCentre |
| SD card readers | UNSUPPORTED | |

Disks built into the machine are **read-only** unless the kernel command
line contains `disks=rw` ([storage.md](../architecture/storage.md)).
File systems: FAT32 (read/write), exFAT (read-only). NTFS and ext4 volumes
are listed as block devices but not mounted. `dmesg` shows what was found.

## Input

| Device | Status | Notes |
| --- | --- | --- |
| USB keyboards and mice (HID) | SUPPORTED | On root ports of an xHCI controller. Works on the ThinkCentre, including a wireless mouse receiver |
| USB hubs | PARTIALLY_SUPPORTED | USB 2 and USB 3 hubs, stacked up to five deep, hot plug. Works on the ThinkCentre; in QEMU only a full-speed hub can be tested |
| USB gamepads (HID) | EXPERIMENTAL | Tested with synthetic reports only |
| PS/2 keyboard and mouse | PARTIALLY_SUPPORTED | QEMU only so far |
| VirtIO input | PARTIALLY_SUPPORTED | QEMU's virtual keyboard and tablet |
| USB 1/2 controllers (UHCI, OHCI, EHCI) | UNSUPPORTED | Only xHCI |
| Touchpads (I2C, PS/2 protocols beyond the basic mouse), touchscreens | UNSUPPORTED | |
| Bluetooth input | UNSUPPORTED | |

## Graphics

| Device | Status | Notes |
| --- | --- | --- |
| UEFI framebuffer (GOP) | SUPPORTED | The boot manager selects the best mode the firmware offers. Works on the ThinkCentre. Software rendering only |
| Intel GPUs, generation 9 (Skylake to Comet Lake: HD/UHD Graphics 5xx/6xx) | PARTIALLY_SUPPORTED | Mode switching to the monitor's resolution and refresh rate with `igpu=native` (the default boot entry). Works on the ThinkCentre (UHD 630): 3440x1440 at 100 Hz over DisplayPort, 50 Hz over HDMI. A mode that needs a faster DisplayPort link than the one that is up gets it (PLL and link training). HDMI monitors get HDMI with info frames (up to 300 MHz; no HDMI 2.0 on these ports). Hardware mouse pointer, vertical blank timing and page flipping (tear-free frames). The mode can be changed while running (Settings, `display`). Hot plug: a monitor that was unplugged or switched off gets its DisplayPort link trained again, another monitor its own modes, and a monitor plugged into another port (DisplayPort or HDMI) gets the picture. Other models of the generation untested; one screen at a time, no acceleration. No automated test: QEMU has no such device |
| QEMU/Bochs standard VGA (1234:1111) | PARTIALLY_SUPPORTED | Mode switching (`bochs-gpu`), tested by `make test`. An emulated card: no real hardware |
| VirtIO GPU (`virtio-vga`, 1af4:1050) | PARTIALLY_SUPPORTED | `virtio-gpu`: frames presented at 60 Hz, page flipping, the card's own pointer, mode switching up to 1920x1080; tested by `make test`. 2D only, one output. `virtio-gpu-pci` (no VGA side, so no boot framebuffer) is not taken over |
| Other Intel GPUs | UNSUPPORTED | Only through the UEFI framebuffer |
| AMD integrated graphics of Ryzen 4000/5000 G (Renoir, Lucienne, Cezanne, Barcelo: display engine DCN 2.1) | EXPERIMENTAL | `amd-gpu` with `amdgpu=native` (the default boot entry; switches to the monitor's best mode at the start): a hardware mouse pointer, vertical blank timing (by interrupt) and page flipping; on DisplayPort the monitor's modes can be switched, including those that need a faster link than the firmware trained (through the video BIOS's AtomBIOS tables). HDMI with info frames and, for HDMI 2.0 monitors, scrambled signals up to 600 MHz pixel clock (3440x1440 at 100 Hz); monitors that are not HDMI ones are driven as DVI, up to 340 MHz. Hot plug (monitor off and on, cable out and in, another monitor), and a monitor plugged into another connector (DisplayPort or HDMI) gets the picture. Works on a Ryzen 5 5600G: 3440x1440 at 100 Hz over DisplayPort. No automated test: QEMU has no such device |
| Other AMD GPUs | UNSUPPORTED | Only through the UEFI framebuffer |
| NVIDIA GPUs | UNSUPPORTED | Only through the UEFI framebuffer |
| Several monitors | UNSUPPORTED | |

## Audio

| Device | Status | Notes |
| --- | --- | --- |
| Intel HD Audio | PARTIALLY_SUPPORTED | Analog output and input at 48 kHz. Playback works on the ThinkCentre; no jack detection |
| Sound of a monitor (HDMI, DisplayPort) on AMD graphics of Ryzen 4000/5000 G | EXPERIMENTAL | The HD Audio codec of the GPU (1002:aa01) as a sound device of its own, "Monitor sound"; stereo at 48 kHz. Needs the `amd-gpu` driver (`amdgpu=native`). Works on a Ryzen 5 5600G over HDMI and over DisplayPort, also after a change of the mode |
| Sound of a monitor (HDMI, DisplayPort) on Intel graphics of generation 9 | EXPERIMENTAL | The HD Audio codec of the GPU (8086:2809, 8086:280b), on the sound card's controller, as a second sound device, "Monitor sound"; stereo at 48 kHz. Needs the `intel-gpu` driver (`igpu=native`). Works on the ThinkCentre (UHD 630) over HDMI and over DisplayPort, also after the cable was moved from one to the other. Not on NVIDIA graphics or other Intel generations |
| USB audio | UNSUPPORTED | |
| Bluetooth audio | UNSUPPORTED | |

## Network

| Device | Status | Notes |
| --- | --- | --- |
| VirtIO network | PARTIALLY_SUPPORTED | QEMU's virtual network card |
| Intel Ethernet, e1000e family (82574L, I217, I218, I219) | PARTIALLY_SUPPORTED | The chipset ports of Intel mainboards. 82574L tested in QEMU; the chipset port of the ThinkCentre works (DHCP, ping, HTTP) |
| Intel I225/I226 (2.5 Gbit), I210/I211, 10 Gbit cards | UNSUPPORTED | |
| Realtek RTL8111H / RTL8168H | PARTIALLY_SUPPORTED | Works on a desktop mainboard (revision 0x541: DHCP, ping at 1000 Mbit/s). No automated test: QEMU does not emulate the chip |
| Realtek RTL8168/RTL8111, older revisions (B ... F) | EXPERIMENTAL | Same driver, simpler start-up path; never tried |
| Other Realtek (8125 2.5 Gbit, 8139), Broadcom, Aquantia | UNSUPPORTED | |
| Wi-Fi | UNSUPPORTED | |
| Bluetooth | UNSUPPORTED | |
