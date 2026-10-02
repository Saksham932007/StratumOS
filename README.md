# StratumOS

A 32-bit x86 kernel and bootloader, written from scratch in C and assembly.
It takes a machine from the BIOS's first instruction in 16-bit real mode all
the way to a preemptively scheduled, paged kernel running an interactive shell
and executing code in ring 3.

[![CI](https://github.com/Saksham932007/StratumOS/actions/workflows/ci.yml/badge.svg)](https://github.com/Saksham932007/StratumOS/actions/workflows/ci.yml)
![language](https://img.shields.io/badge/C11%20%2B%20NASM-11k%20lines-blue)
![arch](https://img.shields.io/badge/arch-x86%20(i686)-lightgrey)
![license](https://img.shields.io/badge/license-MIT-green)

The same kernel binary boots two ways — through a bootloader written for this
project, and through GRUB via Multiboot2 — and 249 automated assertions run on
every push, including three QEMU boot scenarios.

```
BIOS ─► stage 1 (512 B MBR) ─► stage 2 ─► 32-bit protected mode ─► kernel ─► ring 3
         LBA/CHS disk I/O      A20 gate    flat GDT                 paging    syscalls
         retry + verify        E820 map    ELF32 loader              heap     int 0x80
                                                                 scheduler
                        GRUB ─► Multiboot2 ─────────────────────────┘
```

---

## Contents

- [What this actually does](#what-this-actually-does)
- [Quick start](#quick-start)
- [What it looks like running](#what-it-looks-like-running)
- [Feature matrix](#feature-matrix)
- [Two boot paths, one kernel](#two-boot-paths-one-kernel)
- [Testing](#testing)
- [Repository layout](#repository-layout)
- [Design decisions worth defending](#design-decisions-worth-defending)
- [Bugs this project found and fixed](#bugs-this-project-found-and-fixed)
- [What is deliberately not here](#what-is-deliberately-not-here)
- [Documentation](#documentation)

---

## What this actually does

Concretely, from power-on:

1. **The BIOS** loads one 512-byte sector and jumps to it.
2. **Stage 1** probes for INT 13h extensions, reads stage 2 off the disk by LBA
   (falling back to CHS with retries and a controller reset), verifies a magic
   number, and hands over.
3. **Stage 2** enables the A20 gate — trying the BIOS, the 8042 controller and
   the fast port in turn, verifying each with a memory-wraparound test — reads
   the firmware memory map with INT 15h/E820, streams the kernel off disk in
   32 KiB chunks, loads a flat GDT, sets `CR0.PE`, and then, in 32-bit code,
   **parses the kernel's ELF program headers** and copies each segment to its
   physical address.
4. **The kernel** detects which protocol loaded it from the magic in `EAX`,
   normalises the memory map, installs 256 interrupt vectors, remaps the PIC
   off the CPU's exception vectors, brings up the timer and keyboard, *then*
   enables interrupts, builds a physical frame allocator from the firmware map,
   turns on paging with a recursive page directory, creates a guarded kernel
   heap, enumerates PCI, starts a round-robin scheduler, and drops a demo task
   into ring 3.
5. **The shell** runs as a scheduled task, reachable from the VGA console or
   over a serial line, with line editing and command history.

It is about 11,000 lines, and every subsystem listed above is implemented and
tested rather than announced.

---

## Quick start

```bash
# Ubuntu / Debian
sudo apt install build-essential nasm gcc-multilib libc6-dev-i386 \
                 qemu-system-x86 grub-pc-bin grub-common xorriso mtools

git clone https://github.com/Saksham932007/StratumOS.git
cd StratumOS

make              # kernel, disk image, GRUB ISO, and the test images
make run          # boot through the custom bootloader
make run-iso      # boot through GRUB / Multiboot2
make test         # host unit tests + three QEMU boot scenarios
```

`make toolchain` reports exactly which tools were found. An `i686-elf-gcc`
cross-compiler is used automatically if one is on `PATH`; otherwise the host
GCC is driven with `-m32`, which works on any x86-64 Linux with multilib.

Other useful targets:

| Target | What it does |
| --- | --- |
| `make run-serial` | boot headless, serial on stdio — the shell works over it |
| `make debug` | start QEMU stopped, waiting for GDB on `:1234` |
| `make gdb` | attach GDB with symbols, break at `kmain` |
| `make sections` | dump the image layout and re-run the pre-boot validator |
| `make test-host` | host unit tests only (no emulator, ~1 second) |
| `make lines` | line counts by subsystem |

---

## What it looks like running

Boot, over serial, through the custom bootloader:

```
StratumOS stage1
StratumOS stage2
  [ok] A20 gate
  [ok] BIOS memory map
  [..] kernel ...
  [ok] kernel image staged
  [->] entering protected mode

  .-----------------------------------------------------.
  | StratumOS 0.3.0  -  x86 kernel: real mode to ring 3 |
  '-----------------------------------------------------'
[    0.000] INFO  boot: serial COM1        [ok] 115200 8N1
[    0.000] INFO  boot: CPU detect         [ok] GenuineIntel
[    0.000] INFO  boot: boot protocol      [ok] StratumOS native (ELF handoff)
[    0.000] INFO  boot: GDT + TSS          [ok] flat 4 GiB, rings 0 and 3
[    0.000] INFO  boot: IDT                [ok] 256 vectors
[    0.000] INFO  boot: PIC remap          [ok] IRQ 0-15 -> vector 32-47
[    0.000] INFO  timer: PIT at 100 Hz requested, 99.998 Hz actual (divisor 11932)
[    0.010] INFO  boot: interrupts         [ok] enabled
[    0.010] INFO  pmm: 32736 frames total (127 MiB), 32446 free (126 MiB)
[    0.010] INFO  vmm: paging enabled: 16 MiB identity-mapped, recursive PD
[    0.020] INFO  heap: kernel heap at 0xd0000000, 1024 KiB committed
[    0.030] INFO  pci: 6 PCI devices found
[    0.040] INFO  sched: scheduler ready; boot context adopted as pid 0 (idle)
[    0.040] INFO  syscall: syscall gate installed at int 0x80
```

Ring 3, exercising the syscall boundary from the untrusted side:

```
  [ring3] hello from user mode - privilege level 3
  [ring3] getpid() returned 4
  [ring3] slept 50 ms via syscall
  [ring3] asking the kernel to read a kernel address on my behalf
[    0.140] WARN  syscall: pid 4 passed an unreadable buffer 0x00100000+16 to write()
  [ring3] kernel refused it (EFAULT) - the pointer check works
  [ring3] unknown syscall correctly rejected
  [ring3] calling exit(0)
```

The shell:

```
stratum> meminfo
Physical memory
  total     : 127 MiB (32736 frames of 4096 B)
  in use    : 2 MiB (553 frames)
  free      : 125 MiB (32183 frames)
Virtual memory
  paging    : enabled
  identity  : 0x00000000 - 0x01000000
  tables    : 5 page tables, 4351 pages mapped
  faults    : 0
Kernel heap
  window    : 0xd0000000 (+1 MiB committed, max 64 MiB)
  in use    : 32 KiB across 4 blocks (2 free)
  integrity : consistent
Firmware memory map (6 regions)
  [ 0] 00000000 - 0009fbff  usable             639 KiB
  [ 3] 00100000 - 07fdffff  usable             126 MiB
  ...

stratum> ps
   PID  NAME           STATE        TICKS  SWITCH  STACK
     0  idle           ready            2       1  0x00000000
     1  statusd        sleeping         0       5  0xd0000050
     2  shell          running        104       5  0xd0004070

stratum> irq
  IRQ  HANDLER               COUNT
    0  pit                     161
    1  ps2-keyboard              0
    4  16550-uart                3
  spurious: 0    syscalls: 0
```

Diagnostics are a feature, not an afterthought. `fault null` on purpose:

```
stratum> fault null
  faulting address: 0x00000000
  access          : write from ring 0
  reason          : nothing is mapped at that address
  region          : the null page - almost certainly a NULL dereference

================================================================
 KERNEL PANIC - StratumOS 0.3.0
================================================================
 page fault at 0x00000000 in the null page
  EAX 00000000  EBX 00120466  ECX 0000000a  EDX 0000002d
  EIP 0010c760  CS  0008      EFLAGS 00000286
  vector 14 (Page Fault)  error 00000002
  CR0 80010011  CR2 00000000  CR3 00123000  CR4 00000000
Call trace (return addresses; the faulting frame is EIP above):
  [0] 0x0010dbb6     <- addr2line resolves these to
  [1] 0x0010e157        shell_run_line / shell_task /
  [2] 0x00101b02        thread_trampoline
```

---

## Feature matrix

Everything marked ✅ is implemented and covered by a test.

### Bootloader

| | |
| --- | --- |
| ✅ | Two-stage design, stage 1 fitting in 315 of the MBR's 510 usable bytes |
| ✅ | INT 13h extended (LBA) reads, with capability probing |
| ✅ | True LBA→CHS translation fallback, one sector at a time |
| ✅ | Read retries with controller reset between attempts |
| ✅ | A20 gate via BIOS / 8042 / fast port, each verified by a wraparound test |
| ✅ | E820 memory map, with E801 and AH=88h fallbacks |
| ✅ | ACPI 3.0 extended-attribute handling, zero-length entry filtering |
| ✅ | 16→32 bit protected-mode transition with a flat 4 GiB GDT |
| ✅ | ELF32 program-header loader, including `.bss` zeroing from `p_memsz` |
| ✅ | Direct 16550 output, so the whole boot is visible headlessly |
| ✅ | Build-time size assertions — overflowing a sector is a build error |

### Kernel

| | |
| --- | --- |
| ✅ | Multiboot2 and native boot protocols behind one normalised interface |
| ✅ | Flat GDT with ring-0/ring-3 descriptors, plus a TSS |
| ✅ | All 256 IDT vectors filled from a generated stub table |
| ✅ | Full register dump, page-fault error decoding, EBP backtrace |
| ✅ | 8259 PIC remapping, per-line masking, spurious-interrupt detection |
| ✅ | Bitmap physical frame allocator, built from the firmware map |
| ✅ | Paging: 4 KiB pages, recursive page directory, `CR0.WP`, unmapped null page |
| ✅ | `map` / `unmap` / `protect` / `translate`, with frame-ownership tracking |
| ✅ | Kernel heap: first-fit, coalescing, guard magics, grows on demand |
| ✅ | Preemptive round-robin scheduler, per-task stacks, sleep/yield/exit |
| ✅ | Ring 3 via a forged IRET frame; `int 0x80` with pointer validation |
| ✅ | Drivers: 16550 (in and out), VGA text, PIT, PS/2 keyboard, CMOS RTC, PCI |
| ✅ | `kprintf` with width/precision/64-bit support, levelled logging |
| ✅ | 64-bit division helpers — the kernel links against nothing at all |
| ✅ | 20-command shell with line editing, history, and fault injection |

---

## Two boot paths, one kernel

This is the part of the project worth the most scrutiny, because it is where
the two source projects genuinely merge rather than merely sit side by side.

`build/stratum.elf` is one file. GRUB finds a Multiboot2 header in it and loads
it. Stage 2, which has never heard of Multiboot, reads the same file off a raw
disk, walks its ELF program headers, and copies the same segments to the same
addresses. Both then jump to `_start` with a protocol magic in `EAX` and an
information pointer in `EBX`.

```c
/* kernel/core/bootinfo.c - the only file that knows the difference */
switch (magic) {
case STRATUM_BOOT_MAGIC:          /* 0x53545241, our stage 2  */
    ok = parse_stratum(info_addr);
    break;
case MULTIBOOT2_BOOTLOADER_MAGIC: /* 0x36d76289, GRUB         */
    ok = parse_multiboot2(info_addr);
    break;
}
```

Everything downstream reads one `struct boot_params`. Adding a third protocol
means adding one parser and changing nothing else. The CI boots both paths on
every push, so neither can rot while the other is being worked on.

See [docs/BOOT.md](docs/BOOT.md) for the full walkthrough, including the exact
memory layout at each stage and why the A20 gate still matters.

---

## Testing

An OS that "boots on my machine" is not evidence of much. This project is set
up so that a regression is caught by a script, not by someone squinting at
QEMU.

| Layer | What runs | Count |
| --- | --- | --- |
| **Host unit tests** | the kernel's real `printf`/`string`/`div64` sources, compiled for the host, diffed against glibc | 92 checks |
| **Pre-boot validation** | Multiboot2 header and checksum, ELF type, entry point inside a load segment, load address, `.bss`/`.user` alignment, absence of SSE | 20 failure conditions, every link |
| **In-kernel suites** | allocator, paging, heap coalescing, interrupts, scheduler, syscall pointer validation, run against real hardware state | 157 checks in 9 suites |
| **Boot scenarios** | custom bootloader unattended, GRUB/Multiboot2 unattended, 21 shell commands typed over serial | 3 scenarios |

```
$ make test
  RUN     host unit tests
92 checks, 0 failures

  RUN     QEMU boot tests (both boot paths)
  [interactive-shell] shell driven over the serial console, command by command
    commands run     : 21
    result           : PASS
  [custom-bootloader] two-stage BIOS bootloader from a raw disk image
    in-kernel suites : 9/9 passed
    result           : PASS
  [multiboot2-grub] Multiboot2 via GRUB from an ISO
    in-kernel suites : 9/9 passed
    result           : PASS

run-tests: all 3 scenario(s) passed
```

Three details that make this work unattended:

- The kernel takes `autotest` on its command line, runs its suites, and then
  asks QEMU to exit via `isa-debug-exit`. No timeouts, no screen scraping for
  a prompt.
- Test output is a fixed, greppable format (`ktest: heap ... PASS (57 checks)`),
  so the harness asserts on data rather than on prose.
- The interactive scenario drives the shell the way a person would — waiting
  for the prompt, sending one command, reading the reply. Piping a script into
  the serial port does not work, because the UART's 16-byte FIFO overruns long
  before the kernel starts reading it. That is the kind of thing you only learn
  by trying it.

More in [docs/TESTING.md](docs/TESTING.md).

---

## Repository layout

```
boot/                      the bootloader — real mode, then the PM switch
  stage1.asm               512-byte MBR: load stage 2 and verify it
  stage2.asm               A20, E820, disk I/O, PM switch, ELF32 loader
  serial.inc               16550 output from real mode

kernel/
  arch/x86/                everything that is x86 rather than kernel
    boot.asm               Multiboot2 header + the unified entry point
    isr.asm                256 generated interrupt stubs, one common tail
    switch.asm             context switch and the new-task trampoline
    usermode.asm           the forged IRET frame that reaches ring 3
    gdt.c idt.c irq.c      descriptor tables, dispatch, 8259 PIC
    cpu.c                  CPUID, reset, QEMU exit
  mm/
    pmm.c                  bitmap physical frame allocator
    vmm.c                  paging, recursive page directory, fault reporting
    heap.c                 guarded first-fit kmalloc
  core/
    bootinfo.c             the two boot protocols, normalised
    sched.c                round-robin scheduler and tasks
    syscall.c              int 0x80 and userspace pointer validation
    usermode.c             mapping the ring-3 payload
    user_demo.c            code that runs at privilege level 3
    printf.c log.c panic.c string.c div64.c spinlock.c ktest.c kmain.c
  drivers/                 serial, vga, timer, keyboard, rtc, pci
  shell/shell.c            20 commands, line editing, history
  include/                 headers, grouped by subsystem

tools/
  mkimage.py               assemble a bootable image, patch stage 2's header
  check-kernel.py          validate a linked kernel before it is ever booted
  run-tests.py             the QEMU harness
  grub.cfg grub-test.cfg

tests/host/                unit tests that need no emulator
linker/kernel.ld           the link script, with its constraints documented
docs/                      architecture, boot, memory, testing, debugging
```

---

## Design decisions worth defending

The full set is in [docs/DESIGN-DECISIONS.md](docs/DESIGN-DECISIONS.md). Four
that shaped the most code:

**A recursive page directory instead of identity-mapping all of RAM.**
Once `CR0.PG` is set, a page table can only be edited through a virtual
address — but page tables come from the physical allocator, which can return a
frame anywhere. Mapping all of physical memory to solve that does not scale.
Instead the last page directory entry points at the directory itself, so the
hardware walk resolves `0xFFFFF000` to the directory and
`0xFFC00000 + i*4096` to the table for entry *i*. Every page table in the
system becomes addressable, at the cost of one 4 MiB slot.

**The null page is left unmapped.**
The identity map starts at the *second* page. The first one holds only the
real-mode IVT and BIOS data area, which a protected-mode kernel has no use
for, and leaving it unmapped converts every NULL dereference into a precisely
located page fault. It is the cheapest bug-catching measure available.

**Preemption happens after the PIC's end-of-interrupt, not before.**
The timer handler only *requests* a reschedule; the switch happens at the tail
of the interrupt path. Switching earlier parks the outgoing task mid-handler
with the interrupt still in service — and if the incoming task then blocks
anywhere other than an interrupt, nothing ever sends that EOI and the timer
stops permanently.

**One generated table of 256 interrupt stubs.**
The conventional approach is hand-written `isr0..isr31` plus `irq0..irq15` in
two files with near-identical tails. They drift, and the remaining 200 vectors
point at nothing. Here the preprocessor generates all 256 and publishes a table
of their addresses; there is exactly one place the register-saving convention
is defined, and therefore exactly one place it can be wrong.

---

## Bugs this project found and fixed

StratumOS merges two earlier repositories
([Advanced-Bootloader-16-bit-to-32-bit-C-Kernel](https://github.com/Saksham932007/Advanced-Bootloader-16-bit-to-32-bit-C-Kernel)
and [Advanced-Kernel](https://github.com/Saksham932007/Advanced-Kernel)).
Merging them meant reading both carefully, which turned up real defects —
several of which meant the advertised features could not have worked:

| Where | Defect |
| --- | --- |
| `isr.asm`, `irq.asm` | The stubs called the C handlers without pushing the register frame, so every handler received a stale segment selector (`0x10`) as its context pointer. The keyboard and timer paths could not have worked. |
| `idt.c` | `sti()` ran *before* the PIC was remapped, so the first timer tick arrived on vector 8 — the double-fault vector. |
| `boot.asm` | `org 0x7C00` combined with `ds = 0x07C0` double-counts the segment base; every memory reference landed 0x7C00 bytes high. |
| `boot.asm` | The kernel was loaded to `es=0x1000` (physical 0x10000) but called at `0x1000`. |
| `boot.asm` | `ENTRY(kmain)` has no effect in a flat binary, so `call 0x1000` entered whichever function the compiler emitted first. |
| `Makefile` | `run` depended on `$(ISO)`, but the only rule was named `iso` — the target could never build. |
| `README`, `ARCHITECTURE` | Documented paging, virtual memory, a kernel heap with `malloc`/`free`, and system calls. None of them existed. |

The last row is the one that mattered most. The features are now implemented,
and the test suite exists so that the documentation cannot quietly drift away
from the code again.

---

## What is deliberately not here

Being clear about scope is more useful than a longer feature list.

- **No filesystem.** Nothing is read from disk after the kernel itself.
- **No SMP.** Uniprocessor only; `spinlock.c` is honest about being an
  interrupt mask rather than a spin, and says what it will become.
- **No higher-half kernel.** The kernel is identity-mapped at 1 MiB. This is a
  trade-off, not an oversight — [docs/MEMORY.md](docs/MEMORY.md) explains what
  it buys and what it costs.
- **No demand paging or copy-on-write.** Every page fault is currently a bug,
  and is reported as one.
- **No user-space processes.** Ring 3 runs a payload linked into the kernel
  image; there is no ELF loader for userspace, because the bootloader already
  demonstrates ELF loading.
- **Serial transmit is polled**, which holds interrupts off for the duration of
  a write. The fix is a transmit ring buffer, in
  [docs/ROADMAP.md](docs/ROADMAP.md).
- **Empty page tables are never reclaimed.** Unmapping the last page in a
  4 MiB region leaves its table allocated.

---

## Documentation

| Document | Contents |
| --- | --- |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | subsystem map, initialisation order and why it is forced |
| [docs/BOOT.md](docs/BOOT.md) | both boot paths instruction by instruction |
| [docs/MEMORY.md](docs/MEMORY.md) | address-space layout, the three allocators, the recursive mapping |
| [docs/TESTING.md](docs/TESTING.md) | the four test layers and how to add to each |
| [docs/DEBUGGING.md](docs/DEBUGGING.md) | GDB against QEMU, reading a panic, common symptoms |
| [docs/DESIGN-DECISIONS.md](docs/DESIGN-DECISIONS.md) | the trade-offs, with the alternatives that were rejected |
| [docs/ROADMAP.md](docs/ROADMAP.md) | what is next, and what each item would take |
| [CONTRIBUTING.md](CONTRIBUTING.md) | build, style and test requirements |

### References used

- Intel® 64 and IA-32 Architectures Software Developer's Manual, Volume 3A —
  protected mode, paging, interrupt and exception handling
- [Multiboot2 Specification 2.0](https://www.gnu.org/software/grub/manual/multiboot2/multiboot.html)
- [OSDev Wiki](https://wiki.osdev.org/) — A20, 8259 PIC, 8254 PIT, 8042, PCI
- Ralf Brown's Interrupt List — INT 13h and INT 15h behaviour

---

## License

MIT — see [LICENSE](LICENSE).
