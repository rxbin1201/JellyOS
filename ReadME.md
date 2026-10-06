# JellyOS – Final Operating System Architecture & Development Plan

**Status:** Architecture 1.0  
**Target:** x86_64 UEFI desktop operating system  
**Primary language:** C  
**Low-level language:** x86_64 Assembly  
**Kernel model:** Modular monolithic kernel  
**Executable format:** ELF64  
**Initial development target:** QEMU + OVMF

---

## 1. Project Vision

JellyOS is intended to become a complete, modern, general-purpose 64-bit desktop operating system.

The long-term goal is not simply to boot a kernel or display a graphical desktop. JellyOS should provide a stable platform on which users can:

- Run native applications
- Browse the Internet
- Manage files
- Use audio and video
- Connect USB devices
- Use Wi-Fi and Ethernet
- Play games
- Develop software
- Install and update applications
- Recover from system failures
- Use modern graphics hardware

The architecture is designed from the beginning to avoid large rewrites later.

### Core principles

1. **Interfaces before implementations**
2. **Hardware-specific code belongs in drivers**
3. **Userspace whenever practical**
4. **Stable APIs and ABIs**
5. **No unnecessary hardware-specific assumptions**
6. **Explicit ownership and error handling**
7. **Test every major subsystem**
8. **Keep the kernel small enough to maintain**
9. **Make recovery a first-class feature**
10. **Design for future hardware, not only the development machine**

The goal is a stable architecture rather than a large number of features.

---

# 2. Repository Structure

```text
JellyOS/
├── boot/
│   ├── bootloader/
│   ├── protocol/
│   └── recovery/
├── kernel/
│   ├── core/
│   ├── arch/
│   │   └── x86_64/
│   ├── memory/
│   ├── process/
│   ├── scheduler/
│   ├── ipc/
│   ├── syscall/
│   ├── security/
│   ├── time/
│   └── power/
├── drivers/
│   ├── core/
│   ├── bus/
│   │   ├── pci/
│   │   ├── usb/
│   │   └── virtio/
│   ├── storage/
│   ├── graphics/
│   ├── network/
│   ├── input/
│   └── audio/
├── fs/
│   ├── block/
│   ├── partition/
│   ├── vfs/
│   └── filesystems/
├── net/
│   ├── link/
│   ├── arp/
│   ├── ipv4/
│   ├── ipv6/
│   ├── tcp/
│   ├── udp/
│   ├── dns/
│   └── sockets/
├── graphics/
│   ├── core/
│   ├── display/
│   ├── compositor/
│   ├── window/
│   └── gui/
├── audio/
├── input/
├── userspace/
│   ├── init/
│   ├── libc/
│   ├── libos/
│   ├── services/
│   ├── shell/
│   ├── desktop/
│   └── applications/
├── sdk/
│   ├── include/
│   ├── lib/
│   └── tools/
├── tools/
│   ├── image_builder/
│   ├── debugger/
│   ├── profiler/
│   └── test/
├── tests/
│   ├── unit/
│   ├── kernel/
│   ├── drivers/
│   ├── userspace/
│   └── integration/
├── docs/
│   ├── architecture/
│   ├── abi/
│   ├── drivers/
│   └── development/
├── build/
├── Makefile
└── README.md
```

---

# 3. Boot Architecture

```text
UEFI Firmware
      │
      ▼
JellyOS Boot Manager
      │
      ├── Configuration
      ├── Boot Menu
      ├── Boot State
      ├── Recovery Selection
      ├── Diagnostics
      └── Security Verification
      │
      ▼
Kernel Loader
      │
      ├── Kernel ELF
      ├── Initramfs
      └── Early Modules
      │
      ▼
Boot Information Structure
      │
      ▼
ExitBootServices()
      │
      ▼
JellyOS Kernel
```

## Bootloader responsibilities

The bootloader should handle:

- UEFI initialization
- EFI filesystem access
- Boot configuration parsing
- Boot entry selection
- Boot timeout
- Kernel loading
- ELF validation
- Initramfs loading
- Early module loading
- GOP framebuffer setup
- UEFI memory map
- ACPI discovery
- SMBIOS discovery
- CPU and firmware information
- Boot device information
- Secure Boot state
- Signature verification architecture
- Boot-state tracking
- Previous kernel selection
- Recovery mode
- Safe mode
- Diagnostics
- Boot logs
- Kernel command line

The bootloader should **not** contain:

- A scheduler
- Normal process management
- A full network stack
- A desktop
- A full runtime USB stack
- A full VFS
- Application logic

The bootloader is a boot environment, not a miniature operating system.

---

# 4. Boot Configuration

Example:

```ini
[boot]
default=JellyOS
timeout=5
menu=auto

[entry JellyOS]
kernel=/boot/kernels/kernel-current.elf
initrd=/boot/initrd/current.img
cmdline="loglevel=info"

[entry Recovery]
kernel=/boot/kernels/kernel-current.elf
initrd=/boot/initrd/recovery.img
cmdline="recovery=1"

[entry SafeMode]
kernel=/boot/kernels/kernel-current.elf
initrd=/boot/initrd/minimal.img
cmdline="safe_mode=1"
```

A broken optional configuration must never permanently brick the system.

The bootloader should fall back to safe defaults whenever possible.

---

# 5. Boot State Tracking

JellyOS should track boot progress:

```text
BOOT_STARTED
BOOT_CONFIGURATION_LOADED
KERNEL_LOADED
KERNEL_STARTED
BOOT_SUCCESS
BOOT_FAILED
RECOVERY_REQUESTED
```

This enables automatic recovery after repeated failed boots.

For example:

```text
Normal boot
    ↓
Kernel fails
    ↓
BOOT_FAILED
    ↓
Previous known-good kernel
    ↓
If that fails
    ↓
Recovery
```

---

# 6. Kernel Version Management

Kernels should be stored independently:

```text
/boot/kernels/
    kernel-1.0.0.elf
    kernel-1.1.0.elf
    kernel-1.2.0.elf
    kernel-current.elf
    kernel-previous.elf
```

The boot manager should support:

- Current kernel
- Previous known-good kernel
- Recovery kernel
- Manually selected kernel
- Automatic rollback

This will later integrate with the JellyOS update system.

---

# 7. Recovery System

Recovery should not turn the bootloader into a huge application.

Architecture:

```text
JellyOS Boot Manager
        │
        ▼
Recovery Kernel
        │
        ▼
Recovery Init
        │
        ├── Recovery Shell
        ├── Filesystem Check
        ├── Log Viewer
        ├── Configuration Repair
        ├── Kernel Rollback
        ├── Driver Removal
        └── User Backup
```

Recovery should be usable even when the normal desktop cannot start.

---

# 8. Boot Diagnostics

Diagnostics should display:

- Firmware version
- CPU
- CPU cores
- Memory
- ACPI
- SMBIOS
- Framebuffer
- Secure Boot
- Boot device
- Selected kernel
- Initramfs
- Loaded modules
- Boot flags
- Boot state

A verbose debug mode should be available:

```text
debug=1
```

---

# 9. Boot Information ABI

The bootloader passes a versioned structure to the kernel:

```c
typedef struct {
    uint32_t version;
    uint32_t size;

    memory_map_t memory;
    framebuffer_info_t framebuffer;
    acpi_info_t acpi;
    smbios_info_t smbios;
    uefi_info_t uefi;
    cpu_info_t cpu;

    boot_device_t boot_device;
    module_list_t modules;
    boot_flags_t flags;

    const char *cmdline;
} boot_info_t;
```

Rules:

- Every structure has a version.
- Every structure has a size.
- Never silently change the meaning of existing fields.
- Add new fields at the end where possible.
- Kernel compatibility must be explicit.
- ABI changes must be documented.

