# JellyOS Device Framework and Driver Modules

**Code:** [`drivers/core/`](../../drivers/core/) (device manager, resources, DMA, modules), [`drivers/bus/pci/`](../../drivers/bus/pci/), [`drivers/acpi/`](../../drivers/acpi/), interrupt routing in [`kernel/arch/x86_64/`](../../kernel/arch/x86_64/) (`irq.c`, `ioapic.c`)

## Device model (README section 22)

```text
Bus  ──discovers──▶  Device  ──matched with──▶  Driver  ──provides──▶  Capabilities
```

| Concept | Implementation |
|---|---|
| Bus (`bus_t`) | Enumerates devices and implements bus-specific operations: matching, power states, cleanup after detach |
| Device (`device_t`) | Name, place in the device tree, IDs (vendor, device, class, subclass, prog-if, revision), resources, driver, state, power state. Embedded in bus structures (`pci_device_t`) |
| Driver (`driver_t`) | Name, version, bus, ID table, capabilities (`DRIVER_CAP_*`), probe/remove/suspend/resume |
| Device tree | Owned by the device manager; `device_tree_dump()` prints it at boot |

### Lifecycle (README section 23)

```text
Discover ─▶ Match ─▶ (Load module) ─▶ Probe ─▶ Attach/Running ─▶ Suspend ⇄ Resume ─▶ Detach
              │                          │
              └── no driver: DISCOVERED   └── probe fails: FAILED (resources released, retried with the next driver)
```

- `device_register()` adds a device and immediately looks for a matching driver.
- `device_unregister()` removes a device that is gone (USB unplug): its driver's
  `remove` runs, then it leaves the tree.
- `nodriver=<name>[,<name>...]` on the kernel command line keeps drivers from
  registering at all (the names are those in the log, e.g. `nodriver=e1000,intel-hda`).
  This is the way around a driver that does not get along with a machine.
- `driver_register()` adds a driver and probes every unbound device that matches.
- Before `probe`, the device manager claims all of the device's resources and
  puts it into D0. If `probe` fails, everything is released again.
- `driver_unregister()` detaches its devices: driver `remove`, then bus cleanup
  (MSI/MSI-X off, bus mastering off), then the resources are released. After
  that, other drivers may bind.
- `device_suspend()` / `device_resume()` call the driver and the bus power
  operation (PCI: D3hot / D0, with BARs and the command register restored).

### Resource management

`resource_claim()` records MMIO, I/O port, IRQ and DMA ranges per owner and
returns `BUSY` on overlap, so two drivers can never drive the same registers.
Claims are released when a device detaches.

### DMA

`dma_alloc(device, size, max_address, &buffer)` returns zeroed, physically
contiguous memory below an address limit (for example 28 bits for QEMU's edu
device). x86 DMA is cache coherent, so the direct map is used. Without an
IOMMU, device addresses equal physical addresses; the device argument is there
for IOMMU support later.

## Interrupts

| Source | Path |
|---|---|
| MSI / MSI-X | `arch_irq_allocate()` reserves a vector in 0x30–0xEF; `arch_irq_msi_message()` gives the address/data pair; the PCI driver programs the capability or table entry |
| IOAPIC (GSI) | `arch_irq_route_gsi()` / `arch_irq_route_isa()` (applies ACPI interrupt source overrides) |
| Legacy PCI INTx | Not supported: routing needs ACPI `_PRT` (AML interpreter). Drivers use MSI or MSI-X |

Device handlers run in interrupt context. The architecture layer sends the EOI
after the handler returns.

## ACPI

`acpi_init()` validates the RSDP and all tables reachable from the XSDT (or the
RSDT), including checksums. Tables in reserved memory are mapped read-only on
demand. Parsed today:

- **MADT:** CPUs, IOAPICs, interrupt source overrides, Local APIC address
- **MCFG:** ECAM ranges for PCI Express configuration space

ACPI reclaimable memory is kept for now, because AML support will need the
tables.

## PCI (README section 24)

