# Architecture

## Subsystem map

```
                         ┌──────────────────────────┐
                         │   shell (task, ring 0)   │  20 commands, history
                         └────────────┬─────────────┘
                                      │
   ┌──────────────┐      ┌────────────┴─────────────┐      ┌──────────────┐
   │ user_demo    │      │        scheduler         │      │   ktest      │
   │ (ring 3)     │◄────►│  round robin, 100 Hz     │      │ 12 suites    │
   └──────┬───────┘      └────────────┬─────────────┘      └──────────────┘
          │ int 0x80                  │
   ┌──────┴───────┐                   │
   │   syscall    │  pointer          │
   │   dispatch   │  validation       │
   └──────┬───────┘                   │
          │                           │
   ┌──────┴───────────────────────────┴─────────────────────────────────┐
   │                        kernel services                            │
   │   kprintf · log · panic+backtrace · string · div64 · spinlock     │
   └──────┬──────────────────┬──────────────────┬─────────────────────┬─┘
          │                  │                  │                     │
   ┌──────┴──────┐    ┌──────┴──────┐    ┌──────┴──────┐    ┌─────────┴────┐
   │ heap        │    │    vmm      │    │    pmm      │    │   drivers    │
   │ kmalloc     │───►│  paging     │───►│  frames     │    │ serial  vga  │
   │ guarded     │    │  recursive  │    │  bitmap     │    │ pit     kbd  │
   └─────────────┘    └──────┬──────┘    └──────┬──────┘    │ rtc     pci  │
                             │                  │           └──────┬───────┘
   ┌─────────────────────────┴──────────────────┴──────────────────┴───────┐
   │                        arch/x86                                       │
   │   gdt+tss · idt (256 vectors) · irq (8259 PIC) · cpu (CPUID)          │
   │   isr.asm · switch.asm · usermode.asm · boot.asm                      │
   └───────────────────────────────┬───────────────────────────────────────┘
                                   │
   ┌───────────────────────────────┴───────────────────────────────────────┐
   │   bootinfo:  Multiboot2  │  StratumOS native   ──► struct boot_params │
   └───────────────────────────────────────────────────────────────────────┘
```

Dependencies point downward only. `heap` needs `vmm`, which needs `pmm`, which
needs the normalised boot parameters. Nothing below the services layer calls
upward, which is what makes the initialisation order in `kmain()` a straight
line rather than a negotiation.

## Initialisation order, and why it is forced

Most of the order in `kernel/core/kmain.c` is not a preference. Each step
below either cannot work before the one above it, or is actively dangerous
out of sequence.

| # | Step | Why here |
| --- | --- | --- |
| 0 | `_start` (assembly) | Builds early page tables, enables paging, and jumps from 1 MiB to `0xC0100000`. Everything below runs in the higher half - see [MEMORY.md](MEMORY.md). |
| 1 | `serial_init` | Needs no memory manager, no interrupts and no display, so it is the only channel that can report a failure in any of them. |
| 2 | `vga_init`, `console_init` | The display is useful but optional; serial comes first so a VGA bug is still diagnosable. |
| 3 | `cpu_detect` | Before any CPU feature is relied on. CPUID's own presence is probed by trying to flip `EFLAGS.ID`, because a 386 has no CPUID and executing it would fault. |
| 4 | `boot_parse` | Everything downstream needs the memory map. Without one there is nothing to do but panic. It also *copies* the loader's strings, because the memory they live in stops being addressable at step 12. |
| 5 | `parse_cmdline` | Must follow `boot_parse`, because the command line arrives through it. Affects log level and which demos run. |
| 6 | `gdt_init` | The IDT's gates reference the kernel code selector, so the GDT must be live first. Also loads the TSS, which ring 3 will need. |
| 7 | `idt_init` | Fills all 256 vectors. Deliberately does **not** enable interrupts. |
| 8 | `irq_init` | Remaps the 8259s from vectors 8–15 and 0x70–0x77 to 32–47. Until this happens, IRQ 0 arrives as `#DF` and IRQ 1 as `#GP`. Leaves every line masked. |
| 9 | `timer_init`, `keyboard_init`, `serial_console_init` | Each installs its handler and only then unmasks its own line, so an interrupt can never arrive before someone is ready for it. |
| 10 | **`sti()`** | The first moment this is safe. See below. |
| 11 | `pmm_init` | Needs the memory map and the kernel's own extent. Places its bitmap above the kernel image. |
| 12 | `vmm_init` | Adopts the page directory `_start` built, widens the linear map, and drops the boot identity mapping. Needs the PMM for the new page tables. |
| 13 | `heap_init` | Needs paging, because the heap is a virtual window backed on demand. |
| 14 | `rtc_init`, `pci_init` | Non-essential hardware. A failure is logged, not fatal. |
| 15 | `sched_init` | Needs the heap for task stacks. Adopts the boot context as pid 0. |
| 16 | `syscall_init` | Needs the IDT; re-installs vector 0x80 with DPL 3. |
| 17 | task creation | Needs the scheduler and the heap. |
| 18 | `sched_start` | The boot context becomes the idle task and never returns. |