---

# 10. Kernel Architecture

JellyOS uses a **modular monolithic kernel**.

Core components:

```text
Kernel
├── Architecture
├── Interrupts
├── Memory
├── Scheduler
├── Processes
├── Threads
├── IPC
├── Syscalls
├── Security
├── Time
├── Power
├── Device Manager
├── Driver Manager
├── VFS
├── Network Core
├── Graphics Core
├── Audio Core
└── Input Core
```

A modular monolithic design provides:

- High performance
- Direct kernel subsystem communication
- Driver modularity
- Easier development than a fully microkernel architecture
- The ability to move suitable services into userspace later

---

# 11. x86_64 Architecture Layer

All architecture-specific code belongs under:

```text
kernel/arch/x86_64/
```

This includes:

- GDT
- IDT
- Exception entry
- Interrupt entry
- APIC
- CPU initialization
- Page-table primitives
- Context switching
- Atomic operations
- CPU feature detection
- Architecture-specific timers

Generic kernel code should not directly depend on x86-specific implementation details.

---

# 12. CPU and Interrupt Initialization

Initial order:

```text
GDT
 ↓
IDT
 ↓
Exception Handlers
 ↓
Serial / Debug Output
 ↓
PIC / APIC
 ↓
Timer
 ↓
Local APIC
 ↓
SMP Startup
 ↓
Per-CPU Structures
```

Development should initially target one CPU.

SMP should be introduced after the basic kernel is stable.

---

# 13. Physical Memory Manager

The PMM manages physical page frames.

Initial implementation:

```text
Free
Reserved
Allocated
Reclaimable
```

A bitmap allocator is sufficient initially.

Later improvements may include:

- Buddy allocator
- NUMA awareness
- Per-CPU caches
- Huge pages

---

# 14. Virtual Memory

The virtual memory subsystem provides:

- Page tables
- Address spaces
- Mapping
- Unmapping
- Protection flags
- User/kernel separation
- Page faults
- Guard pages
- Shared mappings

The virtual address layout must be documented early.

Avoid scattered magic addresses.

---

# 15. Kernel Heap

Initial API:

```c
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);
```

Later:

- Slab allocator
- Object caches
- Per-CPU allocation caches

---

# 16. Process Model

A process contains:

- PID
- Address space
- Threads
- Handle table
- Credentials
- Environment
- Working directory
- Signals/events
- Resource limits

Threads are independently schedulable execution units.

---

# 17. Handle System

Userspace should never receive raw kernel pointers.

Instead:

```text
Userspace
   │
   │ handle
   ▼
Kernel Handle Table
   │
   ▼
Kernel Object
```

Handles may represent:

- Files
- Directories
- Processes
- Threads
- Sockets
- Events
- Devices
- Shared memory
- Other kernel objects

Benefits:

- Safety
- ABI stability
- Easier debugging
- Better sandboxing
- Cleaner resource management

---

# 18. Scheduler

The initial scheduler should support:

- Preemption
- Priorities
- Timeslices
- Sleep
- Wakeup
- Idle thread

Later:

- SMP run queues
- Load balancing
- Priority classes
- Realtime scheduling

---

# 19. IPC

JellyOS should provide:

- Pipes
- Events
- Shared memory
- Message queues
- Sockets
- Futex-like synchronization

Avoid implementing multiple mechanisms that solve exactly the same problem.

---

# 20. System Call ABI

The syscall ABI should be designed early and versioned.

Categories:

```text
Process
Thread
Memory
File
Directory
Device
Socket
IPC
Time
Graphics
Audio
Input
Security
```

Every syscall must define:

- Number
- Arguments
- Return value
- Error behavior
- Permissions
- ABI version

---

# 21. Global Error Model

Initial errors:

```text
SUCCESS
INVALID_ARGUMENT
NOT_FOUND
ACCESS_DENIED
OUT_OF_MEMORY
BUSY
NOT_SUPPORTED
IO_ERROR
TIMEOUT
DEVICE_ERROR
```

