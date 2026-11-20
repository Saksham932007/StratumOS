# StratumOS — Engineering Deep Dives & Architecture Details

This document preserves the comprehensive engineering explanations, design justifications, benchmark analyses, debugging histories, and technical post-mortems of StratumOS. For an overview and quick start, see the main [README.md](../README.md).

---

## Contents

1. [Core Architectural Deep Dives](#1-core-architectural-deep-dives)
   - [Dual Boot Protocols, Single Binary](#dual-boot-protocols-single-binary)
   - [Copy-on-Write: Proved, Not Claimed](#copy-on-write-proved-not-claimed)
   - [exec Rewriting its Own Trap Frame](#exec-rewriting-its-own-trap-frame)
   - [PAE for One Bit: Hardware W^X](#pae-for-one-bit-hardware-wx)
   - [SMEP and SMAP Enforcement](#smep-and-smap-enforcement)
   - [Storage Cache Optimization: From 47% to 89% Hit Rate](#storage-cache-optimization-from-47-to-89-hit-rate)
   - [RISC-V 64 Port: Uncovering Latent 32-bit Assumptions](#risc-v-64-port-uncovering-latent-32-bit-assumptions)
   - [Network Verification Against Raw Packet Captures](#network-verification-against-raw-packet-captures)
   - [64-bit Long Mode Round Trip](#64-bit-long-mode-round-trip)
   - [Fuzzing Unmodified Kernel Sources](#fuzzing-unmodified-kernel-sources)
   - [Scenarios Required to Panic](#scenarios-required-to-panic)
2. [What Happens at Boot (Instruction by Instruction)](#2-what-happens-at-boot)
3. [Full Interactive Shell & Diagnostic Session](#3-full-interactive-shell--diagnostic-session)
4. [Performance & Microbenchmarks Analysis](#4-performance--microbenchmarks-analysis)
5. [Two Architectures & Portability Methodology](#5-two-architectures--portability-methodology)
6. [Networking Architecture, Sockets, and Congestion Control](#6-networking-architecture-sockets-and-congestion-control)
7. [64-bit Long Mode Technical Mechanics](#7-64-bit-long-mode-technical-mechanics)
8. [Testing Methodology & Automation Details](#8-testing-methodology--automation-details)
9. [Fuzzing Harnesses & Bugs Found](#9-fuzzing-harnesses--bugs-found)
10. [Complete Repository Layout](#10-complete-repository-layout)
11. [Design Decisions Worth Defending](#11-design-decisions-worth-defending)
12. [Bugs Found and Fixed](#12-bugs-found-and-fixed)
13. [Deliberate Scope Omissions](#13-deliberate-scope-omissions)

---

## 1. Core Architectural Deep Dives

### Dual Boot Protocols, Single Binary

`build/x86/stratum.elf` is a single file that can be loaded in two completely different ways:
- **Custom 2-stage bootloader**: The MBR (`boot/stage1.asm`) loads `boot/stage2.asm`. Stage 2 parses the ELF32 program headers directly off raw disk sectors, copies segments to physical memory, enters 32-bit protected mode, and jumps to `_start`.
- **GRUB via Multiboot2**: GRUB reads the Multiboot2 header embedded in `kernel/arch/x86/boot.asm`, allocates memory, and jumps to `_start`.

The kernel normalizes these paths in `kernel/core/bootinfo.c`:
```c
switch (magic) {
case STRATUM_BOOT_MAGIC:          /* 0x53545241, custom stage 2 */
    ok = parse_stratum(info_addr);
    break;
case MULTIBOOT2_BOOTLOADER_MAGIC: /* 0x36d76289, GRUB        */
    ok = parse_multiboot2(info_addr);
    break;
}
```
All downstream kernel subsystems consume a uniform `struct boot_params`. CI boots both paths on every push, preventing either protocol from bit-rotting.
→ Detailed breakdown: [docs/BOOT.md](BOOT.md)

### Copy-on-Write: Proved, Not Claimed

`fork()` marks every writable page read-only in **both** parent and child address spaces, setting a software COW bit in the page table entries and incrementing a per-frame reference count. 

Marking only the child is the subtle classic error: if the parent writes to its page before the child does, the parent silently mutates data that the child expects to inherit. In StratumOS, when either process writes to a shared page:
1. A page fault (vector 14) triggers with error code `bit 1 = 1` (write violation).
2. The VMM fault handler inspects the PTE; seeing `PTE_COW` and a reference count $> 1$, it allocates a new physical frame, copies the 4 KiB content, decrements the old frame's reference count, and maps the new frame as writable.
3. If the reference count is already 1 (the other process already copied or exited), the page is simply upgraded back to writable with no allocation.

The ring-3 demo program explicitly verifies this: the child writes `0x1234abcd` over an inherited `0x5a5a5a5a` variable. The parent reaps the child and asserts that its own variable remains `0x5a5a5a5a`.
→ Detailed breakdown: [docs/PROCESSES.md](PROCESSES.md)

### exec Rewriting its Own Trap Frame

When a process invokes `sys_exec("hello")`, it cannot return through the standard function epilogue: the image that initiated the `int 0x80` syscall has been unmapped from user space. 

Instead, the syscall implementation locates the CPU's interrupt trap frame (`struct trap_frame`) stored on the task's kernel stack. It rewrites `trap_frame->eip` to the new ELF entry point (`0x00400000`) and resets `trap_frame->esp` to `USER_STACK_TOP`. When the kernel's common ISR exit code executes `iret`, the processor pops the forged frame and transitions directly into the entry point of the new binary in user mode.
→ Detailed breakdown: [docs/PROCESSES.md](PROCESSES.md#exec)

### PAE for One Bit: Hardware W^X

In classic 32-bit x86 two-level paging, page table entries are 32 bits wide. There is no "No-Execute" (NX) bit—a page is either supervisor/user and read-only/writable, but any readable page is executable by hardware. Thus, `W^X` on 32-bit x86 is ordinarily an un-enforced aspiration.

StratumOS implements Physical Address Extension (PAE) specifically to obtain **bit 63 (XD/NX)**:
- Three levels: Page Directory Pointer Table (PDPT) with 4 entries of 64 bits $\rightarrow$ Page Directories (512 entries of 64 bits) $\rightarrow$ Page Tables (512 entries of 64 bits).
- Enabled via `CR4.PAE = 1` and `IA32_EFER.NXE = 1` before enabling `CR0.PG`.
- Flag discipline: Callers specify `PTE_EXEC`, not `PTE_NX`. Mappings are non-executable by default, making exceptions explicit and greppable.
- Architectural dividend: The top gigabyte (kernel space) corresponds to exactly one PDPT entry (`index 3`). Therefore, every process address space shares the kernel's page directory **by pointer** rather than copying 256 individual page directory entries. Post-boot kernel mappings automatically reflect across all processes.
→ Detailed breakdown: [docs/SECURITY.md](SECURITY.md#and-the-other-half-nx)

### SMEP and SMAP Enforcement

- **SMEP (Supervisor Mode Execution Prevention)**: `CR4.SMEP = 1`. Prevents the CPU in ring 0 from executing instructions located in user-accessible pages, defeating return-to-user (ret2usr) exploits.
- **SMAP (Supervisor Mode Access Prevention)**: `CR4.SMAP = 1`. Prevents ring 0 from reading or writing user memory unless `EFLAGS.AC` is explicitly opened.

StratumOS wraps every legitimate user-memory access (`copy_to_user`, `copy_from_user`, `user_copy_string`) in explicit `stac` and `clac` instructions. On its initial boot with SMAP, the kernel immediately caught a fault in its own test suite—which wrote to user pages during COW verification without setting `stac`.
→ Detailed breakdown: [docs/SECURITY.md](SECURITY.md#smep-and-smap)

### Storage Cache Optimization: From 47% to 89% Hit Rate

The original FAT16 driver utilized a single-sector LRU cache. A microbenchmark reading a file in 64-byte chunks (8 reads per 512-byte sector, expecting a ~87.5% hit rate) achieved only a 47% hit rate.

Profiling revealed that FAT table lookups (checking cluster chains) and data sector reads were constantly evicting each other within the same cluster. By splitting the cache into two dedicated single-sector slots—one for FAT metadata and one for file data—the hit rate increased from **47% to 89%** with negligible memory overhead.
→ Detailed breakdown: [docs/STORAGE.md](STORAGE.md#the-cache-and-what-measuring-it-changed)

### RISC-V 64 Port: Uncovering Latent 32-bit Assumptions

StratumOS boots on RISC-V 64 (`qemu-system-riscv64 -M virt`):
- Initializes M-mode reset vector, configures CLINT timer, delegates traps, and drops to S-mode via `mret`.
- Configures 3-level Sv39 paging.
- Links the core formatter (`printf.c`), strings (`string.c`), 64-bit division (`div64.c`), and logger (`log.c`) directly from the shared x86 source files.

**The Portability Findings**:
Compiling the core kernel for RISC-V revealed 88 build errors across 7 files with one uniform root cause:
```c
error: cast to 'void *' from smaller integer type 'u32' (aka 'unsigned int')
```
`paddr_t` and `vaddr_t` had been defined as `typedef u32`. This worked silently on i686 but truncated pointers on 64-bit architectures. Switching them to `uintptr_t` resolved all 88 failures.

Additionally, `mm/heap.c` failed on its include line: it had imported `<arch/io.h>` (x86 port I/O) solely to obtain `cli()` and `sti()`. RISC-V has memory-mapped I/O and no port I/O. Creating `<kernel/irqflags.h>` decoupled architecture-independent code from x86-specific concepts.
→ Detailed breakdown: [docs/PORTING.md](PORTING.md)

### Network Verification Against Raw Packet Captures

The Intel e1000 driver operates using DMA descriptor rings (32 receive and 32 transmit descriptors) mapped uncached. 

During initial development, the driver transmitted frames with a source MAC of `00:00:00:00:00:00`. The boot log reported the correct MAC because it printed the controller's register state, while `eth_output()` pulled from an uninitialized struct. Transmission counters incremented and the host replied, but the e1000 receive filter silently dropped replies directed to `00:00:00:00:00:00`. Every in-kernel assertion passed, and the serial log appeared flawless.

To eliminate this class of silent failure, CI dumps all network traffic to a `.pcap` capture and parses raw Ethernet frames with an independent Python script:
- Recomputes IPv4 and ICMP one's-complement checksums from scratch.
- Asserts that no transmitted frame contains a zero MAC source address.
- Asserts that ARP, ICMP echo, and UDP roundtrips complete over the wire.
→ Detailed breakdown: [docs/NETWORK.md](NETWORK.md)

### 64-bit Long Mode Round Trip

The kernel demonstrates a complete 32-bit $\rightarrow$ 64-bit $\rightarrow$ 32-bit mode transition:
1. `CR0.PG` is disabled (as `CR4.PAE` cannot be toggled while paging is active).
2. The trampoline copies itself to low physical memory (`0x9000`), identity-mapped, avoiding virtual address collapse.
3. 4-level paging (PML4 $\rightarrow$ PDPT $\rightarrow$ PD $\rightarrow$ PT) is loaded into `CR3`.
4. `IA32_EFER.LME` is set and `CR0.PG` is re-enabled, entering IA-32e compatibility mode.
5. A far jump to a code descriptor with the **L** bit (bit 21) set enters true 64-bit mode.
6. The 64-bit payload executes instructions impossible in 32-bit mode: 64-bit immediate loads, 64-bit additions exceeding $2^{32}$, accessing register `r15`, RIP-relative addressing, and reading memory via a 64-bit pointer above 4 GiB.
7. It far-jumps back to a 32-bit compatibility descriptor, reloads the 3-level PAE tables, and restores the 32-bit kernel without disruption.
→ Detailed breakdown: [docs/LONGMODE.md](LONGMODE.md)

### Fuzzing Unmodified Kernel Sources

Rather than extracting standalone copies of parsers for host fuzzing (which risks testing a divergent rewrite), StratumOS links the **exact kernel source files** (`kernel/core/elf.c`, `kernel/fs/fat16.c`, `kernel/mm/heap.c`, `kernel/arch/x86/acpi.c`) against a testing shim (`tests/fuzz/shim.c`) under AddressSanitizer (ASan):
- `vmm_alloc_at()` is implemented using `mmap(MAP_FIXED_NOREPLACE)` in a 32-bit host process (`-m32`), mapping the memory exactly where the ELF header requested.
- Out-of-bounds writes trigger immediate `SIGSEGV` via ASan.
- This campaign uncovered six pre-existing bugs, including an unprivileged DoS where an ELF with an entry point outside mapped segments caused a kernel panic instead of returning `SYS_ENOEXEC`.
→ Detailed breakdown: [docs/FUZZING.md](FUZZING.md)

### Scenarios Required to Panic

Testing only paths that succeed leaves security checks unverified. StratumOS maintains three automated CI test cases that are explicitly required to panic:
1. `fault text`: Writes to the kernel's own `.text` segment. Must trigger vector 14, identify the region as read-only kernel code, and exit QEMU with code `35`.
2. `fault nx`: Executes code on a heap/data page. Must trigger vector 14 with `bit 4 = 1` (instruction fetch violation).
3. `fault stackguard`: Pushes data past the bottom of a task stack into the unmapped guard page.
→ Detailed breakdown: [docs/TESTING.md](TESTING.md#scenarios-that-must-panic)

---

## 2. What Happens at Boot

From power-on to user space:

1. **BIOS**: Loads 512-byte sector 0 to `0x7C00` and jumps to it.
2. **Stage 1 (`boot/stage1.asm`)**: Probes INT 13h extensions; reads Stage 2 via LBA (falling back to CHS with controller resets); verifies magic number `0x53545241`.
3. **Stage 2 (`boot/stage2.asm`)**:
   - Enables A20 gate (testing BIOS, 8042 keyboard controller, and fast port 0x92 with memory wraparound verification).
   - Retrieves physical memory map via INT 15h, E820h.
   - Reads the kernel ELF from disk into scratch physical memory.
   - Sets up a flat 4 GiB GDT, sets `CR0.PE` to enter 32-bit protected mode.
   - Parses ELF32 program headers, copies loadable segments to physical addresses, and zeroes `.bss`.
4. **`_start` (`kernel/arch/x86/boot.asm`)**:
   - Sets up early page directory mapping low physical memory to `0xC0000000`.
   - Enables `CR4.PAE`, sets `CR0.PG`, and performs an absolute jump into higher-half virtual space (`0xC0100000`).
5. **Kernel Core (`kernel/core/kmain.c`)**:
   - Normalizes boot parameters from `EAX` (Stage 2 or Multiboot2).
   - Initializes COM1 serial port (115200 8N1) and VGA text console.
   - Configures IDT with 256 vector stubs; remaps 8259 PIC to IRQ 32–47.
   - Sets 8254 PIT timer to 100 Hz; unmasks interrupts (`sti()`).
   - Builds bitmap Physical Memory Manager (PMM) from E820 map.
   - Establishes Virtual Memory Manager (VMM), drops boot identity map, maps linear memory window, and activates PAE NX bits.
   - Allocates guarded kernel heap at `0xD0000000`.
   - Protects kernel `.text` and `.rodata` as read-only via `CR0.WP`.
   - Enables SMEP and SMAP via `CR4`.
   - Initializes ACPI (RSDP, RSDT/XSDT, MADT); sets up local APIC timer calibrated against PIT.
   - Brings up secondary SMP application processors using real-mode trampoline (INIT-SIPI-SIPI).
   - Probes PCI bus; initializes Intel 82540EM (e1000) Gigabit Ethernet controller; configures TCP/IP stack (10.0.2.15).
   - Probes ATA IDE disk controller; parses MBR partition table; mounts FAT16 filesystem at `/`.
   - Initializes VFS, registering `devfs` (`/dev/console`, `/dev/null`, `/dev/zero`), `fatfs`, and `socket`.
   - Starts round-robin preemptive scheduler (`sched_init`).
   - Installs system call gate at `int 0x80`.
6. **Ring 3 Entry**:
   - Loads `/bin/INIT` from the mounted FAT16 disk into a new per-process address space.
   - Allocates user stack with guard pages.
   - Forges an `iret` trap frame (`CS = USER_CS | 3`, `SS = USER_DS | 3`, `EFLAGS = 0x202`).
   - Executes `iret` to drop to privilege level 3.
   - User process exercises syscall boundary, validates pointer rejection, performs copy-on-write `fork()`, reaps child with `wait()`, `exec()`s `/bin/HELLO`, opens BSD sockets, and communicates over UDP/TCP.
7. **Shell & Maintenance**:
   - Spawns background status bar task (`statusd`).
   - Launches interactive kernel shell on serial and VGA console.

---

## 3. Full Interactive Shell & Diagnostic Session

```
stratum> meminfo
Physical memory
  total     : 127 MiB (32736 frames of 4096 B)
  in use    : 2 MiB (578 frames)
  free      : 125 MiB (32158 frames)
Virtual memory
  paging    : enabled
  kernel at : 0xc0000000
  linear map: 0xc0000000 - 0xc1000000 (16 MiB of physical memory)
  tables    : 6 page tables, 4352 pages mapped
  faults    : 4 total
  spaces    : 3 address spaces created
  COW       : 4 faults, 3 needed a copy, 1 resolved by dropping the last sharer
  shared    : 0 frames held by more than one address space
Kernel heap
  window    : 0xd0000000 (+1 MiB committed, max 64 MiB)
  in use    : 32 KiB across 4 blocks (2 free)
  integrity : consistent
Firmware memory map (6 regions)
  [ 0] 00000000 - 0009fbff  usable             639 KiB
  [ 3] 00100000 - 07fdffff  usable             126 MiB

stratum> ps
   PID  PPID  NAME           STATE     RING VMSPACE      TICKS  SWITCH
     0     0  idle           ready     ring0 kernel          36       3
     1     0  statusd        sleeping  ring0 kernel           0       6
     2     0  shell          running   ring0 kernel         104       7
     3     2  init           zombie    ring3 -                2       4
  4 tasks, 22 context switches total

stratum> cpus
Processors (4 online of 4 reported)
   CPU  APIC  ROLE STATE          STACK    PINGS      TLB   IDLE LOOPS
     0     0   bsp online    0x00000000        0        0            0  <- this one
     1     1    ap online    0xe800a000        0        0            1
     2     2    ap online    0xe800f000        0        0            1
     3     3    ap online    0xe8014000        0        0            1
  The scheduler runs on cpu 0 only; the others service interrupts. See docs/SMP.md.

stratum> ipi
broadcasting a ping IPI to every other processor...
  cpu 1: 0 -> 1 ping(s)
  cpu 2: 0 -> 1 ping(s)
  cpu 3: 0 -> 1 ping(s)
3 of 3 other processor(s) answered

now a TLB shootdown, which waits for every processor to acknowledge:
  1 shootdown(s) sent, 3 served (was 0)

stratum> disk
ATA drives
  DEV  MODEL                           SECTORS    ADDR
  hd0  QEMU HARDDISK                     36864   LBA48
  2 read(s), 2 sector(s), 2 command(s), 0 error(s), 0 timeout(s)
Block devices
  NAME     TYPE        FIRST LBA      SECTORS
  hd0      disk                0        36864
  hd0p1    0e               2048        32768

stratum> mount
FAT16 "STRATUM" on hd0p1, mounted at /
  geometry  : 512-byte sectors, 4 per cluster (2 KiB clusters)
  layout    : 1 reserved, FAT at +1 (2 x 32 sectors), root at +65 (512 entries), data at +97
  size      : 32768 sectors, 8167 clusters
  activity  : 52 sector read(s), 30 cache hit(s), 8 lookup(s), 2 file(s) read whole

stratum> ls /bin
  NAME               SIZE  ATTR
  .                     0  d---
  ..                    0  d---
  HELLO              8752  ----
  INIT              12524  ----
  2 file(s), 21276 bytes; 2 director(y|ies)

stratum> harden
Kernel/user separation
  null page       : unmapped
  CR0.WP          : set - ring 0 honours read-only pages
  kernel .text    : read-only
  kernel .rodata  : read-only
  SMEP (CR4.20)   : enabled - ring 0 cannot execute user pages
  SMAP (CR4.21)   : enabled - ring 0 cannot touch user pages without EFLAGS.AC
Stacks
  task stacks at  : 0xe0000000, 16 KiB each
  guard pages     : one unmapped page below every stack
  canaries        : checked on every context switch (0 failures)
  this task's guard page at 0xe000a000 is unmapped, as it should be
W^X
  paging          : PAE, three levels, 64-bit entries
  no-execute      : EFER.NXE set; data pages are mapped bit-63 no-execute
  read-only       : .text and .rodata, enforced on ring 0 by CR0.WP
  try it          : `fault nx` jumps into a data page, `fault text`
                    writes to .text; both must fault
Address spaces
  kernel half     : one page directory, shared by pointer, so every
                    address space sees identical kernel mappings

stratum> pagemap 0xc0109000
virtual   : 0xc0109000
pdpt index: 3    dir index: 0    table index: 265
physical  : 0x00109000
pte       : 0000000000109001
flags     : present read-only executable supervisor

stratum> pagemap 0xd0000000
virtual   : 0xd0000000
pdpt index: 3    dir index: 128    table index: 0
physical  : 0x002d3000
pte       : 80000000002d3203
flags     : present write no-execute supervisor kernel-owned

stratum> fault null
  faulting address: 0x00000000
  access          : write from ring 0
  reason          : nothing is mapped at that address
  region          : the null page - almost certainly a NULL dereference

================================================================
 KERNEL PANIC - StratumOS
================================================================
 page fault at 0x00000000 in the null page
  EAX 00000000  EBX 00120466  ECX 0000000a  EDX 0000002d
  EIP 0010c760  CS  0008      EFLAGS 00000286
  vector 14 (Page Fault)  error 00000002
  CR0 80010011  CR2 00000000  CR3 00123000  CR4 00000000
Call trace (return addresses; the faulting frame is EIP above):
  [0] 0x0010dbb6     <- resolved via embedded ksyms table
  [1] 0x0010e157        shell_run_line / shell_task
  [2] 0x00101b02        thread_trampoline
```

---

## 4. Performance & Microbenchmarks Analysis

The kernel measures micro-architectural costs directly using TSC calibration against the 8254 PIT (`make bench`).

Under QEMU TCG at a calibrated 2100 MHz, median of 24 samples (with the 2306-cycle harness overhead subtracted):

| Benchmark | Cycles | Time | Architectural Note |
|---|---:|---:|---|
| virtual $\rightarrow$ physical translation | 55 | 26.3 ns | Two loads through the PAE page-table recursive window |
| heap integrity walk | 58 | 27.8 ns | Header and footer canary verification across all blocks |
| context switch | 654 | 311.3 ns | `CR3` swap, stack pointer swap, canary validation, accounting |
| physical frame alloc + free | 733 | 349.2 ns | PMM bitmap scan, 32 frames per word |
| `kmalloc(64)` + `kfree` | 962 | 458.0 ns | First-fit search, block splitting, boundary-tag coalescing |
| map + unmap a 4 KiB page | 1054 | 502.1 ns | PTE updates, page-table allocation if needed, and `invlpg` |
| `int 0x80` syscall roundtrip | 1488 | 708.6 ns | Ring-3 $\rightarrow$ Ring-0 $\rightarrow$ Ring-3 privilege transition |
| `ksnprintf` | 2666 | 1269.3 ns | Base-10 formatting with software 64-bit integer division |
| `memset` 4 KiB | 4977 | 2369.8 ns | 32-bit dword store loop |
| `memcpy` 4 KiB | 6767 | 3222.2 ns | 1.65 cycles/byte, 32-bit dword copy loop |

*Note on emulation*: The absolute cycle counts reflect QEMU TCG emulation rather than native silicon execution. However, relative ratios between primitives are preserved.

### Sampling Profiler
StratumOS includes an in-kernel statistical profiler hooked into the timer interrupt. Because interrupt frames contain the interrupted `EIP`, the profiler samples execution and attributes cycles to function symbols via an embedded symbol table:

```
stratum> profile run 1500

  SAMPLES   SHARE  FUNCTION
       32   45.7%  kmalloc
       24   34.2%  kfree
       14   20.0%  emit_number      <- integer formatting digit loop
```

Embedding this symbol table requires a three-pass linking pipeline: Pass A generates raw addresses; Pass B accounts for table size shifts; Pass C generates the final kernel ELF, followed by automated verification that table addresses match the final executable image.
→ Detailed breakdown: [docs/PERFORMANCE.md](PERFORMANCE.md)

---

## 5. Two Architectures & Portability Methodology

```bash
make ARCH=riscv64 run
make portability
```

The boot structure of RISC-V 64 closely mirrors the x86 journey:
```
x86:     16-bit real mode  ->  32-bit protected mode  ->  PAE paging
RISC-V:  machine mode (M)  ->  supervisor mode (S)    ->  Sv39 paging
```
A RISC-V hart comes out of reset in machine mode. Translation cannot be enabled in M-mode because fetches bypass `satp`. The kernel configures trap vectors (`mtvec`), sets up `mstatus`, delegates exceptions to supervisor mode, and uses `mret` to enter S-mode, where 3-level Sv39 paging is activated.

`make portability` evaluates how much of the non-architecture kernel code builds unmodified for RISC-V 64 with `-Werror`:
- **63% of the portable kernel files compile completely unmodified**:
  - Entire TCP/IP network stack above device driver: `net/{net,arp,ipv4,udp,tcp}.c`
  - VFS and FAT16 filesystem: `fs/{fat16,blockdev,vfs,devfs,fatfs}.c`
  - Allocators: `mm/{heap,pmm}.c`
  - Utility runtime: `core/{printf,string,div64,log}.c`

The shared code requires exactly **two** external functions from the architecture layer: `console_putc()` and `timer_ms()`.
→ Detailed breakdown: [docs/PORTING.md](PORTING.md)

---

## 6. Networking Architecture, Sockets, and Congestion Control

StratumOS implements a native TCP/IP stack from Ethernet up to BSD sockets:
- **Intel 82540EM (e1000)**: PCI bus-mastering network interface controller. Uses two 32-descriptor circular DMA rings.
- **ARP**: Cache table with timestamp-based entry aging and automatic broadcast queries.
- **IPv4**: Packet parsing, validation, IP checksum computation, and single-gateway routing.
- **ICMP**: Ping request and reply handling.
- **UDP**: Header handling with pseudo-header checksum and loopback queuing.
- **TCP**: Full connection state machine (LISTEN, SYN_SENT, SYN_RECEIVED, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT).
  - RFC 5681 Congestion Control: Slow start, congestion avoidance, fast retransmit on 3 duplicate ACKs, fast recovery.
  - Measured Retransmission Timeout (RTO): Jacobson’s smoothed round-trip time estimator with variance term, exponential backoff, and Karn's rule.
- **Application Protocols**:
  - DHCP: DORA (Discover, Offer, Request, Ack) client.
  - DNS: Name resolution with cache and compression pointer loop protection.
  - HTTP: HTTP/1.1 client supporting chunked transfer encoding.
- **BSD Socket Layer**: File descriptors wrapping sockets. `read()` is `recv()`, `write()` is `send()`.
→ Detailed breakdown: [docs/NETWORK.md](NETWORK.md)

---

## 7. 64-bit Long Mode Technical Mechanics

The repository origin was a 16-bit to 32-bit project; the 64-bit transition represents the logical third tier:
1. Low-memory trampoline (`0x9000`) setup.
2. Building 4-level PML4 tables mapping the lowest 1 GiB.
3. Clearing `CR0.PG` to enable `CR4.PAE`.
4. Setting `IA32_EFER.LME` (Long Mode Enable).
5. Setting `CR0.PG` (entering 64-bit compatibility mode).
6. Far jump reloading `CS` with 64-bit descriptor (`L=1`, `D=0`).
7. Executing 64-bit immediate instructions and reading pointers beyond the 4 GiB boundary.
8. Far jump back to 32-bit compatibility segment, clearing `EFER.LME`, and restoring PAE kernel state.
→ Detailed breakdown: [docs/LONGMODE.md](LONGMODE.md)

---

## 8. Testing Methodology & Automation Details

StratumOS tests 5 distinct validation tiers:
1. **Host unit tests**: Glissando unit tests verifying string, printf, and 64-bit division against host `glibc`.
2. **Pre-boot image checks**: Python scripts validating Multiboot2 header checksums, load addresses, and absence of SSE instructions before booting.
3. **In-kernel test suites**: 24 suites executing 1243 assertions against live hardware registers and MMU state.
4. **QEMU Scenario testing**: 15 distinct headless QEMU boot environments testing SMP, legacy CPUs, network cards, and deliberate panics.
5. **libFuzzer campaigns**: Continuous fuzzing of parsers under AddressSanitizer.

Headless automation relies on QEMU's `isa-debug-exit` mapped at I/O port `0xF4`. When the kernel writes code `0x01`, QEMU exits cleanly with code `(0x01 << 1) | 1 = 3`.
→ Detailed breakdown: [docs/TESTING.md](TESTING.md)

---

## 9. Fuzzing Harnesses & Bugs Found

The libFuzzer targets in `tests/fuzz/` test the actual kernel source files:
- `fuzz_elf`: Untrusted ELF32 binaries passed to the program loader.
- `fuzz_fat16`: Malformed directory records, boot sectors, and cluster chains.
- `fuzz_heap`: Allocation and deallocation sequences with random sizes.
- `fuzz_acpi`: Corrupted RSDP and MADT structures.
- `user/fuzz.c`: 40,000 malformed syscall invocations issued from ring 3.

### Bugs Discovered by Fuzzing

| Vulnerability / Bug | Detection Vector | Root Cause & Resolution |
|---|---|---|
| `memmove` overlapping copy | ASan `memcpy` interceptor | `memmove` routed to `memcpy` in forward order; fixed with directional copy check |
| ACPI RSDP length overrun | `fuzz_acpi` | Kernel trusted length field from untrusted BIOS; fixed by capping table size |
| Heap coalescing failure | `fuzz_heap` (`heap_check`) | Shrinking `krealloc` left fragmented boundary tags; fixed coalescing logic |
| Ring-3 page fault panic | `fuzz_elf` | ELF with entry point outside load segment panicked ring 0; fixed to kill process |
| ELF segment allocation leak | Resource leak assertion | Allocation error mid-segment failed to unmap earlier pages; fixed unwinding |
| Syscall log flooding | Ring-3 syscall fuzzer | Rapid invalid syscalls overwhelmed serial console; added call-site rate limiter |
→ Detailed breakdown: [docs/FUZZING.md](FUZZING.md)

---

## 10. Complete Repository Layout

```
boot/                      the bootloader — real mode, then the PM switch
  stage1.asm               512-byte MBR: load stage 2 and verify it
  stage2.asm               A20, E820, disk I/O, PM switch, ELF32 loader
  serial.inc               16550 output from real mode

kernel/
  arch/x86/                everything that is x86 rather than kernel
    sources.mk arch.mk     what this architecture compiles, and how
    boot.asm               Multiboot2 header + the unified entry point
    isr.asm                256 generated interrupt stubs, one common tail
    switch.asm             context switch and the new-task trampoline
    usermode.asm           the forged IRET frame that reaches ring 3
    gdt.c idt.c irq.c      descriptor tables, dispatch, 8259 PIC
    cpu.c                  CPUID, reset, QEMU exit
    harden.c               W^X, SMEP/SMAP, and reading them back
    acpi.c                 RSDP, RSDT/XSDT, the MADT
    apic.c                 the local APIC, IPIs, INIT-SIPI-SIPI
    ap_boot.asm            176 bytes: a second CPU, real mode to the higher half
    longmode.c             four-level tables, and the boundary of the claim
    longmode_tramp.asm     32-bit -> 64-bit -> 32-bit, from a low copy
  arch/riscv64/            the second architecture
    sources.mk arch.mk     the same contract, 160 lines instead of 520
    boot.S                 park the other harts, then mret into S-mode
    trap_entry.S           31 registers saved by hand; there is no pushad
    trap.c                 two vectors, and decoding instruction length
    paging.c               Sv39: three levels, and no self-mapping window
    timer.c uart.c main.c  the CLINT, a 16550, and 58 assertions
  mm/
    pmm.c                  bitmap physical frame allocator
    vmm.c                  PAE paging, the self-map window, NX, fault reporting
    heap.c                 guarded first-fit kmalloc
  fs/
    blockdev.c             MBR partitions, and the bounds check for each
    fat16.c                read-only FAT16: BPB, chains, 8.3 names
    vfs.c devfs.c fatfs.c  VFS core, device filesystem, FAT VFS adapter
  net/                     the protocol stack, all of it portable
    net.c                  addresses, the checksum, Ethernet, the frame builder
    arp.c                  a cache that ages out, and why a miss drops
    ipv4.c                 header validation, ICMP echo, the pseudo-header
    udp.c tcp.c            UDP and TCP with RFC 5681 congestion control
    dhcp.c dns.c http.c    DHCP client, DNS resolver, HTTP/1.1 client
    socket.c               BSD socket abstraction over the VFS
  core/
    bootinfo.c             the two boot protocols, normalised
    sched.c                scheduler, tasks, fork, wait, the zombie reaper
    smp.c                  per-CPU state, bring-up, IPIs, TLB shootdown
    spinlock.c             real locks: test-and-test-and-set, bounded
    syscall.c              int 0x80 and userspace pointer validation
    elf.c                  defensive ELF32 loader for untrusted images
    usermode.c             address space + image for a process, and exec
    bench.c profile.c      microbenchmarks and the sampling profiler
    ksyms.c                the embedded symbol table
    printf.c log.c panic.c string.c div64.c ktest.c kmain.c
  drivers/                 serial, vga, timer, keyboard, rtc, pci, ata, e1000
  shell/shell.c            interactive shell, command dispatch, history
  include/                 headers, grouped by subsystem

user/                      ring-3 programs, built as separate ELFs
  init.c                   started at boot; probes boundary, forks, execs, net demo
  hello.c                  what exec() replaces a process with
  netcat.c                 ring-3 socket tester (UDP/TCP/listen)
  fuzz.c                   40,000 malformed syscalls from untrusted mode
  syscall.h                syscall stubs and libc-free runtime
  user.ld                  linked at 0x00400000

tools/
  mkimage.py               assemble a bootable image, patch stage 2's header
  mkfat.py                 write a FAT16 filesystem from a directory tree
  check-image.py           validate a built image: MBR, BPB, /BIN contents
  check-kernel.py          validate a linked kernel before boot
  run-tests.py             the QEMU test harness with pcap assertions
  portability.py           measure riscv64 compilation coverage
  gen-ksyms.py             embedded symbol table 3-pass generator
```

---

## 11. Design Decisions Worth Defending

1. **A Self-Mapping Page Directory Instead of Identity-Mapping RAM**:
   Mapping all physical RAM directly into the kernel virtual space does not scale to machines with large physical memories and reduces available virtual address space. StratumOS maps entry 511 of each Page Directory back to the directory itself. This allows virtual addresses `0xFFFFF000` and `0xFFE00000 + i*4096` to access any page table in the active address space at a cost of only 2 MiB per gigabyte.

2. **Linked High, Loaded Low**:
   `_start` cannot run at `0xC0100000` because paging is off at boot. The initial setup code lives in a low identity-mapped section (`0x00100000`), sets up PAE tables, enables paging, and executes an absolute jump into higher-half virtual space. `vmm_init()` drops the identity map, making NULL-pointer dereferences instantly fatal.

3. **Preemption After PIC End-Of-Interrupt (EOI)**:
   The timer interrupt handler requests a reschedule, but context switches occur strictly at the tail of the interrupt path *after* sending EOI to the PIC/APIC. Switching earlier would freeze the interrupt line if the newly scheduled task blocks outside an interrupt.

4. **One Generated Table of 256 Interrupt Stubs**:
   Rather than manually writing 256 ISR stubs that drift, macro generation defines all 256 vectors with unified register-saving frames and a common C dispatch tail.
→ Detailed breakdown: [docs/DESIGN-DECISIONS.md](DESIGN-DECISIONS.md)

---

## 12. Bugs Found and Fixed

### Defects Found When Merging Earlier Projects

| Location | Original Defect | Correction |
|---|---|---|
| `isr.asm`, `irq.asm` | Stubs called C without pushing register frame; context pointers were stale segment selector `0x10`. | Unified register frame push macro across all 256 vectors. |
| `idt.c` | `sti()` ran before PIC was remapped; timer interrupt hit vector 8 (double fault). | Forced initialization order: Remap PIC, register stubs, then enable interrupts. |
| `boot.asm` | `org 0x7C00` combined with `ds = 0x07C0` double-counted segment base by 0x7C00 bytes. | Set `ds = 0` with `org 0x7C00`. |
| `boot.asm` | Kernel loaded to physical `0x10000` (`es=0x1000`) but called at `0x1000`. | Harmonized load and jump addresses. |
| `boot.asm` | `ENTRY(kmain)` ignored in flat binary; jumped to first emitted function. | Added explicit entry jump stub. |
| `Makefile` | `run` depended on non-existent `$(ISO)` target rule. | Cleaned up Makefile dependency tree. |
| `README` | Documented paging, heap `malloc`, and syscalls that did not exist in code. | Implemented every feature and added 1243 assertions. |

### Defects Caught by StratumOS Test Suites

| Component | Defect | Caught By |
|---|---|---|
| `sched.c` | `wait()` marked collected zombie reusable while still linked in runqueue. | `proc` test suite |
| `heap.c` | 4-byte footer resulted in 28-byte block overhead, breaking 8-byte payload alignment. | `KT_ASSERT(IS_ALIGNED(p, 8))` |
| `bootinfo.c` | Command line pointed directly into stage 2 memory, dangling once identity map dropped. | `version` command page fault |
| `vmm.c` | Boot identity map covered page 0, preventing NULL-pointer dereference traps. | `fault null` test case |

---

## 13. Deliberate Scope Omissions

1. **No Long Filenames (LFN)**: FAT16 8.3 filenames only. Avoids patent legacy and excessive directory parser complexity.
2. **Polled PIO ATA**: Disk transfers use programmed I/O rather than bus-mastering DMA. Simpler, predictable timing during boot verification.
3. **No Scheduler Priorities or Affinity**: Single shared runqueue with 100 Hz round-robin quantum across all CPUs.
4. **I/O APIC Unprogrammed**: Legacy interrupts routed through 8259 PIC to BSP; local APIC timer handles per-CPU preemption.
5. **No `argv` or Environment Variables**: `exec()` accepts program name only.
6. **No Unix Signals or `kill`**: Processes terminate via explicit `exit()` or fatal CPU exception.
7. **No Demand Paging**: Program segments are eagerly mapped. Copy-on-write is the sole lazy mechanism.
8. **No KASLR**: Kernel linked at static higher-half address `0xC0100000`.
9. **Polled Serial Output**: UART transmissions spin-wait on transmit buffer empty.
10. **No Page Table Reclamation**: Empty page tables remain allocated for the lifetime of the address space.
11. **No 2 MiB Huge Pages**: Paging uses uniform 4 KiB frames.
12. **TCP Omissions**: No out-of-order reassembly, no SACK, no window scaling, and no IP fragmentation.
13. **No TLS / HTTPS**: Plaintext HTTP/1.1 client only.
14. **Transition Only, Not Full 64-bit Kernel**: x86-64 mode is entered and exited as a verification harness; kernel code remains 32-bit.
