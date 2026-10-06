# JellyOS Virtual Memory Layout (x86_64)

**Constants:** [`boot/protocol/jelly/boot_layout.h`](../../boot/protocol/jelly/boot_layout.h), [`kernel/arch/x86_64/linker.ld`](../../kernel/arch/x86_64/linker.ld)

JellyOS uses 4-level paging with 48-bit canonical addresses. The lower half
belongs to userspace and the upper half to the kernel.

```text
0x0000_0000_0000_0000 ┐
                      │ User space (per process)                 128 TiB
0x0000_7FFF_FFFF_FFFF ┘
        ... non-canonical hole ...
0xFFFF_8000_0000_0000 ┐ PML4 256
                      │ Direct map of physical memory (HHDM)     up to 64 TiB
0xFFFF_BFFF_FFFF_FFFF ┘ PML4 383
0xFFFF_C000_0000_0000 ┐ PML4 384
                      │ Reserved: kernel heap, vmalloc,          ~64 TiB
                      │ MMIO mappings, per-CPU data (Phase 3)
0xFFFF_FFFF_7FFF_FFFF ┘
0xFFFF_FFFF_8000_0000 ┐ PML4 511
                      │ Kernel image (text, rodata, data, bss)   2 GiB
0xFFFF_FFFF_FFFF_FFFF ┘
```

Kernel code never hardcodes these addresses. It reads `hhdm_base` from
`boot_info_t`, and image symbols come from the linker script.

---

## Mappings at kernel entry

The boot manager builds the following page tables:

| Region | Mapping | Rights | Page size |
|---|---|---|---|
| Kernel image | Each `PT_LOAD` segment at its link address | From the ELF flags: text `R-X`, rodata `R--`, data/bss `RW-` | 4 KiB |
| Direct map | Physical `[0, hhdm_size)` at `hhdm_base` | `RW-`, NX | 2 MiB |
| Handoff trampoline | Identity mapping of its 1–2 pages | `R-X` | 4 KiB |

`hhdm_size` covers the highest RAM-like address in the firmware memory map and
the framebuffer. It is at least 4 GiB, so the usual low MMIO (Local APIC, I/O
APIC, HPET) is covered, and it is rounded up to whole GiB.

### Rules for the kernel

- The lower half is **not** identity mapped. Only the trampoline pages exist
  there, and the kernel must drop them.
- Everything in the direct map is cached write-back. MMIO and the framebuffer
  must get proper cache attributes (UC or WC via PAT) once the kernel builds its
  own tables in Phase 3. Until then, `early_map_mmio()` marks individual
  direct-map pages as uncached (see x86_64.md).
- The kernel switches to its own 64 KiB stack in `.bss` at entry. The boot
  stack is no longer used after that.
- The boot page tables live in `BOOTLOADER_RECLAIMABLE` memory. The kernel
  replaces them with its own before it reclaims that memory.
- The boot stack has no guard page. The kernel switches to its own stacks early.