The error system must be consistent across kernel and userspace APIs.

---

# 22. Device Model

The general model is:

```text
Bus
  ↓
Device
  ↓
Driver
  ↓
Capabilities
```

Device resources may include:

- MMIO
- I/O ports
- Interrupts
- DMA
- Bus addresses
- Device IDs
- Power states

The Device Manager owns the system device tree.

---

# 23. Driver Manager

The Driver Manager is a central JellyOS subsystem.

Driver lifecycle:

```text
Discover
   ↓
Match
   ↓
Load
   ↓
Probe
   ↓
Attach
   ↓
Running
   ↓
Suspend / Resume
   ↓
Detach
```

Driver modules should declare:

- API version
- Required kernel version
- Supported device IDs
- Capabilities
- Dependencies

Initially, drivers may be statically linked.

Later, JellyOS should support dynamically loaded driver modules.

The module loader should support:

- Validation
- Version checking
- Dependencies
- Symbol resolution
- Relocations
- Initialization
- Safe unloading where possible

---

# 24. PCI

PCI should be the first major hardware bus.

Implement:

1. PCI configuration access
2. Device enumeration
3. BAR discovery
4. IRQ handling
5. PCI capabilities
6. MSI
7. MSI-X

This becomes the basis for many later drivers.

---

# 25. Storage Architecture

```text
NVMe / AHCI / USB / VirtIO
          ↓
    Block Device API
          ↓
   Partition Manager
          ↓
    Filesystem Driver
          ↓
          VFS
          ↓
      File API
```

The layers must remain independent.

---

# 26. VFS

The VFS should support:

- open
- close
- read
- write
- seek
- stat
- mkdir
- unlink
- rename
- readdir
- mount
- unmount

Additional concepts:

- Mount points
- Permissions
- Directories
- Symlinks
- Metadata
- File handles

Boot should initially use FAT32.

The installed system should initially use one reliable filesystem.

Additional filesystems can be added later.

---

# 27. Initramfs

The initial filesystem should contain approximately:

```text
/init
/bin/
/lib/
/etc/
/dev/
```

Responsibilities:

1. Mount required virtual filesystems
2. Discover the root filesystem
3. Start essential services
4. Transition into the normal root filesystem

The initramfs must be replaceable without rebuilding the kernel.

---

# 28. Userspace

Core userspace:

```text
Userspace
├── Init
├── libc
├── libos
├── Service Manager
├── Device Services
├── Network Services
├── Display Server
├── Audio Server
├── Shell
├── Desktop
└── Applications
```

---

# 29. Init System

`init` should:

- Mount filesystems
- Initialize `/dev`
- Start essential services
- Start device management
- Initialize networking
- Start login/session management
- Start the desktop

The init system should remain independent of the graphical desktop.

---

# 30. libc

Initial libc functionality:

- Memory
- Strings
- stdio
- Files
- Processes
- Threads
- Time
- Environment

libc translates standard APIs into JellyOS syscalls.

---

# 31. Service Manager

The service manager should support:

- Start
- Stop
- Restart
- Dependencies
- Failure detection
- Logging
- Automatic restart
- Startup ordering

---

# 32. Networking

Architecture:

```text
NIC Driver
   ↓
Network Interface
   ↓
Ethernet
   ↓
ARP / Neighbor Discovery
   ↓
IPv4 / IPv6
   ↓
TCP / UDP
   ↓
Sockets
   ↓
Applications
```

Implementation order:

1. Network abstraction
2. Ethernet
3. ARP
4. IPv4
5. ICMP
6. UDP
7. TCP
8. IPv6
9. DNS
10. Higher-level services

---

# 33. Graphics Architecture

Development order:

