# StratumOS

An operating system kernel and bootloader written from scratch in C and assembly for x86 (with a portable RISC-V 64 port), transitioning a machine from the BIOS's first 16-bit real-mode instruction to a preemptively scheduled, higher-half, paged kernel with per-process address spaces, copy-on-write `fork`, `exec` from a FAT16 filesystem on disk, BSD sockets usable from ring 3, an e1000 TCP/IP stack with RFC 5681 congestion control, and a hardened kernel/user boundary enforced by PAE no-execute, SMEP, SMAP and `CR0.WP`.

[![CI](https://github.com/Saksham932007/StratumOS/actions/workflows/ci.yml/badge.svg)](https://github.com/Saksham932007/StratumOS/actions/workflows/ci.yml)
![language](https://img.shields.io/badge/C11%20%2B%20asm-50.0k%20lines-blue)
![arch](https://img.shields.io/badge/arch-i686%20%2B%20riscv64-lightgrey)
![tests](https://img.shields.io/badge/tests-1243%20assertions%2C%2015%20boot%20scenarios-brightgreen)
![license](https://img.shields.io/badge/license-MIT-green)

---

## Contents

1. [Key Achievements](#1-key-achievements)
2. [Boot Demonstration](#2-boot-demonstration)
3. [Architecture Diagram](#3-architecture-diagram)
4. [Feature Summary](#4-feature-summary)
5. [Quick Start](#5-quick-start)
6. [Testing and Benchmarking Highlights](#6-testing-and-benchmarking-highlights)
7. [Selected Engineering Deep Dives](#7-selected-engineering-deep-dives)
8. [Full Documentation Index](#8-full-documentation-index)

---

## 1. Key Achievements

| Subsystem / Area | Implementation Highlights | Verification & Evidence |
|---|---|---|
| **Dual Bootloader** | Custom two-stage MBR bootloader (`stage1.asm` in 315 bytes + `stage2.asm` with A20/E820/ELF32 loader) and Multiboot2/GRUB support in a **single ELF binary**. | CI boots both protocols on every push; uniform `struct boot_params` handoff. |
| **Memory Protection** | **3-level PAE paging** adopted specifically for bit 63 (hardware NX bit); `CR0.WP` read-only text protection; SMEP and SMAP ring-0 isolation; guard pages on task stacks. | True hardware $W\oplus X$ enforced across linear map; automated panics on `.text` writes (`fault text`) and execution of data pages (`fault nx`). |
| **Processes & Multitasking** | Preemptive round-robin scheduler (100 Hz); **proven copy-on-write `fork`**; `exec` by rewriting the syscall's trap frame; SMP bring-up (INIT-SIPI-SIPI) across 4 CPUs with local APIC timers. | Child writes to shared page and parent validates its copy remains unchanged; 4 CPU cores actively schedule tasks in QEMU. |
| **Storage & VFS** | ATA IDE driver (PIO, LBA28/LBA48); MBR partition table parsing; FAT16 driver with a **two-slot purpose-split sector cache**; Virtual File System (`devfs`, `fatfs`, `socket`). | `/bin/INIT` loaded from disk; cache hit rate elevated from 47% to 89% by preventing directory and data sector thrashing. |
| **Networking & BSD Sockets** | Intel 82540EM (e1000) PCI driver with DMA descriptor rings; ARP, IPv4, ICMP, UDP, and TCP with **RFC 5681 congestion control** and Jacobson RTO estimation; DHCP; DNS; HTTP client; BSD socket syscalls in ring 3. | Verified against raw packet capture (`.pcap`) with independent checksum validation; full socket loopback, UDP, and TCP echo in ring 3. |
| **Multi-Arch & Portability** | **RISC-V 64 port** on QEMU's `virt` board (M-mode $\rightarrow$ S-mode $\rightarrow$ Sv39 paging) linking shared core/net/fs files; on-demand **64-bit long mode round trip** from 32-bit protected mode. | 63% of non-arch kernel code compiles for RISC-V unmodified; port surfaced latent 32-bit pointer truncation in shared types. |
| **Hardening & Quality** | Defensive ELF32 loader; kernel-to-user pointer sanitization; rate-limited logging; 24 in-kernel test suites; 4 libFuzzer targets under ASan. | 1243 assertions across 15 QEMU boot scenarios; 40,000 malformed ring-3 syscalls survived; 6 pre-existing bugs uncovered by fuzzing. |

---

## 2. Boot Demonstration

The following serial capture demonstrates StratumOS booting from raw disk sectors, establishing PAE paging and hardware hardening, bringing up the e1000 network stack and FAT16 filesystem, dropping to ring 3, proving copy-on-write `fork()` and `exec()`, and communicating over the network via BSD sockets from user space:

```
StratumOS stage1
StratumOS stage2
  [ok] A20 gate
  [ok] BIOS memory map
  [..] kernel ............
  [ok] kernel image staged
  [->] entering protected mode

  .------------------------------------------------------.
  | StratumOS 0.13.0  -  x86 kernel: real mode to ring 3 |
  '------------------------------------------------------'
[    0.000] INFO  boot: serial COM1        [ok] 115200 8N1
[    0.000] INFO  boot: VGA text mode      [ok] 80x25
[    0.000] INFO  boot: CPU detect         [ok] AuthenticAMD
[    0.000] INFO  boot: boot protocol      [ok] StratumOS native (ELF handoff)
[    0.000] INFO  boot: GDT + TSS          [ok] flat 4 GiB, rings 0 and 3
[    0.000] INFO  boot: IDT                [ok] 256 vectors
[    0.000] INFO  boot: PIC remap          [ok] IRQ 0-15 -> vector 32-47
[    0.000] INFO  boot: PIT timer          [ok] 100 Hz
[    0.000] INFO  boot: interrupts         [ok] enabled
[    0.010] INFO  boot: physical memory    [ok] bitmap allocator (127 MiB RAM)
[    0.010] INFO  vmm: paging: PAE, kernel at 0xc0000000, linear map 16 MiB, identity map dropped
[    0.010] INFO  vmm: the kernel's gigabyte is one PDPT entry, so address spaces share it by pointer
[    0.020] INFO  boot: paging             [ok] 4 KiB pages, recursive PD
[    0.030] INFO  boot: kernel heap        [ok] first-fit, guarded, growable at 0xd0000000
[    0.030] INFO  vmm: kernel .text and .rodata mapped read-only (CR0.WP enforced on ring 0)
[    0.030] INFO  vmm: W^X: 4035 pages of the kernel's linear map marked no-execute
[    0.040] INFO  harden: SMEP enabled, SMAP enabled
[    0.040] INFO  boot: hardening          [ok] W^X (NX), guard pages, SMEP + SMAP
[    0.050] INFO  boot: PCI                [ok] legacy 0xCF8 enumeration
[    0.060] INFO  boot: ACPI               [ok] RSDT, 4 tables, 1 cpu(s)
[    0.060] INFO  boot: local APIC         [ok] enabled, IPIs available
[    0.120] INFO  boot: APIC timer         [ok] 100 Hz per processor, bus clock 991 MHz measured
[    0.140] INFO  e1000: 82540EM at 00:03.0, MMIO 0xfebc0000, IRQ 11, MAC 52:54:00:12:34:56
[    0.140] INFO  net: address 10.0.2.15 netmask 255.255.255.0 gateway 10.0.2.2
[    0.140] INFO  tcp: listening on port 7 (echo service)
[    0.150] INFO  boot: network            [ok] 52:54:00:12:34:56, 10.0.2.15, link up
[    0.150] INFO  ata: hd0: QEMU HARDDISK, 36864 sectors (18 MiB), LBA48
[    0.160] INFO  fat: mounted hd0p1: FAT16 "STRATUM", 16384 KiB, 8167 clusters of 2 KiB
[    0.160] INFO  boot: filesystem         [ok] FAT16 "STRATUM" on hd0p1
[    0.160] INFO  boot: vfs                [ok] devfs + fat16 + sockets
[    0.170] INFO  boot: scheduler          [ok] round robin, 100 Hz preemption
[    0.170] INFO  boot: syscalls           [ok] int 0x80 (11 calls available)
[    0.180] INFO  boot: StratumOS 0.13.0 is up: 127 MiB RAM, 6 PCI devices, 24 test suites

[    0.190] INFO  user: pid 5 entering ring 3 at 0x00400250 in its own address space ("init" from disk)
  [ring3] hello from user mode - privilege level 3
  [ring3] I am a separate ELF, loaded into my own pages
  [ring3] getpid() returned 5
  [ring3] slept 50 ms via syscall
  [ring3] asking the kernel to read a kernel address on my behalf
[    0.260] WARN  syscall: pid 5 passed an unreadable buffer 0xc0100000+16
  [ring3] kernel refused it (EFAULT) - the pointer check works
  [ring3] unknown syscall correctly rejected
  [ring3] fork(): duplicating this process
[    0.270] INFO  sched: fork: pid 5 -> pid 7, address space 0x002f7000
  [ring3] fork() returned 7 here - one call, two return values
    [child] fork() returned 0 here; my pid is 7, my parent is 5
    [child] I inherited 0x5a5a5a5a and am about to write over it
    [child] my copy now reads 0x1234abcd
    [child] exiting with 7
  [ring3] wait() collected pid 7 with exit code 7
  [ring3] my own copy still reads 0x5a5a5a5a - copy-on-write gave the child a private page
  [ring3] exec(): forking a child to replace its own image
[    0.290] INFO  sched: fork: pid 5 -> pid 8, address space 0x002f6000
    [child] exec("nonexistent") failed cleanly and I am still here
[    0.300] INFO  user: pid 8 exec("hello") from the filesystem: replacing 7 user pages
[    0.300] INFO  user: pid 8 now running "hello" at 0x00400000, 6 user pages
  [exec] hello: a different image, running in the same process
  [exec] getpid() returned 8 - the pid survived exec, the image did not
  [exec] the rebuilt address space still refuses kernel and unmapped pointers
  [exec] calling exit(0)
  [ring3] the exec'd child (pid 8) exited with 0

  [ring3] network: opening a BSD socket from ring 3
  [ring3] network: sending UDP datagram to echo port (10.0.2.15:7)
  [ring3] network: echo reply received successfully: "hello from user mode"
  [ring3] calling exit(0)
```

---

## 3. Architecture Diagram

```
+===================================================================================+
|                                    BOOTSTRAP                                      |
|                                                                                   |
|  BIOS -> Stage 1 (MBR 512B) -> Stage 2 (A20, E820, ELF Loader) -\                 |
|                                                                 +-> _start        |
|  GRUB -> Multiboot2 Header -------------------------------------/   (Higher-Half) |
+===================================================================================+
                                         |
                                         v
+===================================================================================+
|                                   KERNEL SPACE                                    |
|                                                                                   |
|  [Hardware & MMU]      PAE Paging (3 levels) | W^X (NX bit 63) | CR0.WP           |
|                        SMEP & SMAP Isolation | Guarded Kernel Heap (0xD0000000)   |
|                        Physical Frame Allocator (Bitmap, 4 KiB frames)            |
|                                                                                   |
|  [Core & Scheduling]   Preemptive Round-Robin (100 Hz) | Per-CPU APIC Timers      |
|                        Multi-Core SMP (INIT-SIPI-SIPI) | Real Spinlocks & IPIs    |
|                        Per-Process Address Spaces (CR3 switched on ctxsw)         |
|                                                                                   |
|  [VFS & Storage]       VFS Layer (/dev, /, sockets) | FAT16 Purpose-Split Cache   |
|                        MBR Partition Table Parser   | ATA IDE PIO (LBA28/LBA48)   |
|                                                                                   |
|  [Network Stack]       Intel e1000 Driver (DMA descriptor rings)                  |
|                        ARP Cache | IPv4 Routing | ICMP Echo | UDP                 |
|                        TCP (RFC 5681 Congestion Control, Jacobson RTO)            |
|                        DHCP Client | DNS Resolver | HTTP/1.1 Client               |
+===================================================================================+
                                         |
                       Syscall Boundary (`int 0x80`)
                                         |
                                         v
+===================================================================================+
|                                    USER SPACE                                     |
|                                                                                   |
|  Ring 3 Execution   -> Forged IRET Frame | Pointer Validation (EFAULT)            |
|  Process Control    -> Copy-on-Write fork() | Trap-Frame Rewriting exec()         |
|  VFS File & Sockets -> open(), read(), write(), close(), fstat(), lseek()         |
|                        socket(), bind(), listen(), accept(), connect(), sendto()  |
|  Binaries on Disk   -> /bin/INIT, /bin/HELLO, /bin/NETCAT, /bin/FUZZ              |
+===================================================================================+
                                         |
        +--------------------------------+--------------------------------+
        |                                                                 |
        v                                                                 v
+==================================+             +==================================+
|     64-BIT LONG MODE TRANSITION  |             |      RISC-V 64 ARCHITECTURE      |
|                                  |             |                                  |
| 32-bit Protected -> 64-bit Long  |             | Machine Mode -> Supervisor Mode  |
| 4-Level Paging (PML4) -> Back    |             | Sv39 Paging -> Shared C Code     |
+==================================+             +==================================+
```

---

## 4. Feature Summary

### Kernel Core & Memory Protection
- **PAE 3-Level Paging**: 64-bit page table entries granting hardware No-Execute (bit 63) protection across all non-code memory.
- **Hardware-Enforced $W\oplus X$**: `.text` and `.rodata` mapped read-only (`CR0.WP`); all data/stack/heap pages marked non-executable.
- **Kernel/User Boundary**: Ring-0 access to user space blocked by SMAP; execution of user space blocked by SMEP.
- **Shared Kernel Page Directory**: The top 1 GiB virtual address space corresponds to a single PDPT entry (`index 3`), shared by pointer across all processes.
- **Physical Memory Manager**: Bitmap allocator tracking 4 KiB frames dynamically parsed from the BIOS E820 memory map.
- **Guarded Kernel Heap**: First-fit memory allocator with boundary-tag coalescing, head/tail magic canaries, and automatic virtual expansion.

### Processes & Multitasking
- **Preemptive Round-Robin**: 100 Hz timer preemption driven by per-processor local APIC timers.
- **True Copy-on-Write**: Software PTE tracking with physical frame reference counts; shared pages duplicated only upon write fault.
- **Defensive `exec()`**: Rewrites the syscall trap frame in place to jump to the new binary upon `iret`; validates user bounds without unmapping until confirmed.
- **Multi-Core SMP**: ACPI MADT enumeration, APIC timer calibration against the PIT, 176-byte real-mode AP trampoline, IPI ping, and TLB shootdown.

### Storage & Virtual File System
- **ATA IDE Driver**: PIO modes LBA28 and LBA48 with bounded status polling and ATAPI detection.
- **FAT16 Filesystem**: Full cluster chain traversal, 8.3 filename parsing, directory creation, file reads/writes.
- **Purpose-Split Cache**: Dedicated sectors for FAT structures and file data, achieving an 89% hit rate under sequential chunked reads.
- **Virtual File System (VFS)**: Uniform file descriptor table (0 to 31 per process) dispatching across `devfs` (`/dev/console`, `/dev/null`, `/dev/zero`), `fatfs` (`/`), and BSD sockets.

### Network Protocol Stack & Sockets
- **e1000 Driver**: Intel 82540EM Gigabit Ethernet controller operated through memory-mapped I/O and circular DMA descriptor rings.
- **Protocols**: Ethernet framing, ARP table with timeouts, IPv4 with header verification and checksum calculation, ICMP ping echo.
- **Transport Layer**: UDP with pseudo-header validation; TCP state machine with RFC 5681 slow start, congestion avoidance, fast retransmit, and Jacobson's RTO estimator.
- **Application Services**: DHCP client, DNS stub resolver, HTTP/1.1 client with chunked encoding decoding.
- **BSD Sockets**: `socket()`, `bind()`, `listen()`, `accept()`, `connect()`, `send()`, `recv()`, `sendto()`, `recvfrom()`, and `shutdown()`.

### Multi-Architecture & Verification
- **RISC-V 64**: Boots on QEMU `virt` machine mode, transitions via `mret` to supervisor mode, enables Sv39 paging, and links shared core/string/net sources.
- **x86-64 Transition**: Switches dynamically from 32-bit protected mode into 64-bit long mode on 4-level PML4 tables and safely returns to 32-bit execution.
- **Dual Boot Protocol**: One ELF binary bootable by custom MBR bootloader and Multiboot2/GRUB.

---

## 5. Quick Start

### Prerequisites

```bash
# Ubuntu / Debian
sudo apt install build-essential nasm gcc-multilib libc6-dev-i386 \
                 qemu-system-x86 grub-pc-bin grub-common xorriso mtools

# Fedora
sudo dnf install gcc glibc-devel.i686 nasm qemu-system-x86 grub2-tools-extra xorriso mtools

# Arch Linux
sudo pacman -S base-devel nasm lib32-glibc qemu-system-x86 grub xorriso mtools
```

*Optional (for RISC-V 64 build and libFuzzer suites)*:
```bash
sudo apt install clang lld llvm qemu-system-misc
```

### Build and Run

```bash
# Clone the repository
git clone https://github.com/Saksham932007/StratumOS.git
cd StratumOS

# Build kernel, disk image, and user binaries
make

# Boot via the custom two-stage bootloader in QEMU
make run

# Boot headless over serial console (interactive shell)
make run-serial

# Boot with an attached Intel e1000 network controller (captures net.pcap)
make run-net

# Run all 15 automated QEMU test scenarios and host unit tests
make test
```

### Second Architecture (RISC-V 64)

```bash
# Build and run the RISC-V 64 kernel (Sv39 paging on QEMU virt board)
make ARCH=riscv64
make ARCH=riscv64 run
make ARCH=riscv64 test
```

### Key Makefile Targets

| Target | Description |
|---|---|
| `make` | Builds `build/x86/stratum.elf`, disk images, and user binaries (`ARCH=x86` default). |
| `make run` | Boots the custom MBR two-stage bootloader in QEMU. |
| `make run-iso` | Boots the kernel via GRUB Multiboot2 from an ISO. |
| `make run-serial` | Headless execution with console redirected to stdio. |
| `make run-net` | Boots with an Intel e1000 controller and writes traffic to `build/x86/net.pcap`. |
| `make run-x86-64` | Executes 32-bit image on a 64-bit CPU model to test the `longmode` transition. |
| `make test` | Executes host unit tests and all 15 automated QEMU boot scenarios. |
| `make bench` | Runs microbenchmarks and statistical sampling profiler against TSC. |
| `make fuzz` | Runs 4 libFuzzer targets under ASan over unmodified kernel sources. |
| `make portability` | Measures compilation coverage of shared code on RISC-V 64. |
| `make lines` | Generates detailed line-count breakdown by subsystem. |

---

## 6. Testing and Benchmarking Highlights

StratumOS enforces verification across 5 distinct testing tiers:

| Tier | Scope | Coverage & Invariants |
|---|---|---|
| **Host Unit Tests** | `tests/host/` | Validates `printf`, `string`, and `div64` implementations compiled for host against `glibc` behavior (92 assertions). |
| **Pre-Boot Validation** | `tools/check-kernel.py` | 20 build-time assertions verifying Multiboot2 headers, higher-half address splits, ELF segment bounds, and absence of SSE instructions. |
| **Image Validation** | `tools/check-image.py` | Validates MBR partition entries, stage 2 pointer bounds, and FAT16 BPB structures before boot. |
| **In-Kernel Test Suites** | `kernel/core/ktest.c` | **24 suites running 1243 assertions** directly against hardware state (PMM, VMM, COW, SMP, e1000, VFS, sockets, scheduler). |
| **QEMU Boot Scenarios** | `tools/run-tests.py` | **15 distinct scenarios**: dual boot paths, SMP across 4 CPUs, SMEP/SMAP, legacy CPU with no NX bit, no-network fallback, raw `.pcap` validation, and interactive serial shell. |
| **Deliberate Panics** | Scenarios required to fail | Verifies `fault text`, `fault nx`, and `fault stackguard` crash ring 0 with expected exception vectors and exit code `35`. |
| **Fuzzing Campaigns** | libFuzzer + ring-3 syscall fuzzer | 4 libFuzzer targets over unmodified source files under ASan; 40,000 malformed ring-3 syscalls survived without kernel panic. |

### Microbenchmarks

Calibrated against the 8254 PIT via the CPU TSC under QEMU TCG at 2100 MHz (median of 24 samples, harness overhead subtracted):

| Operation | Emulated Cycles | Emulated Latency | Description |
|---|---:|---:|---|
| Virtual $\rightarrow$ Physical Translation | 55 | 26.3 ns | Recursive PAE page table walk |
| Heap Canary Integrity Check | 58 | 27.8 ns | Header/footer boundary tag validation |
| Process Context Switch | 654 | 311.3 ns | `CR3` swap, TSS stack reload, canary check |
| Physical Frame Alloc + Free | 733 | 349.2 ns | Bitmap scan and toggle |
| `kmalloc(64)` + `kfree` | 962 | 458.0 ns | First-fit search, split, and coalesce |
| Map + Unmap 4 KiB Page | 1054 | 502.1 ns | Page directory manipulation and `invlpg` |
| Ring-3 Syscall (`int 0x80`) Roundtrip | 1488 | 708.6 ns | User $\rightarrow$ Kernel $\rightarrow$ User privilege change |
| `ksnprintf` Formatting | 2666 | 1269.3 ns | String formatting with 64-bit integer division |
| 4 KiB `memcpy` | 6767 | 3222.2 ns | 1.65 cycles/byte |

---

## 7. Selected Engineering Deep Dives

StratumOS documents the reasoning, design trade-offs, and debugging discoveries behind its subsystems. The full technical narratives are maintained in [docs/ENGINEERING-DETAILS.md](docs/ENGINEERING-DETAILS.md) and individual subsystem documents:

1. **Copy-on-Write Proved, Not Claimed**  
   Marking only the child read-only during `fork()` is the classic error that leads to quiet data corruption. StratumOS marks *both* parent and child read-only with a software COW bit, verifying private page allocation when the child mutates memory.  
   $\rightarrow$ [docs/PROCESSES.md](docs/PROCESSES.md) · [Deep Dive Details](docs/ENGINEERING-DETAILS.md#copy-on-write-proved-not-claimed)

2. **PAE for One Bit: Hardware W^X**  
   Standard 32-bit x86 page tables lack an execute-disable bit. StratumOS implements 3-level PAE paging to obtain bit 63 (XD/NX), mapping all data, stack, and heap memory non-executable by default.  
   $\rightarrow$ [docs/SECURITY.md](docs/SECURITY.md) · [Deep Dive Details](docs/ENGINEERING-DETAILS.md#pae-for-one-bit-hardware-wx)

3. **Network Stack Verified Against Raw Packet Captures**  
   An early driver bug sent frames with a source MAC of `00:00:00:00:00:00` while logging the correct address, passing every in-kernel test while failing silently on the wire. CI now parses raw `.pcap` files and independently re-verifies packet checksums.  
   $\rightarrow$ [docs/NETWORK.md](docs/NETWORK.md) · [Deep Dive Details](docs/ENGINEERING-DETAILS.md#network-verification-against-raw-packet-captures)

4. **RISC-V 64 Port: Finding Latent 32-bit Assumptions**  
   Porting the kernel core to RISC-V 64 revealed 88 build errors stemming from `paddr_t` and `vaddr_t` defined as `u32`, and decoupled memory management from x86 port I/O headers.  
   $\rightarrow$ [docs/PORTING.md](docs/PORTING.md) · [Deep Dive Details](docs/ENGINEERING-DETAILS.md#risc-v-64-port-uncovering-latent-32-bit-assumptions)

5. **Fuzzing Unmodified Kernel Sources with AddressSanitizer**  
   Using `mmap(MAP_FIXED_NOREPLACE)` in an address space emulator, the real `elf.c` and `fat16.c` files are fuzzed without modification, uncovering 6 pre-existing vulnerabilities including an unprivileged panic.  
   $\rightarrow$ [docs/FUZZING.md](docs/FUZZING.md) · [Deep Dive Details](docs/ENGINEERING-DETAILS.md#fuzzing-unmodified-kernel-sources)

6. **Purpose-Split FAT Sector Cache (47% $\rightarrow$ 89% Hit Rate)**  
   Sequential file reads thrash when directory entries, FAT cluster tables, and data sectors compete for a single cache slot. Splitting into dedicated FAT and data slots nearly doubled read efficiency.  
   $\rightarrow$ [docs/STORAGE.md](docs/STORAGE.md) · [Deep Dive Details](docs/ENGINEERING-DETAILS.md#storage-cache-optimization-from-47-to-89-hit-rate)

7. **32-bit to 64-bit Long Mode Round Trip**  
   Transitions the processor into 64-bit long mode on 4-level PML4 tables, executes 64-bit exclusive instructions, and returns cleanly to 32-bit protected mode.  
   $\rightarrow$ [docs/LONGMODE.md](docs/LONGMODE.md) · [Deep Dive Details](docs/ENGINEERING-DETAILS.md#64-bit-long-mode-round-trip)

8. **Automated Scenarios Required to Panic**  
   A security mitigation has two halves: normal execution and the trap on violation. StratumOS automates CI scenarios that intentionally violate `.text` read-only status and stack guard pages, requiring a kernel panic and QEMU exit code `35`.  
   $\rightarrow$ [docs/TESTING.md](docs/TESTING.md) · [Deep Dive Details](docs/ENGINEERING-DETAILS.md#scenarios-required-to-panic)

---

## 8. Full Documentation Index

| Document | Focus & Contents |
|---|---|
| [docs/ENGINEERING-DETAILS.md](docs/ENGINEERING-DETAILS.md) | **Complete engineering archive**: detailed architectural deep dives, shell diagnostic transcripts, benchmark analyses, porting lessons, and full defect histories. |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Subsystem topology, dependencies, and initialization order constraints. |
| [docs/BOOT.md](docs/BOOT.md) | Stage 1, Stage 2, A20 gate, E820 memory mapping, and Multiboot2 entry mechanics. |
| [docs/MEMORY.md](docs/MEMORY.md) | Address space layout, higher-half split, recursive page directory, PMM, and heap. |
| [docs/SECURITY.md](docs/SECURITY.md) | Hardware $W\oplus X$, PAE bit 63, SMEP/SMAP, guard pages, and kernel/user isolation. |
| [docs/PROCESSES.md](docs/PROCESSES.md) | Address space cloning, copy-on-write refcounts, `exec()` trap frame rewrites, and zombies. |
| [docs/USERSPACE.md](docs/USERSPACE.md) | Ring-3 privilege transitions, ELF loader defensive checks, and syscall boundary. |
| [docs/STORAGE.md](docs/STORAGE.md) | ATA PIO driver, MBR partitioning, FAT16 filesystem implementation, and caching. |
| [docs/NETWORK.md](docs/NETWORK.md) | Intel e1000 DMA descriptor rings, ARP, IPv4, UDP, TCP congestion control, and sockets. |
| [docs/SMP.md](docs/SMP.md) | ACPI MADT parsing, APIC timer calibration, 176-byte AP trampoline, spinlocks, and IPIs. |
| [docs/PORTING.md](docs/PORTING.md) | RISC-V 64 port (M-mode $\rightarrow$ S-mode $\rightarrow$ Sv39), portability measurement, and shared code. |
| [docs/LONGMODE.md](docs/LONGMODE.md) | 32-bit $\rightarrow$ 64-bit long mode round trip, 4-level paging, and compatibility modes. |
| [docs/PERFORMANCE.md](docs/PERFORMANCE.md) | PIT/TSC calibration methodology, benchmark results, and sampling profiler. |
| [docs/TESTING.md](docs/TESTING.md) | 5 testing layers, 15 QEMU scenarios, deliberate panic tests, and CI automation. |
| [docs/FUZZING.md](docs/FUZZING.md) | libFuzzer shims over unmodified kernel source files, ASan coverage, and bugs found. |
| [docs/DEBUGGING.md](docs/DEBUGGING.md) | GDB attachment, panic decoding, embedded ksyms resolver, and symptom guides. |
| [docs/DESIGN-DECISIONS.md](docs/DESIGN-DECISIONS.md) | Recorded architectural trade-offs and rejected alternatives. |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Future planned enhancements, including DMA disk transfers, KASLR, and serial IRQ. |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Code styling, build requirements, and test contribution guidelines. |

---

## License

StratumOS is licensed under the [MIT License](LICENSE).
