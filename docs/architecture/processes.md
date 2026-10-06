# JellyOS Processes, Threads and IPC

**Code:** [`kernel/process/`](../../kernel/process/), [`kernel/scheduler/`](../../kernel/scheduler/), [`kernel/ipc/`](../../kernel/ipc/), [`kernel/syscall/`](../../kernel/syscall/), [`kernel/core/object.c`](../../kernel/core/object.c), [`kernel/core/handle.c`](../../kernel/core/handle.c)
**ABI:** [../abi/syscalls.md](../abi/syscalls.md)

## Process model (README section 16)

| Part | Implementation |
|---|---|
| PID | Monotonic 64-bit counter |
| Address space | Own lower half; the kernel half is shared ([memory.md](memory.md)) |
| Threads | List of threads; the process ends when the last one is reaped |
| Handle table | Per process, see below |
| Credentials | `uid` / `gid` (root for now; permission checks arrive with the VFS) |
| Resource limits | Handles, threads, memory pages |
| Exit | Exit code and reason; the process object becomes signaled |
| Working directory | Normalized path string (`SYS_CHDIR`, `SYS_GETCWD`), see storage.md |
| Arguments, environment | Copied onto the new stack by `SYS_PROCESS_SPAWN` (startup block, see the ABI) |
| Critical flag | Set for init: its exit panics the kernel |

When the last thread is reaped, the process is handed to the **reaper**
kernel thread, which closes its handles and frees its address space. This does
not happen in the scheduler context, because closing a file may sleep on the
VFS lock or the disk. Waiters on the process wake up after finalization.

### User address layout

```text
0x0000_0000_0000_0000  page 0, never mapped
0x0000_0000_0040_0000  program image (ELF PT_LOAD segments, below 1 TiB)
0x0000_0100_0000_0000  SYS_MEMORY_ALLOCATE and SYS_SHM_MAP (each range followed by an unmapped page)
0x0000_7FFF_FFFB_0000  main thread stack, 256 KiB (unmapped guard below)
0x0000_7FFF_FFFF_0000  stack top
```

### Program loading and spawning

`process_load_elf()` maps a static ELF64 executable. It applies the same rules
as the boot manager: segments page-separated, never writable and executable,
and in user space. The ELF part was moved forward to Phase 4 because milestone
M3 needed user code; Phase 7 completes the loader with programs from files.

`process_spawn()` (`kernel/process/spawn.c`, system call `SYS_PROCESS_SPAWN`)
starts a program from a file in one step. There is no fork/exec.

1. The file is opened with `VFS_OPEN_EXEC`. It must be a regular file with an
   execute bit and execute permission for the caller. It is read whole (at
   most 64 MiB).
2. A new process gets the caller's credentials and working directory and the
   program's base name.
3. The ELF image is mapped, and the startup handles are installed in slots
   0–7 of the child's handle table.
4. The stack receives the strings, the `argv`/`envp` arrays and the
   `jelly_startup_t` block. The main thread starts with `RDI` pointing at the
   block.

The caller gets a process handle with `WAIT` (exit), `MANAGE` (kill) and
`DUPLICATE`. `SYS_PROCESS_INFO` reports the PID, the state and the exit code.

### init

After initialization, `kernel_main` unpacks the initramfs (see storage.md) and
spawns `/init` as root with the working directory `/`. Its startup handles 0,
1 and 2 are `/dev/console` for reading and writing. Its environment is
`PATH=/bin:/sbin`, `HOME=/` and `JELLY_CMDLINE=<kernel command line>`. init is
marked **critical**: if it exits, the kernel panics, because nothing could
take over its role. `SYS_PROCESS_KILL` refuses critical processes. Userspace
from init onward is described in [userspace.md](userspace.md).

## Threads and scheduler (README section 18)

| Feature | Implementation |
|---|---|
| Priorities | 32 levels; user threads 16, kernel threads 20, idle outside the queues |
| Timeslices | 10 ms round robin within a priority |
| Preemption | User mode: the timer marks a reschedule, which happens on the way back to ring 3 |
| Sleep / wakeup | One sorted sleep list with deadlines; the timer wakes expired threads |
| Idle thread | `sti; hlt` without a lost-wakeup window |
| Exit | Dead threads are reaped by the next thread after the switch (stack, context, process accounting) |

The kernel is **not preemptible**. A thread leaves the CPU only when it
blocks, yields, exits, or returns to user mode with a pending reschedule.
Scheduler state is protected by disabling interrupts. This is enough on one
CPU; SMP brings per-CPU run queues and spinlocks.

All blocking goes through **wait queues** (`scheduler/wait.h`): object waits,
sleeps and futexes. A blocked thread can be woken by its queue, by its
deadline (`TIMEOUT`) or by being killed (`INTERRUPTED`).

### Context switch (x86_64)

- `arch_context_switch` saves the callee-saved registers and RSP.
- FPU/SSE state is saved eagerly with `FXSAVE`/`FXRSTOR` on every switch. The
  kernel itself is built without SSE.
- `TSS.RSP0` and the per-CPU kernel stack pointer are set to the next thread's
  kernel stack.
- `CR3` changes only when the next thread belongs to a different process.
  Kernel threads run in whatever space is active.

## Handles and kernel objects (README section 17)

Every kernel object (process, thread, event, channel endpoint, shared memory)
starts with an `object_t`: type, reference count, operations (destroy,
signaled, consume) and a wait queue. Handles, threads and in-kernel users each
hold a reference.

Handle values encode slot and generation (see the ABI), and each entry stores
the holder's rights. Duplication can only reduce rights. Closing the last
handle releases the object; objects can also outlive handles while the kernel
uses them, for example a process that is waited on.

## IPC (README section 19)

One mechanism per problem:

| Mechanism | Purpose | Semantics |
|---|---|---|
| Event | "Something happened" | Manual or auto reset, waitable |
| Channel | Messages between two endpoints | Datagrams up to 64 KiB, 64 queued, `PEER_CLOSED` after the other side goes away |
| Shared memory | Bulk data | Frames owned by the object; mappings keep it alive |
| Futex | User-space locks and condition variables | Wait/wake on a 32-bit word, keyed by physical address so it works across processes |

| Pipe | Byte stream between programs (shell `a \| b`) | 16 KiB ring, file handles (stream vnodes), EOF without writers, `PEER_CLOSED` without readers |

Not yet available: passing handles through channels, waiting on several
objects at once, and sockets (with the network stack).

## Isolation and security (README section 41)

- Each process has its own lower half; other processes' memory is simply not
  mapped.
- User pages carry `US`; kernel pages never do, and the kernel image keeps its
  W^X rights.
- SMEP (no kernel execution of user pages), SMAP (no kernel access to user
  pages outside `copy_from_user`/`copy_to_user`) and UMIP (no `SGDT`/`SIDT` in
  ring 3) are enabled when the CPU supports them.
- Every user pointer passed to a system call is validated against the page
  tables before use.
- `IOPL = 0`: port I/O and privileged instructions fault in ring 3.
- A CPU fault in ring 3 ends only that process (`JELLY_EXIT_FAULT`), with the
  cause in its exit reason.
