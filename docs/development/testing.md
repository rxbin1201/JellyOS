# Testing JellyOS

## Kernel self-tests (`make test`)

Kernel tests live in [`tests/kernel/`](../../tests/kernel/) and are linked
into the kernel. A test is a function registered with `KTEST`:

```c
#include "tests/kernel/ktest.h"

KTEST(heap_calloc)
{
    uint32_t *values = kcalloc(1000, sizeof(uint32_t));
    KASSERT(values != NULL);     /* failure stops this test */
    KEXPECT(values[999] == 0);   /* failure is recorded, the test continues */
    kfree(values);
}
```

`make test` boots the kernel in QEMU with a separate ESP and the command line
`selftest=exit`. The kernel runs every test after initialization and reports
the result through QEMU's `isa-debug-exit` device:

| QEMU exit status | Meaning |
|---|---|
| 1 | All tests passed |
| 3 | At least one test failed |
| 124 | Timeout (the kernel hung or panicked) |

`make test KVM=1` runs the same tests on the host CPU.

On a normal boot, `selftest=1` runs the tests and keeps the system running.

### User-mode tests

[`tests/userspace/usertest.c`](../../tests/userspace/usertest.c) is a static
user program built on libos. It is embedded in the kernel
(`tests/kernel/user_images.S`) and started by
[`tests/kernel/process_tests.c`](../../tests/kernel/process_tests.c) with a
scenario number and startup handles. Exit code 0 means success; any other code
is the source line of the failed check. Fault scenarios must be killed with
`JELLY_EXIT_FAULT`.

The process tests cover ring 3 execution, isolation (faults, foreign address
spaces, bad pointers), threads with a futex mutex, FPU state across
preemption, sleep, memory limits, handle rights and generations, channels,
shared memory, and the release of all resources after exit.

### Driver tests

`make test` adds the QEMU devices `edu` (1234:11e8) and `e1000e` (8086:10d3)
and passes the modules from [`tests/drivers/`](../../tests/drivers/) as boot
modules:

| Module | Purpose |
|---|---|
| `edu.ko` | Probe verifies MMIO, an MSI interrupt and DMA in both directions; supports suspend/resume |
| `e1000e_msix.ko` | Probe verifies MSI-X delivery (link status change cause through IVAR "other") |
| `bad_api.ko`, `bad_dependency.ko`, `bad_symbol.ko` | Must be rejected (API version, missing dependency, unexported symbol) |

[`tests/kernel/driver_tests.c`](../../tests/kernel/driver_tests.c) checks
ACPI, IOAPIC routing (PIT channel 0 through ISA IRQ 0), PCI enumeration and
BARs, resource conflicts, module loading/rejection/unloading/reloading,
dependency tracking, and suspend/resume including PCI D3hot.

To confirm that a test can actually fail, break the code it covers once. For
example, disabling `FXSAVE`/`FXRSTOR` in `kernel/arch/x86_64/thread.c` makes
`preemption_preserves_fpu_state` fail.

## Fault tests

`crashtest=<kind>` on the kernel command line triggers a fault after
initialization, to check exception handling and panic output:

| Kind | Expected result |
|---|---|
| `divide` | Panic: divide error |
| `pagefault` | Panic: page fault, unmapped address |
| `invalid-opcode` | Panic: invalid opcode |
| `write-rodata` | Panic: write to read-only page |
| `stack-overflow` | Panic: double fault, kernel stack overflow (guard page hit) |
| `breakpoint` | Logged, execution continues |
| `panic` / `assert` | Panic with message / failed assertion |
