# Design decisions

Each entry states the decision, what was rejected, and what it costs. The
costs are the point: a decision with no downside was not a decision.

---

## 1. One kernel binary, two boot protocols

**Decision.** `stratum.elf` carries a Multiboot2 header *and* is loadable by
our own stage 2, which parses its ELF program headers. Protocol detection is
one `switch` on the magic in `EAX`; everything downstream reads a normalised
`struct boot_params`.

**Rejected: two kernels.** The obvious merge of the two source projects would
have kept a flat-binary kernel for the custom bootloader and an ELF one for
GRUB. That means two link scripts, two sets of entry assumptions, and two
things to keep working — and in practice one of them rots while the other is
being developed.

**Rejected: a flat binary for both.** GRUB can load a flat binary via the
`a.out` kludge, but then there are no program headers, no `.bss` size
information and no symbols, which costs the backtrace and GDB.

**Cost.** Stage 2 has to contain a 32-bit ELF loader — about 40 instructions,
and it has to run after the protected-mode switch so the destination at 1 MiB
is reachable without unreal mode. That is real complexity in assembly, in a
place that is awkward to debug.

**Why it was worth it.** It is the thing that makes this one project rather
than two in a trenchcoat, and it forced a clean boot-information abstraction
that a third protocol could slot into unchanged.

---

## 2. A recursive page directory

**Decision.** Page directory entry 1023 points at the directory itself, so
`0xFFFFF000` resolves to the directory and `0xFFC00000 + i*4096` to the page
table for entry *i*.

**The problem it solves.** After `CR0.PG`, page tables can only be written
through virtual addresses, but they are allocated from the physical frame
allocator, which can return a frame anywhere in RAM.

**Rejected: identity-map all of physical memory.** Works up to a point and
then does not: on a 4 GiB machine there is no room left in a 32-bit address
space. It also makes every physical page writable through a kernel pointer
forever.

**Rejected: a temporary mapping window.** Reserve one virtual page, map the
table into it, edit, unmap. Correct, but every page-table edit becomes
map/`invlpg`/edit/unmap, and it is not reentrant — which matters because
`vmm_map` is called from the heap, which is called from interrupt handlers.

**Cost.** One 4 MiB slot at the top of the address space is unusable, and
`vmm_map()` has to refuse to map over it. Two accessor functions have to know
whether paging is on yet. The mechanism is genuinely confusing the first time
you meet it — hence the long comment at the top of `mm/vmm.c`.

---

## 3. The null page is left unmapped

**Decision.** The identity map starts at `PAGE_SIZE`, not 0.

**Rejected: mapping from 0.** It is one fewer special case, and the first page
does contain the real-mode IVT and BIOS data area.

**Cost.** Essentially none. A protected-mode kernel has no use for the
real-mode IVT, and the BIOS data area is not consulted after boot. The one
hazard — a loader placing its information block in the first page — is handled
with an explicit check and a warning, so if it ever happens the reason the
check stopped working is in the log.

**Why.** It converts every NULL dereference from a silent write into a
precisely located fault. Among all the bug-catching measures available to a
kernel, this is the cheapest.

---

## 4. One generated table of 256 interrupt stubs

**Decision.** `isr.asm` generates a stub for every vector 0–255 with the
preprocessor and publishes `isr_stub_table`, which C indexes when filling the
IDT. One common tail defines the register-saving convention.

**Rejected: hand-written `isr0..isr31` plus `irq0..irq15`.** This is what the
source project had, and what most tutorials show. Two files with near-identical
tails that drift apart, and 200 vectors left pointing at nothing.

**Cost.** 2.7 KiB of stubs and a 1 KiB table — about 4 KiB of a 94 KiB image.
The `%rep` block is less immediately readable than a literal list.

**Why.** There is exactly one place the frame layout is defined, so exactly one
place it can be wrong. That matters here specifically: the source project's
version of this code never pushed the frame pointer, so every handler received
`0x10` as its `struct regs *`. One definition, plus a test that executes
`int $0x03` to exercise the whole path, makes that class of bug non-recurring.

