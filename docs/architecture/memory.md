# JellyOS Memory Management

**Code:** [`kernel/memory/`](../../kernel/memory/), page-table primitives in [`kernel/arch/x86_64/paging.c`](../../kernel/arch/x86_64/paging.c)
**Layout:** [memory-layout.md](memory-layout.md)

## Physical memory manager (README section 13)

`kernel/memory/pmm.c` uses one bit per 4 KiB frame (1 = in use). The bitmap
sits in the first usable region large enough to hold it.

| State | Meaning |
|---|---|
| Free | Usable RAM, available to `pmm_alloc_page()` / `pmm_alloc_pages()` |
| Allocated | Handed out, returned with `pmm_free_page[s]()` |
| Reserved | Firmware, kernel image, modules, the bitmap, and everything below 1 MiB |
| Reclaimable | `BOOTLOADER_RECLAIMABLE` and `ACPI_RECLAIMABLE`, freed by `pmm_reclaim(type)` |

- Memory below 1 MiB stays reserved for legacy firmware areas and the future
  SMP trampoline.
- `pmm_alloc_pages(n)` returns physically contiguous frames (first fit).
- Freeing a frame that is not allocated panics, which catches double frees.
- Boot manager memory is reclaimed once the kernel has its own page tables and
  stack and has copied the boot data. ACPI memory is reclaimed after the ACPI
  tables are parsed (Phase 5).

Later improvements (buddy allocator, NUMA, per-CPU caches, huge pages) can
replace the bitmap behind the same interface.

## Virtual memory manager (README section 14)

The architecture layer provides the page-table primitives (`memory/mmu.h`):
create, map, unmap, query, destroy, activate. The VMM (`memory/vmm.c`) builds
address spaces from them.

| Feature | Implementation |
|---|---|
| Protection flags | `VM_WRITE`, `VM_EXEC`, `VM_USER`, `VM_GLOBAL`. Pages are always readable; NX is used whenever the CPU supports it |
| Cache modes | `VM_UNCACHED` and `VM_WRITE_COMBINING` through the PAT (index 0 WB, 1 WC, 2 UC-, 3 UC) |
| Ownership | `VM_OWNED` (a software PTE bit): the frame belongs to the mapping and is freed on unmap or when the space is destroyed |
| User/kernel separation | `VM_USER` only below `USER_SPACE_END`, kernel mappings only in the upper half, page 0 never mapped |
| Address spaces | `vmm_space_create()` shares the kernel half. `vmm_space_destroy()` frees the user page tables and owned frames |
| Shared mappings | The same frame mapped without `VM_OWNED` in several spaces. The owner (later a shared-memory object) controls its lifetime |
| Guard pages | Kernel stacks are allocated in slots of `[unmapped guard][64 KiB stack]` |
| MMIO | `vmm_map_mmio(phys, size, VM_UNCACHED or VM_WRITE_COMBINING)` |

### Page faults

The page-fault handler calls `vmm_page_fault()` first. That function will
handle demand paging and copy-on-write once processes exist, and currently
resolves nothing. Unresolved faults panic with a classified cause:

| Cause | Example |
|---|---|
| null pointer dereference | Access to page 0 |
| kernel stack overflow (guard page) | Recursion runs into the guard page |
| write to read-only page | Write to `.rodata` or `.text` |
| execution of non-executable page | Jump into data |
| unmapped kernel / user address | Wild pointer |

A stack overflow usually escalates to a double fault, because the CPU cannot
push the page-fault frame onto the full stack. The double-fault handler runs on
its own IST stack and checks the guard pages, so it reports the overflow as
such.

## Kernel heap (README section 15)

`kmalloc`, `kcalloc`, `krealloc` and `kfree` live in `memory/heap.c`.

| Size | Allocation |
|---|---|
| Up to 2032 bytes | Size classes 32–2048 bytes (including a 16-byte header), carved from single pages |
| Larger | Physically contiguous pages via `pmm_alloc_pages()` |

- Memory is 16-byte aligned and reached through the direct map.
- Every block has a header with a magic value. `kfree()` panics on double frees,
  foreign pointers and corrupted headers.
- Freed small blocks are filled with `0xDE` so use-after-free is easy to spot.
- `kcalloc()` checks for multiplication overflow. `krealloc()` resizes in place
  when the block's size class still fits.
- All heap, PMM and VMM bookkeeping runs with interrupts disabled. Spinlocks
  replace this when SMP arrives.

Pages of small size classes are not returned to the PMM yet. A slab allocator
with object caches and per-CPU caches (README section 15, "Later") will replace
the size classes.
