# JellyOS Boot State Tracking and Rollback

**Header:** [`boot/protocol/jelly/boot_state.h`](../../boot/protocol/jelly/boot_state.h)
**Implementation:** [`boot/bootloader/boot_tracker.c`](../../boot/bootloader/boot_tracker.c)

## Storage

The UEFI variable `JellyOSBootState` stores the record. Its vendor GUID is
`c64f307e-e85f-4506-aec0-cbdccf291161` and its attributes are
`NON_VOLATILE | BOOTSERVICE_ACCESS | RUNTIME_ACCESS`. The record
(`boot_state_record_t`) has a version and a size like every other boot ABI.
An unreadable or incompatible record is replaced by a fresh one.

## States (README section 5)

| State | Written by | When |
|---|---|---|
| `BOOT_STARTED` | Boot manager | At startup |
| `BOOT_CONFIGURATION_LOADED` | Boot manager | After the configuration was parsed |
| `KERNEL_LOADED` | Boot manager | Kernel and modules are in memory |
| `KERNEL_STARTED` | Boot manager | Right before `ExitBootServices()` |
| `BOOT_SUCCESS` | **System** | Once the system is healthy (README section 43: health check) |
| `BOOT_FAILED` | Boot manager | Recorded as `last_result` when a tracked boot never reported success |
| `RECOVERY_REQUESTED` | **System** | Requests recovery for the next boot (one-shot) |

The system changes only `state`. It reads the record, sets the value and
writes it back with the same attributes. All other fields belong to the boot
manager.

## Tracked kernels

Only kernels that declare `BOOT_NOTE_FLAG_REPORTS_SUCCESS` in their ELF note
(see boot-protocol.md) take part in failure counting. A kernel without the
flag can never report success, and counting it would roll back every boot.
The current kernel does not set the flag yet. It will once it can call UEFI
runtime services.

## Decision at startup

```text
previous state = KERNEL_STARTED and tracked
    → previous boot failed: increase the counter of its mode (normal / previous kernel)
previous state = BOOT_SUCCESS
    → normal mode: reset both counters
    → previous kernel mode: reset only its own counter
      (the current kernel stays marked as broken until it boots successfully)
previous state = RECOVERY_REQUESTED
    → boot the recovery entry

normal_failures   < max_attempts → default entry, current kernel
fallback_failures < max_attempts → default entry, fallback_kernel   (BOOT_FLAG_ROLLBACK)
otherwise                        → recovery entry, menu with countdown
```

This implements the chain from README section 5:
`Normal boot → kernel fails → BOOT_FAILED → previous known-good kernel → if that fails → Recovery`.

The update system (Phase 15) resets the record after it installs a new kernel.
A successful manual boot of the default entry also clears the failures.

## Wear

The boot manager writes the variable at most four times per boot, only when
the state changes. If the variable cannot be written, tracking is disabled for
that boot and a warning is logged. The system still boots.