---

## 5. Preemption after the end-of-interrupt, never before

**Decision.** The timer handler sets a flag. The context switch happens at the
tail of `interrupt_dispatch()`, after `irq_dispatch()` has sent the PIC its
EOI.

**Rejected: switching inside the timer handler.** Shorter and more obvious.

**The failure it avoids.** Switching before the EOI parks the outgoing task
mid-handler with the interrupt still in service. The incoming task resumes
wherever it last stopped — and if that was `sched_yield()` rather than an
interrupt, there is no pending EOI anywhere in its stack, so the PIC never
learns the timer interrupt finished and stops delivering IRQ 0 forever. The
machine keeps running and time stops.

**Cost.** The scheduler's entry point is split in two (`sched_tick` requests,
`sched_preempt` performs) and the ordering constraint lives in a comment rather
than in the type system. Both halves say why.

---

## 6. The boot context becomes the idle task

**Decision.** `sched_init()` adopts the already-running context as pid 0;
`kmain()` ends by calling `sched_start()`, which is the idle loop.

**Rejected: a dedicated idle task plus a "first switch" path.** Needs an
allocated stack and a special case to switch *into* the scheduler from code
that is not yet a task.

**Cost.** Pid 0 has no heap-allocated stack, so the reaper and `ps` have to
tolerate `stack_base == NULL`.

**Why.** There is always exactly one runnable task, so `pick_next()` never has
to answer "what if nothing can run", and there is no first-switch special
case. Reaping exited tasks also has an obviously-safe home: the idle task is
the one context guaranteed not to be standing on the stack it is freeing.

---

## 7. The heap's footer is 8 bytes for a 4-byte magic

**Decision.** `FTR` is 8, with a `_Static_assert` tying it to `HEAP_ALIGN`.

**Why not 4.** Then `HDR + FTR` is 28. Splitting a block puts the next header
28 bytes along and its payload at a 4-byte boundary, so a `u64` or `double` in
a `kmalloc`'d struct straddles a cache line and, on other architectures,
faults.

**How it was found.** The in-kernel heap suite asserts
`IS_ALIGNED((u32)a, 8)`. It failed on the first full test run, 1 of 57 checks,
with the expression printed. The static assertions now make the invariant
explicit so it cannot regress quietly.

**Cost.** Four bytes per allocation.

---

## 8. Guard magics instead of a redzone allocator

**Decision.** Every block carries a header magic and a footer magic, validated
on every `kfree`, `krealloc` and `heap_check`. Allocated and free blocks use
different header magics.

**Rejected: no checking.** Faster, and standard for a hobby kernel.

**Rejected: full redzones and quarantine (ASAN-style).** Much stronger, and far
more machinery than this kernel's size justifies.

**Cost.** 32 bytes of overhead per allocation and a few instructions per free.

**Why.** The characteristic kernel heap bug is a one-byte overrun into the
*next* block's header; the crash then happens in an unrelated allocation
minutes later and points at innocent code. A footer check turns that into an
immediate panic naming the guilty pointer and its size.

---

## 9. Serial first, and serial everywhere

**Decision.** `serial_init()` is the first line of `kmain()`. Both bootloader
stages write to COM1 directly as well as to the BIOS teletype. The shell reads
from it.

**Cost.** About 40 bytes in each bootloader stage, and transmit is polled —
`console_write` holds interrupts off for the duration of a write. On QEMU the
emulated UART accepts bytes immediately so this is free; on real hardware at
115200 it is ~87 µs per character, which can cost timer ticks during heavy
logging. A transmit ring buffer is the fix, and is in the roadmap.

**Why.** It is the only output channel that works before memory management,
before interrupts, and before the display — so it is the only one that can
report a failure in any of them. It is also what makes the project testable at
all: the entire boot, from the MBR's first instruction onward, becomes a
transcript CI can assert on. An early failure in this project was invisible
precisely because the bootloader only spoke to a screen nobody was watching.

---

## 10. Identity-mapped at 1 MiB, not higher-half