```text
UEFI GOP Framebuffer
        ↓
Basic Drawing
        ↓
Display Abstraction
        ↓
Surfaces
        ↓
Windows
        ↓
Compositor
        ↓
GUI Toolkit
        ↓
Hardware Acceleration
```

Start with the UEFI framebuffer.

Do not make a GPU driver a prerequisite for having a functional desktop.

---

# 34. Graphics API

The graphics API should expose hardware-independent concepts:

- Device
- Context
- Buffer
- Texture
- Shader
- Pipeline
- Command Buffer
- Surface
- Swapchain

This allows different GPU backends to share one application-facing API.

---

# 35. Display Server

A userspace display server should eventually manage:

- Displays
- Surfaces
- Windows
- Composition
- Input routing
- Display configuration

---

# 36. GUI Toolkit

Reusable controls:

```text
Window
Button
Text
Input
List
Table
Menu
Dialog
ScrollView
Layout
Theme
Icon
```

The toolkit should support:

- Light mode
- Dark mode
- Accessibility scaling
- Keyboard navigation
- Consistent controls
- Optional transparency
- Modern visual effects

JellyOS should have a coherent visual identity rather than mixing unrelated UI paradigms.

---

# 37. Input System

Hardware:

```text
USB HID
PS/2
Bluetooth
Gamepads
        ↓
Input Manager
        ↓
Standardized Events
```

Events:

```text
KEY_DOWN
KEY_UP
MOUSE_MOVE
MOUSE_BUTTON
MOUSE_WHEEL
GAMEPAD_BUTTON
GAMEPAD_AXIS
```

Applications should not need to understand individual keyboard or mouse hardware.

---

# 38. Audio

Architecture:

```text
Audio Driver
     ↓
Audio Device
     ↓
Audio Server
     ↓
Mixer
     ↓
Application
```

Initial features:

- Playback
- Volume
- Multi-client audio
- Sample-rate handling

Later:

- Recording
- Low-latency audio
- Bluetooth audio

---

# 39. Power Management

Initial:

- Shutdown
- Reboot

Later:

- Suspend
- Hibernate
- CPU power management
- Display power management
- Battery support

---

# 40. Time System

Provide:

- Monotonic clock
- Wall clock
- High-resolution timers
- Sleep
- Timer objects

Applications should not access hardware timers directly.

---

# 41. Security

Initial security boundaries:

- Kernel/user separation
- Memory protection
- Process isolation
- File permissions
- Ownership

Later:

- Users and groups
- Capabilities
- Sandboxing
- Signed applications
- Signed drivers
- Secure Boot integration

Security must be designed into the architecture instead of added as a final feature.

---

# 42. Package System

A JellyOS package should contain:

- Manifest
- Application
- Libraries
- Resources
- Icons
- Permissions
- Version
- Dependencies

Package manager operations:

```text
install
remove
update
rollback
verify
list
search
```

---

# 43. Update System

Safe update flow:

```text
Download
   ↓
Verify
   ↓
Install inactive version
   ↓
Boot
   ↓
Health check
   ↓
Mark successful
```

If the new version fails:

```text
Failed boot
    ↓
Previous version
    ↓
Recovery
```

This should apply to kernels and eventually to system components.

---

# 44. Logging

Kernel:

```c
klog_info();
klog_warn();
klog_error();
klog_debug();
```

Userspace should have a central logging service.

Logs must be accessible from:

- Normal system
- Recovery
- Diagnostics

---

# 45. Crash Handling

Kernel panic information should include:

- Panic reason
- CPU
- RIP
- CR2
- Stack trace
- Relevant register state

Later:

- Crash dumps
- Persistent logs
- Automatic reboot
- Recovery integration

---

# 46. Testing

JellyOS should use:

### Unit tests

Individual functions and algorithms.

### Kernel tests

Memory, scheduler, IPC, handles, syscalls.

### Driver tests

Device discovery and driver lifecycle.

### Userspace tests

libc and system services.

### Integration tests

Full system functionality.

### System tests

Booting and using a complete JellyOS installation.

