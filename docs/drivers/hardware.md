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
| Intel GPUs | UNSUPPORTED | Only through the UEFI framebuffer. Phase 12 |
| AMD GPUs | UNSUPPORTED | Only through the UEFI framebuffer |
| NVIDIA GPUs | UNSUPPORTED | Only through the UEFI framebuffer |
| Several monitors | UNSUPPORTED | |

## Audio

| Device | Status | Notes |
| --- | --- | --- |
| Intel HD Audio | PARTIALLY_SUPPORTED | Analog output and input at 48 kHz. Playback works on the ThinkCentre; no jack detection, no HDMI/DisplayPort audio |
| USB audio | UNSUPPORTED | |
| Bluetooth audio | UNSUPPORTED | |

## Network

| Device | Status | Notes |
| --- | --- | --- |
| VirtIO network | PARTIALLY_SUPPORTED | QEMU's virtual network card |
| Intel Ethernet, e1000e family (82574L, I217, I218, I219) | PARTIALLY_SUPPORTED | The chipset ports of Intel mainboards. 82574L tested in QEMU; the chipset port of the ThinkCentre works (DHCP, ping, HTTP) |
| Intel I225/I226 (2.5 Gbit), I210/I211, 10 Gbit cards | UNSUPPORTED | |
| Realtek, Broadcom, Aquantia and other Ethernet | UNSUPPORTED | |
| Wi-Fi | UNSUPPORTED | |
| Bluetooth | UNSUPPORTED | |