**Decision.** The kernel's virtual addresses equal its physical ones.

**Rejected: relocating to `0xC0000000`.** What a production kernel does, and
what this one would need before it could host real processes.

**Cost.** Userspace cannot have a clean private 3 GiB; every address space has
to share its bottom with the kernel. This is the main structural limit on the
project.

**Why, for now.** Both boot paths stay simple — stage 2 copies segments to
physical addresses and jumps, with no early page tables and no trampoline
running at an address it was not linked for. Physical addresses from the
firmware stay directly usable. The `CR0.PG` transition changes nothing
observable, which is what makes it debuggable rather than a cliff.

This is a staging decision, not a belief about what is correct, and it is
written up as such in [MEMORY.md](MEMORY.md) and first in the roadmap.

---

## 11. The kernel links against nothing

**Decision.** `div64.c` implements `__udivdi3`, `__umoddi3`, `__divdi3`,
`__moddi3` and `__udivmoddi4` by restoring division, so no libgcc is linked.

**Rejected: `-lgcc`.** The normal, correct answer for a real project.

**Cost.** 64 iterations per 64-bit division. Division appears in `printf` and a
handful of time conversions, never in a hot path, and there is a 32-bit fast
path for operands that fit.

**Why here.** "What is in my kernel" has a one-sentence answer: code from this
repository plus the compiler's freestanding headers. For a project whose
purpose is to demonstrate understanding, not depending on a library whose
internals are out of scope is worth 64 shifts.

---

## 12. `spinlock_t` that does not spin

**Decision.** `spin_lock()` saves and clears the interrupt flag; `spin_unlock()`
restores it. The `locked` counter is maintained and a recursive acquisition
panics.

**Rejected: calling it `irq_save`/`irq_restore` and nothing else.** Honest, but
it does not leave a place for the real thing to go.

**Rejected: a real test-and-set spinlock.** On a uniprocessor there is nothing
to contend with. It would be theatre.

**Cost.** The name promises something it does not yet do, which the header
comment addresses directly.

**Why.** On a uniprocessor the only concurrent context is an interrupt handler,
so mutual exclusion *is* masking interrupts. Keeping the counter means a
recursive acquisition — which would deadlock a real spinlock — is caught today
rather than discovered the week SMP support is added.

---

## 13. Tests that run inside the kernel, not only beside it

**Decision.** Four layers: host unit tests for pure code, a pre-boot image
validator, in-kernel suites against real hardware state, and QEMU boot
scenarios.

**Rejected: boot it and look.** The default for this kind of project, and the
reason the source project's README could claim features that did not exist.

**Cost.** `ktest.c` is about 450 lines, and the test images and harness are
another 600 — roughly a tenth of the project.

**Why.** Three of the bugs fixed during this merge were in code that "worked"
in the sense that the machine booted. A heap that silently misaligns payloads,
an interrupt stub that passes the wrong pointer, and a memory map that is
never validated all produce a kernel that reaches a shell prompt. The only
thing that catches them is an assertion.

The test suite earned its keep on the first full run: it caught the heap
alignment bug, a double free in its own `vmm` test, and `int3` having no
handler — all in one pass.

---

## 14. The command line goes in stage 2's header

**Decision.** `tools/mkimage.py` patches a 96-byte command-line field at a
fixed offset in stage 2, and the Makefile builds three images from one kernel:
interactive, self-test, and quiet-interactive.

**Rejected: a compile-time constant.** Then testing `autotest` means rebuilding
the kernel, and CI tests a different binary than the one that ships.

**Rejected: an interactive prompt in stage 2.** Needs keyboard handling in real
mode and does not help CI at all.

**Cost.** A fixed-size field, so stage 2's entry point had to move to offset
112 and stage 1 has to agree. A build-time `%if` enforces that they do:

```asm
%if ($ - $$) != 112
  %error "stage2 entry point is not at offset 112; update stage1.asm to match"
%endif
```

**Why.** The same kernel binary is interactive, self-testing and
serial-only — the images differ only in 96 bytes of stage 2.