---

# 47. Development Environment

Primary target:

```text
QEMU
+
OVMF
```

Initial virtual hardware:

- VirtIO disk
- VirtIO network
- GOP framebuffer
- USB keyboard
- USB mouse

Real hardware testing should be introduced progressively.

---

# 48. Hardware Compatibility Matrix

Each device should have a status:

```text
SUPPORTED
PARTIALLY_SUPPORTED
EXPERIMENTAL
UNSUPPORTED
```

Examples:

- Storage
- Ethernet
- Wi-Fi
- USB
- Audio
- GPU
- Bluetooth

This prevents the project from confusing “works on my PC” with actual hardware support.

---

# 49. Performance Metrics

Measure:

- Boot time
- Context-switch latency
- Syscall latency
- Filesystem throughput
- Network throughput
- Frame time
- Audio latency
- Memory consumption

Performance should be measured rather than guessed.

---

# 50. Debugging

Required tools:

- Serial output
- GDB
- QEMU debugging
- Kernel symbols
- Stack traces
- Assertions
- Structured logs

Useful build commands:

```text
make
make run
make debug
make test
make iso
make image
make clean
make release
```

---

# 51. Coding Rules

JellyOS code should follow these rules:

- No hidden global state where avoidable
- No magic addresses
- No unchecked hardware access
- Explicit ownership
- Explicit errors
- Documented public interfaces
- Assertions for invariants
- Consistent naming
- No abstraction bypasses

Subsystem boundaries must be respected.

---

# 52. Development Roadmap

## Phase 0 – Architecture Specification

Define:

- Repository
- Memory map
- Boot ABI
- Boot flags
- Kernel entry contract
- Syscall ABI draft
- Handle model
- Driver/device interfaces
- VFS API
- Error model
- Versioning
- Coding standards
- Test strategy

**Deliverable:** Complete architecture documentation.

---

## Phase 1 – Bootloader

Implement:

- UEFI entry
- Logging
- FAT32
- ELF loader
- Configuration parser
- Boot info
- Memory map
- GOP
- ACPI
- SMBIOS
- Kernel handoff
- Boot menu
- Boot state
- Recovery
- Previous kernel
- Diagnostics

**Milestone:**

```text
UEFI
 ↓
JellyOS Boot Manager
 ↓
Select JellyOS
 ↓
Load Kernel
 ↓
Pass boot_info
 ↓
Kernel starts
```

---

## Phase 2 – Minimal Kernel

Implement:

- Architecture initialization
- GDT
- IDT
- Exceptions
- Interrupts
- Serial
- Panic
- Timer
- APIC
- Single-core execution

---

## Phase 3 – Memory

Implement:

- PMM
- Virtual memory
- Page tables
- Kernel heap
- Page faults
- User address spaces

---

## Phase 4 – Multitasking

Implement:

- Threads
- Context switching
- Scheduler
- Processes
- User mode
- Syscalls
- Handles
- IPC

**Milestone:** Isolated userspace processes.

---

## Phase 5 – Device Framework

Implement:

- Device objects
- Bus objects
- Drivers
- Driver matching
- Driver lifecycle
- Driver versioning
- Module infrastructure
- Resource management
- PCI

---

## Phase 6 – Storage

Implement:

- Block device API
- Partition layer
- VFS
- Mounts
- Handles
- Filesystem
- NVMe or VirtIO block driver

**Milestone:** Userspace can access files.

---

## Phase 7 – Init and Userspace

Implement:

- ELF userspace loader
- Initramfs
- Init
- libc
- Shell
- Service manager

**Milestone:** JellyOS can boot into a functional command-line environment.

---

## Phase 8 – Networking

Implement:

- Network interface
- Ethernet
- ARP
- IPv4
- ICMP
- UDP
- TCP
- DNS
- Sockets

**Milestone:** JellyOS can access the Internet.

---

## Phase 9 – Graphics

Implement:

- GOP framebuffer
- Drawing
- Display abstraction
- Surfaces
- Windows
- Compositor
- Input routing
- GUI toolkit

**Milestone:** Graphical applications can run.

---

## Phase 10 – Desktop

Implement:

- Login/session
- Desktop shell
- Taskbar
- Launcher
- Window manager
- File manager
- Terminal
- Settings
- Notifications
- System status

**Milestone:** JellyOS has a usable graphical desktop.

---

## Phase 11 – Audio and Input

Implement:

- Keyboard
- Mouse
- Gamepad
- Audio API
- Audio server
- Mixer
- Recording

---

## Phase 12 – Advanced Hardware

Add:

- VirtIO
- NVMe
- USB
- Intel GPU
- AMD GPU
- NVIDIA GPU
- Ethernet
- Wi-Fi
- Bluetooth
- Audio devices

GPU support should be approached incrementally. NVIDIA will likely require substantially more engineering than simple framebuffer or basic device support.

---

## Phase 13 – SDK

Provide:

- Compiler toolchain
- Headers
- libc
- libos
- Graphics libraries
- Audio libraries
- Input libraries
- Network libraries
- Debugging tools
- Examples

Examples:

```text
Hello World
CLI Application
GUI Application
2D Application
Network Application
Simple Game
```

---

## Phase 14 – Gaming

Priorities:

- GPU acceleration
- Graphics API
- Gamepad support
- Audio
- Low-latency timers
- Efficient threads
- Fast filesystem streaming
- Networking
- Profiling

The goal is to make JellyOS capable of running native games rather than merely demonstrating a desktop.

---

## Phase 15 – Production

Implement:

- Package manager
- Permissions
- Users/groups
- Secure Boot
- Signed drivers
- Signed packages
- System updates
- Rollback
- Backup/recovery
- Crash reporting
- Localization
- Accessibility
- Power management
- Sleep
- Hibernate

---

# 53. Major Milestones

```text
M0  Architecture documented
M1  Bootloader starts kernel
M2  Kernel memory and interrupts
M3  Isolated processes
M4  Dynamic driver management
M5  Files accessible from userspace
M6  Shell
M7  Internet
M8  Graphical windows
M9  Desktop
M10 Audio and gamepads
M11 Third-party applications compile
M12 Simple native game
M13 Safe update and rollback
M14 Everyday-use suitable
```

---

# 54. Final Development Principle

The previous JellyOS implementation should be treated as a **reference implementation**, not as the foundation that must be preserved at all costs.

The new architecture should preserve:

- Lessons learned
- Working algorithms
- Useful drivers
- Proven hardware knowledge
- Debugging experience

But components should only be ported when they fit the new interfaces.

The objective is to build JellyOS so that future features can be added without requiring another complete architectural rewrite.

---

# 55. First Implementation Tasks

The first development tasks should be:

1. Create the JellyOS repository.
2. Create architecture documentation.
3. Define `boot_info_t`.
4. Define boot ABI versioning.
5. Define boot configuration.
6. Define boot flags.
7. Define boot states.
8. Define the kernel memory layout.
9. Define the initial virtual address layout.
10. Define the error model.
11. Define the handle model.
12. Define driver/device interfaces.
13. Define the VFS interface.
14. Define the initial syscall ABI.
15. Build the new UEFI bootloader.

Only after these contracts are sufficiently stable should the kernel implementation begin.

---

# JellyOS Architecture Goal

The final JellyOS should be a coherent operating system rather than a collection of working components.

The architecture should allow:

```text
Hardware
   ↓
Firmware / Boot Manager
   ↓
Kernel
   ↓
Drivers / Core Services
   ↓
System APIs
   ↓
Userspace Services
   ↓
Desktop / Applications
   ↓
User
```

Every layer should have a clear responsibility.

That is the foundation on which JellyOS can evolve from a personal operating-system project into a serious general-purpose desktop operating system.
