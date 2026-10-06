# JellyOS Virtual Memory Layout (x86_64)

**Constants:** [`kernel/memory/layout.h`](../../kernel/memory/layout.h), [`boot/protocol/jelly/boot_layout.h`](../../boot/protocol/jelly/boot_layout.h), [`kernel/arch/x86_64/linker.ld`](../../kernel/arch/x86_64/linker.ld)
**Related:** [memory.md](memory.md) (PMM, VMM, heap)

JellyOS uses 4-level paging with 48-bit canonical addresses. The lower half
belongs to userspace and the upper half to the kernel.

```text
0x0000_0000_0000_0000 ┐ page 0: never mapped (null pointer guard)
0x0000_0000_0000_1000 │ User space (per process)                     128 TiB
0x0000_7FFF_FFFF_FFFF ┘
        ... non-canonical hole ...
0xFFFF_8000_0000_0000 ┐ PML4 256
                      │ Direct map of physical RAM (hhdm_base)       up to 64 TiB
0xFFFF_BFFF_FFFF_FFFF ┘ PML4 383
0xFFFF_C000_0000_0000 ┐ PML4 384
                      │ Kernel stacks: [guard page][64 KiB stack]... 1 TiB
0xFFFF_C0FF_FFFF_FFFF ┘
0xFFFF_C100_0000_0000 ┐ PML4 386
                      │ MMIO: device registers, framebuffers          1 TiB
0xFFFF_C1FF_FFFF_FFFF ┘
0xFFFF_C200_0000_0000 ┐
                      │ Reserved (vmalloc, per-CPU data, ...)
0xFFFF_FFFF_7FFF_FFFF ┘
0xFFFF_FFFF_8000_0000 ┐ PML4 511
                      │ Kernel image (text, rodata, data, bss)        2 GiB
0xFFFF_FFFF_FFFF_FFFF ┘
```

All region bases are defined in `kernel/memory/layout.h`. Nothing else
hardcodes virtual addresses. The direct-map base comes from `boot_info_t`.

---

## Kernel address space (built by the VMM)

| Region | Mapping | Rights | Cache | Page size |
|---|---|---|---|---|
| Direct map | Every RAM range of the memory map (not MMIO holes) | `RW-`, global | WB | 2 MiB where aligned, else 4 KiB |
| Kernel text | `__text_start`–`__text_end` | `R-X`, global | WB | 4 KiB |
| Kernel rodata | `__rodata_start`–`__rodata_end` | `R--`, global | WB | 4 KiB |
| Kernel data/bss | `__data_start`–`__kernel_end` | `RW-`, global | WB | 4 KiB |
| Kernel stacks | Allocated per stack, guard page unmapped | `RW-`, global | WB | 4 KiB |
| MMIO | Through `vmm_map_mmio()` | `RW-`, global | UC or WC | 4 KiB |

All 256 kernel-half top-level entries are allocated at startup. A new user
address space copies them, so kernel mappings made later are visible in every
address space without extra synchronization.

The direct map contains only RAM. Mapping MMIO write-back would let the CPU
cache or speculatively read device registers.

---

## Mappings at kernel entry (boot manager)

Until the VMM switches `CR3`, the kernel runs on the boot manager's tables:

| Region | Mapping | Rights | Page size |
|---|---|---|---|
| Kernel image | Each `PT_LOAD` segment at its link address | From the ELF flags | 4 KiB |
| Direct map | Physical `[0, hhdm_size)` at `hhdm_base`, write-back | `RW-`, NX | 2 MiB |
| Handoff trampoline | Identity mapping of its 1–2 pages | `R-X` | 4 KiB |

`hhdm_size` covers at least 4 GiB and the framebuffer. The kernel keeps the
same direct-map base, so pointers into it remain valid across the switch.

### Startup sequence

1. `_start` switches to a 64 KiB entry stack in `.bss`.
2. `boot_accept()` copies `boot_info_t`, the memory map, the module list, the
   command line and the boot log into the kernel.
3. The VMM builds the kernel address space and activates it.
4. The kernel switches to a stack with a guard page (`arch_switch_stack`).
5. `BOOTLOADER_RECLAIMABLE` memory (boot tables, boot stack, boot data) is
   returned to the PMM.