| Step | Implementation |
|---|---|
| Configuration access | ECAM from MCFG (1 MiB mapped per bus on first use, uncached), legacy `0xCF8/0xCFC` as fallback |
| Enumeration | Recursive scan from bus 0 through PCI-to-PCI bridges, multi-function devices |
| BAR discovery | Sizing with decoding disabled; I/O, 32-bit and 64-bit memory, prefetchable flag; recorded as device resources |
| IRQ handling | MSI and MSI-X (see above) |
| Capabilities | PM, MSI, MSI-X and PCIe offsets |
| MSI | Single vector, 32/64-bit address, INTx disabled |
| MSI-X | Table mapped through its BAR, all entries masked at enable, per-entry vectors |
| Power | PM capability: D3hot and D0, configuration restored after D0 |

Driver API: `pci_read/write{8,16,32}`, `pci_enable_device`, `pci_map_bar`,
`pci_enable_msi`, `pci_enable_msix`, `pci_set_power`.

## Modules (README section 23, milestone M4)

### Module information

Every module (built-in or loadable) declares itself with `MODULE(...)`:

```c
static const char *const deps[] = { "pci", NULL };

MODULE(.name = "edu", .description = "QEMU edu test device", .version = 1,
       .min_kernel_version = KERNEL_VERSION(0, 5, 0), .dependencies = deps,
       .init = edu_module_init, .exit = edu_module_exit);
```

| Field | Check at load time |
|---|---|
| `api_version` (set by the macro) | Must equal the kernel's `DRIVER_API_VERSION` |
| `min_kernel_version` | Must not be newer than the running kernel |
| `name` | Unique among loaded modules |
| `dependencies` | Every named module must be running |
| `init` / `exit` | `init` registers the drivers; `exit` unregisters them. A missing `exit` makes the module permanent |

Device IDs and capabilities are declared per driver (`driver_t`).

### Built-in modules

Built-in modules (currently `pci` and `virtio_blk`, see storage.md) are linked into the kernel. Their
`module_info_t` structures sit in `.jelly_modules`, and they start in
dependency order at boot.

### Loadable modules (`.ko`)

A module is an ELF64 relocatable object built like kernel code
(`-mcmodel=kernel`) and linked with `ld -r`. The loader:

1. Validates the ELF header and every section.
2. Places all `SHF_ALLOC` sections in three page-aligned groups (text, rodata,
   data) inside the module region (`0xFFFFFFFFA0000000`, 512 MiB, next to the
   kernel so 32-bit relocations reach it).
3. Applies `RELA` relocations (`R_X86_64_64`, `PC32`, `PLT32`, `32`, `32S`,
   `PC64`) with range checks. Debug sections are skipped.
4. Resolves undefined symbols **only** against exported symbols
   (`EXPORT_SYMBOL`): the kernel's export table, then other running modules'
   `.kexports`. A module that uses a symbol from another module becomes its
   user.
5. Checks the module information, then maps text `R-X`, rodata `R--` and data
   `RW-` (W^X) before running `init`.

`module_unload()` refuses built-in modules, modules without `exit` and modules
other modules still use (`BUSY`). After `exit` it verifies that the module left
no driver registered before it frees the memory.

At boot, every boot module named `*.ko` (from `module=` in the boot
configuration) is loaded. Signature verification of modules belongs to
Phase 15 (signed drivers).

### Exported kernel API (driver API version 1)

Memory (`kmalloc`, `kcalloc`, `krealloc`, `kfree`, `mem*`, `str*`), logging
(`klog`, `panic`), time (`clock_monotonic_ns`, `thread_sleep`), devices
(`driver_register`, `driver_unregister`, `device_find`, `resource_claim`,
`dma_alloc`, `dma_free`), PCI (`pci_*`), interrupts (`arch_irq_allocate`,
`arch_irq_free`, `arch_irq_msi_message`), port I/O (`arch_io_*`) and ACPI
(`acpi_find_table`).

## Not yet included

- Hotplug (PCIe native hotplug, USB) and driver matching events for userspace
- An AML interpreter (legacy INTx routing, `_PS0`/`_PS3`, device enumeration
  through ACPI)
- An IOMMU
- Module dependencies by symbol version (only the driver API version as a whole)