### The `sti()` placement

Step 10 is the single most load-bearing line in the boot sequence. Out of
reset the 8259 pair maps IRQ 0–7 onto vectors 8–15, which collides head-on
with the CPU's own exception vectors: vector 8 is `#DF`, vector 13 is `#GP`.
Enabling interrupts before step 8 therefore turns the first timer tick into a
spurious double fault, and the machine triple-faults a moment later with no
useful diagnostic at all.

The original version of this kernel called `sti()` at the end of
`idt_initialize()`, three steps too early. The comment in `idt_init()` now
says why that line is absent.

## The interrupt path, end to end

```
hardware IRQ or CPU exception
        │
        ▼
isr_stub_N              (arch/x86/isr.asm, one of 256, generated)
  push err_code (0 if the CPU did not)
  push vector
        │
        ▼
isr_common
  pusha                 → edi esi ebp esp ebx edx ecx eax
  push ds/es/fs/gs      → via EAX, so all 32 bits are defined
  load kernel selectors → never trust what ring 3 left in them
  push esp              → THE pointer to struct regs
  call interrupt_dispatch
        │
        ▼
interrupt_dispatch      (arch/x86/idt.c)
  vector 32..47  → irq_dispatch → handler → PIC EOI → sched_preempt
  installed hook → handler            (page fault, syscall, int3, NMI)
  vector  0..31  → panic_with_regs    (register dump + backtrace)
  otherwise      → warn
        │
        ▼
isr_common (tail)
  pop segment registers
  popa                  → this is how a syscall's return value in r->eax
  add esp, 8              reaches userspace
  iret                  → restores EFLAGS, interrupt flag included
```

Two details that are easy to get wrong and are worth pointing at:

**`push esp` before the call.** That is the `struct regs *` the C handler
takes as its argument. The original project omitted it, so every handler
received whatever happened to be at `[esp+4]` — the saved `ds` value, `0x10` —
and dereferenced it. The fields it read came from the real-mode interrupt
vector table.

**No `sti` before `iret`.** `IRET` restores the caller's `EFLAGS`, interrupt
flag included, which is exactly right: a handler that interrupted a critical
section must not return into it with interrupts enabled. An explicit `sti`
there silently breaks every `irq_save`/`irq_restore` pair in the kernel.

## Where the frame layout is defined

`struct regs` in `kernel/include/arch/idt.h` mirrors, field for field, what
`isr.asm` pushes. The ordering is not arbitrary — the struct's first member is
the *last* thing pushed, because the stack grows down:

```
low address   gs fs es ds                      ← pushed by the stub
              edi esi ebp esp ebx edx ecx eax  ← pushed by `pusha`
              int_no err_code                  ← pushed by the stub
              eip cs eflags                    ← pushed by the CPU
high address  user_esp ss                      ← CPU, only on a ring change
```

`user_esp` and `ss` are only present when the trap crossed a privilege
boundary, which is why `regs_dump()` checks `cs & 3` before printing them —
otherwise it would be reporting stack garbage as a stack pointer.

The in-kernel `irq` test suite executes `int $0x03` specifically to exercise
this whole path end to end. If the struct and the assembly ever disagree
again, the kernel does not survive that line.

## Task model

Tasks are kernel threads. A task's entire context is its stack pointer:

```
context_switch(&prev->saved_esp, next->saved_esp)
  pushfd; push ebx, esi, edi, ebp   save callee-saved state + flags
  [save_esp] = esp                  park the outgoing task
  esp = load_esp                    the incoming task takes over
  pop ebp, edi, esi, ebx; popfd
  ret                               returns into whatever the new task
                                    was doing when it last gave up the CPU
```

Caller-saved registers need no handling: the C compiler has already assumed
`EAX`, `ECX` and `EDX` are destroyed across a call.

A brand-new task has no saved context, so `task_create()` fabricates one. The
stack is laid out so that `context_switch`'s epilogue delivers control to
`thread_trampoline` with the entry point and argument sitting where a normal
cdecl call would have left them — which avoids a special case in the switch
itself:

```
sp[0..3]  ebp edi esi ebx    popped by context_switch
sp[4]     eflags = 0x202     popfd; IF set, so the task is preemptible
sp[5]     thread_trampoline  `ret` lands here
sp[6]     (unused ret slot)
sp[7]     entry
sp[8]     arg
```

`ebp` is seeded to zero so that a backtrace from inside a new task terminates
cleanly instead of walking off into the heap.

## Reading further

- [BOOT.md](BOOT.md) — both boot paths, instruction by instruction
- [MEMORY.md](MEMORY.md) — the address space, the higher-half transition, the allocators
- [USERSPACE.md](USERSPACE.md) — the user program, the ELF loader, the privilege boundary
- [PERFORMANCE.md](PERFORMANCE.md) — benchmarks, the profiler, the symbol table
- [DESIGN-DECISIONS.md](DESIGN-DECISIONS.md) — the trade-offs behind the above
