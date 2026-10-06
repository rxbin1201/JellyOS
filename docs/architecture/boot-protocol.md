# JellyOS Boot Protocol

**Version:** 1
**Header:** [`boot/protocol/jelly/boot_info.h`](../../boot/protocol/jelly/boot_info.h), [`boot/protocol/jelly/boot_layout.h`](../../boot/protocol/jelly/boot_layout.h)

This document defines the contract between the JellyOS Boot Manager and the kernel:
which kernels the boot manager accepts, how it loads them, and the exact machine
state at kernel entry.

---

## 1. Kernel image requirements

The kernel is a static ELF64 executable (`ET_EXEC`, `EM_X86_64`, little endian).
The boot manager loads it from `\boot\kernels\kernel-current.elf` on the boot volume.

| Requirement | Reason |
|---|---|
| All `PT_LOAD` segments at or above `0xFFFFFFFF80000000` | Kernel lives in the top 2 GiB (`-mcmodel=kernel`) |
| Segments never share a page | Each page gets exactly the rights of its segment |
| No segment is both writable and executable | W^X from the first instruction |
| Entry point lies inside an executable segment | Rejects broken links early |
| Image span ≤ 256 MiB | Sanity limit |
| Contains the JellyOS protocol note | Explicit compatibility |

### Protocol note

Every kernel carries a `PT_NOTE` entry:

| Field | Value |
|---|---|
| name | `"JellyOS"` |
| type | `BOOT_NOTE_TYPE_PROTOCOL` (1) |
| desc | `boot_note_protocol_t { uint32_t required_version; uint32_t reserved; }` |

The boot manager refuses a kernel when the note is missing or when
`required_version` is greater than the `boot_info_t` version it produces. The
check happens **before** `ExitBootServices()`, so the error is still visible and
the boot manager can fall back to another entry.

---

## 2. Versioning rules

- `boot_info_t.magic` identifies the protocol family. It only changes on an
  incompatible break, which is treated as a new protocol.
- `boot_info_t.version` is incremented whenever fields are appended.
- `boot_info_t.size` is the size written by the boot manager. A kernel may only
  read fields that lie within `size`.
- Fields are only appended at the end. Existing fields never change meaning.
- Embedded structures (`boot_framebuffer_t`, `boot_acpi_info_t`, ...) are part of
  `boot_info_t` and versioned with it.
- Arrays (`boot_memory_entry_t`, `boot_module_t`) carry an `entry_size`. Consumers
  iterate with that stride, so the element types can grow independently.

Compatibility works in both directions:

| Situation | Result |
|---|---|
| Newer boot manager, older kernel | Works: the kernel ignores appended fields |
| Older boot manager, kernel needs a newer version | Refused by the boot manager via the note |

---

## 3. boot_info_t contents (version 1)

**All addresses inside `boot_info_t` are physical.** They are stored as `uint64_t`,
never as pointers. The kernel converts them with `hhdm_base + phys`.

| Field | Description |
|---|---|
| `magic`, `version`, `size` | See section 2 |
| `flags` | `BOOT_FLAG_DEBUG`, `SAFE_MODE`, `RECOVERY`, `SECURE_BOOT` |
| `hhdm_base`, `hhdm_size` | Direct map of physical memory (see memory-layout.md) |
| `kernel` | Physical and virtual base and size of the loaded image |
| `memory` | Converted memory map (section 4) |
| `framebuffer` | GOP linear framebuffer. `phys_base == 0` means none |
| `acpi` | RSDP address and revision |
| `smbios` | SMBIOS 3.x entry point, or 2.x as a fallback |
| `uefi` | System table address, revisions, firmware vendor (ASCII) |
| `modules` | Boot modules (initramfs, early drivers). Empty in v1 |
| `cmdline_phys`, `cmdline_length` | Kernel command line, ASCII, NUL terminated, never null |

Flags are currently fixed. They will be derived from the boot configuration and
the boot state once those exist.

---

## 4. Memory map

The UEFI memory map is converted into hardware-neutral `boot_memory_entry_t`
records. The entries are sorted by `base`, do not overlap, are page aligned, and
adjacent entries of the same type are merged.

| Type | Source | Kernel may use it |
|---|---|---|
| `USABLE` | `EfiConventionalMemory` | Immediately |
| `BOOTLOADER_RECLAIMABLE` | Loader and boot-services memory, `boot_info_t`, page tables, boot stack | After it has its own page tables, its own stack, GDT and IDT, and has copied the boot data it needs |
| `KERNEL_AND_MODULES` | Kernel image, modules | Never (as long as they are in use) |
| `ACPI_RECLAIMABLE` | `EfiACPIReclaimMemory` | After parsing ACPI tables |
| `ACPI_NVS` | `EfiACPIMemoryNVS` | Never |
| `FIRMWARE_RUNTIME` | UEFI runtime services | Never, if runtime services are used |
| `BAD` | `EfiUnusableMemory` | Never |
| `RESERVED` | Everything else, including MMIO | Never |

The boot manager allocates its own memory with OS-loader memory types
(`0x80000001` kernel, `0x80000002` boot data). These types survive in the
firmware map and are translated exactly, so no range bookkeeping is needed.

---

## 5. Kernel entry state

| Item | State |
|---|---|
| CPU mode | 64-bit long mode, CPL 0, boot CPU only |
| Interrupts | Disabled (`IF = 0`), direction flag clear |
| Paging | 4-level. 5-level paging (LA57) is refused by the boot manager |
| `CR3` | Boot manager page tables (see memory-layout.md) |
| `EFER.NXE` | Set if the CPU supports NX |
| `CR0.WP` | As left by the firmware. The kernel must set it itself |
| `RDI` | Virtual address (in the direct map) of `boot_info_t` |
| `RSP` | Top of a 64 KiB boot stack in the direct map. At entry `RSP % 16 == 8`, as after a `call`, with a zero return address |
| `RBP` | 0 |
| GDT, IDT | **Invalid.** The descriptor registers still point at firmware tables that are no longer mapped. The kernel must load its own GDT and IDT before it touches segment registers or enables interrupts. Any exception before that causes a triple fault |
| Boot services | Exited |
| Runtime services | Not remapped. `SetVirtualAddressMap()` has not been called |
| FPU/SSE | As left by the firmware. The kernel is built without SSE |

---

## 6. Planned extensions (still Phase 1)

These do not change version 1 of the layout. They only fill in fields that
already exist:

- Boot configuration file (README section 4) → `cmdline`, `flags`, kernel path
- Boot menu, timeout, entries
- Boot state tracking and rollback (README sections 5 and 6)
- Recovery and safe-mode entries
- Initramfs and early modules → `modules`
- Diagnostics screen
