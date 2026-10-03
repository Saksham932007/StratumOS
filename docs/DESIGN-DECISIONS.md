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

---

## 15. Copy-on-write marks *both* copies, and stores the mark in the PTE

**Decision.** `vmm_clone_current()` clears `PTE_WRITE` and sets `PTE_COW` —
software bit 10 — in the parent's page table entry as well as the child's, and
bumps a per-frame reference count the PMM keeps in one byte per frame.

**Rejected: marking only the child.** It looks sufficient, and it is wrong in a
way that will not show up in a test that forks once. The parent stays writable,
so the parent's next write goes straight through into a page the child is still
reading. The two processes share a page until the *child* writes, at which
point they stop — so the symptom is a parent whose data is occasionally,
quietly wrong, with no fault and no log line anywhere.

**Rejected: a side table of COW pages.** A hash of `(address space, page)`
keeps the page tables clean, but it is a second structure to keep in step with
the first, and the fault handler has to consult it on every write fault
rather than reading the entry it already walked to. x86 leaves bits 9–11 of a
PTE to software precisely so that this does not have to be invented.

**Rejected: no reference count — copy on every fault.** Simpler, and it leaks.
When one of two sharers exits, the survivor's page is still COW; without a
count, the next write allocates a frame, copies 4 KiB and drops the original,
all to arrive at the page it already had. A process forking in a loop pays
that cost every time. With a count, `refs <= 1` hands the write bit back and
copies nothing.

**Cost.** One byte per frame — 32 KiB per GiB of RAM — allocated next to the
frame bitmap, plus the discipline that every frame-sharing path has to call
`pmm_frame_ref()`. The count saturates at 255 instead of wrapping, because a
counter that has to fail should leak a page rather than free one that is
still in use.

**Why.** The bit is already there, the fault handler has already walked to the
entry, and the reference count is what makes the whole thing safe rather than
merely working. The `vmspace` suite checks all three properties — both copies
marked, the count rising and falling, the frame count returning to where it
started — because each of them fails silently.

---

## 16. `exec` returns by rewriting the trap frame

**Decision.** `usermode_exec()` does not return a value. It sets `r->eip` to
the new image's entry point and `r->user_esp` to the top of a fresh stack, and
the syscall's own `IRET` delivers control into the new program. The dispatcher
`return`s without touching `r->eax`.

**Rejected: returning to the caller and jumping afterwards.** There is nothing
to return to. The code that executed `int 0x80` was in the image that `exec`
just unmapped, so the return address in the trap frame points at a page that
no longer exists.

**Rejected: a dedicated "enter user mode" path, as the first `exec` uses.**
`usermode_enter()` forges an inter-privilege `IRET` frame from scratch and
works fine for a task that has never been in ring 3. Using it here would mean
abandoning the kernel stack frame the syscall is standing on, and two separate
pieces of code that know how to get from ring 0 to ring 3. The trap frame is
already exactly the right shape; the only honest thing to do with it is edit
it.

**Cost.** `SYS_EXEC` has to receive `struct regs *`, like `SYS_FORK`, and the
dispatcher needs a `return` rather than a `break` in that one case — a quiet
asymmetry that a comment has to carry. And the ordering rules become load
bearing: the program name must be copied out of user memory *before* the
address space is torn down, because the string lives in the image being
replaced.

**Why.** It is the same mechanism `fork` uses from the other direction.
`fork_trampoline` jumps into `isr_restore_and_return` so a brand-new child
returns to user space through the identical instructions a page fault does;
`exec` edits the frame those instructions will read. One restore path, two
callers, nothing duplicated.

---

## 17. A dead task's resources are released by two paths, not one

**Decision.** `release_task_resources()` is idempotent and called from both the
idle task's reaper and from `wait()`. The task *slot* stays `TASK_ZOMBIE`
holding the exit code until a parent collects it.

**Rejected: the reaper alone.** This was the first implementation, and it had a
real bug. `wait()` simply marked the slot `TASK_UNUSED`, and `wait()` usually
wins the race — the parent becomes runnable the moment its child exits. So the
slot went back into circulation while the task was **still linked into the run
queue**, holding a stack and a page directory. The next `task_create()` handed
out that slot and spliced it into the list a second time.

**Rejected: `wait()` alone.** Then a process whose parent never calls `wait()`
holds 16 KiB of kernel stack and a whole address space forever. Orphans are
normal, not exceptional.

**Rejected: freeing in `task_exit()`.** A task cannot free the stack it is
standing on or the address space it is running in. That is the constraint the
whole design follows from, and it is why a zombie state exists at all.

**Cost.** A `resources_freed` flag, set inside the same interrupts-off window
that unlinks the task, and the discipline that both callers go through one
function. The function has to refuse to act on `current`.

**Why.** The two lifetimes are genuinely different: memory should come back as
soon as anyone notices, and an exit status is owed to a specific process until
it asks. Trying to serve both with one trigger is what produced the bug.

---

## 18. All 255 kernel page tables, up front

**Decision.** `vmm_init()` allocates a page table for every page directory
slot in the kernel half — 255 of them, 1020 KiB — before the first address
space other than the kernel's can exist. `ensure_table()` panics if a kernel
table is created later.

**Rejected: reserving only the regions in use.** The linear map, the heap's
maximum window, the temporary slots and the stack region come to 21 slots and
84 KiB: twelve times cheaper, and it works. What it leaves behind is a list
that has to be kept in step with the address-space layout, and the failure
mode for forgetting an entry is this bug: a kernel mapping that exists in
whichever address space happened to be current when it was made, and faults
in every other. It appears when the wrong process is scheduled, somewhere
unrelated to the code that caused it.

That is not hypothetical. The heap window spans slots 832 to 847 and
`heap_init()` maps 1 MiB, so only slot 832 existed at boot; growing the heap
past 4 MiB creates slot 833, and a process forked before the growth would
never have seen it. Finding it required writing the assertion.

**Rejected: propagating a new kernel directory entry to every live address
space.** This is what the bug actually asks for, and it is what some kernels
do. It needs a registry of every address space, a lock around it, and a
correctness argument about a process being forked while the propagation is
half done. Pre-allocating needs none of those and cannot be got wrong.

**Cost.** 1020 KiB of RAM on a machine with 127 MiB, 0.8%. The number does
not grow with memory, only with the size of the kernel half, so it is the
same 1 MiB on a 4 GiB machine.

**Why.** It converts a class of bug whose symptom is a page fault in an
unrelated process into something that cannot happen. The assertion that
replaces it fires at the moment the mistake is made, names the directory slot
and the address it covers, and says what to do about it.

---

## 19. SMAP, and therefore an enumerable list of user accesses

**Decision.** `CR4.SMAP` is enabled where the CPU has it, and the five places
the kernel deliberately touches user memory are wrapped in
`user_access_begin()`/`user_access_end()` — `stac` and `clac`.

**Rejected: SMEP only.** SMEP is free and uncontroversial; the kernel never
executes user memory, so enabling it costs nothing. Stopping there would have
been the easy half. SMAP is the one that changes the code, and the change is
the benefit: after it, the places where the kernel dereferences a ring-3
pointer are a list of five rather than "anywhere in the kernel".

**Rejected: a copy_from_user/copy_to_user pair instead.** The usual shape, and
better engineering in a bigger kernel — every user access goes through two
functions and the windows are invisible. Here it would have hidden what this
change exists to show: that `cow_fault` reads a user page, that
`elf_load_user` writes several, that `sys_write` hands a user pointer
straight to the console layer. Making each site declare itself is worse
abstraction and better evidence.

**Cost.** Two instructions per access, a flag check before each so that
`stac` on a CPU without SMAP is not a `#UD`, and the discipline that a new
user access without a window is a page fault rather than a code review
comment. Also a nesting rule: `upoke()` in the test suite opens a window and
writes to a copy-on-write page, which faults into `cow_fault`, which opens
another. That works because `AC` is part of `EFLAGS` and the handler's `IRET`
restores it — but it is a thing to know.

**Why.** It caught a bug on its first boot. Not in the kernel — in the
`vmspace` suite, which writes to a user page from ring 0 to prove
copy-on-write works and had never declared the access. A mitigation that
finds a defect in the same change that introduces it has earned its two
instructions.

And it is read back, not assumed: `harden_init()` writes CR4 and then reads
it, because a hypervisor may advertise SMAP in CPUID and refuse the bit, and
a kernel that believed SMAP was on would skip the `stac` pairs and fault on
its first legitimate user access.

---

## 20. A guard page *and* a canary under every stack

**Decision.** Task stacks moved out of the heap into a region where each one
is preceded by a permanently unmapped page, and the lowest usable word holds
a magic that `switch_to()` checks on every switch.

**Rejected: the heap, as before.** `kmalloc_aligned(TASK_STACK_SIZE, 16)` put
each stack next to another allocation's header. An overflow ate that header
silently, and the complaint arrived later from an unrelated `kfree`
complaining about a guard magic — a diagnosis three steps removed from the
cause.

**Rejected: the guard page alone.** It is the stronger of the two: the
hardware enforces it, it costs nothing at runtime, and the resulting page
fault names the guard page and the function that overran. It catches a stack
that *grew* too far. It does not catch a wild write that landed past it — a
`memcpy` with a bad length jumps over the guard entirely and lands below.

**Rejected: the canary alone.** Then an overflow is detected at the next
context switch rather than at the instruction that caused it, which is the
difference between a backtrace naming the culprit and a backtrace naming the
scheduler.

**Cost.** One page of address space per task that is never backed by a frame,
a fixed region instead of a general allocation, and one load plus one compare
per context switch. The slot index is the task's index in `tasks[]`, which is
unique and stable, so there is no second allocator.

**Why.** The two mechanisms fail differently, and the cheap one covers the
expensive one's blind spot. The canary check runs on the task being switched
*away* from — the one that has just been running — so the panic names the
culprit rather than its successor.

---

## 21. Two CI scenarios whose expected result is a panic

**Decision.** `fault-text` and `fault-stackguard` boot a kernel, type one
`fault` command, and require QEMU to exit 35 — the panic code — with the
serial log naming the right faulting address, reason and region.

**Rejected: asserting the configuration and stopping there.** The `harden`
suite already checks that the page table entries covering `.text` have no
write bit and that the page below each stack is unmapped. That is half of a
mitigation. The other half is whether the CPU acts on it, and the correct
outcome of testing that is a dead kernel, which no passing test can contain.

**Rejected: an expected-fault mechanism in the page-fault handler.** A flag
saying "a fault at this address is expected, skip the instruction and carry
on" would let both halves live in the `harden` suite. It needs
instruction-length decoding to know where to resume, which is a disassembler
in the fault path — a large amount of fragile machinery, in the one place
where a bug is hardest to diagnose.

**Cost.** Two extra QEMU boots in CI, and a harness path that treats a panic
as success. Each is driven through the serial console a character at a time
because there is no prompt to wait for afterwards, and the session keeps
draining for five seconds past the panic banner, because the register dump,
the resolved symbol and the call trace all arrive after it.

**Why.** A clean exit from `fault text` would mean the kernel's text is
writable again — exactly the regression that is invisible to every other test
in the suite. Asserting on the exit code is what makes the scenario a test of
the CPU rather than of a log line.

---

## 22. One disk, with a real partition table

**Decision.** The kernel and the filesystem share a single disk image. Sector
0 is both stage 1 and a real MBR partition table, and the FAT16 partition
starts at LBA 2048 — 1 MiB in, aligned the way every partitioning tool has
aligned since about 2010.

**Rejected: a second drive.** `-drive index=1` carrying the filesystem would
have been less work in the Makefile and in the kernel, and it would have
tested less. A filesystem whose partition starts at LBA 0 works whether or not
the block layer is adding the partition's offset, so the bug that layer exists
to prevent would have been invisible. With one disk, the partition table has
to be parsed correctly and every filesystem read has to be offset, and the
`storage` suite can assert the two agree: a read of `hd0p1` block 0 must
return the same bytes as a read of `hd0` at the partition's first LBA.

**Rejected: no partition table, a bare filesystem after the kernel.** Then the
kernel needs to be told where the filesystem is, which means another field in
stage 2's header — a private convention where a standard already exists, and
an image no other tool could read. The current image mounts under `mdir` and
`mtype` unchanged, which is how the formatter was checked in the first place.

**Cost.** Stage 1 has 446 usable bytes rather than 510, because the table
starts at 446. It uses 315. Both `mkimage.py` and `check-image.py` verify that
rather than assuming it, because the assembler cannot: as far as NASM is
concerned the whole 510 bytes are free.

**Why.** The partition table is the piece that makes this a disk rather than a
file with a kernel at the front, and parsing one is a thing an operating
system does. Only the LBA fields are written and only the LBA fields are read;
the CHS fields exist for pre-1994 BIOSes, cannot describe anything past 8 GiB,
and are wrong on most real disks.

---

## 23. The FAT driver's cache is split by purpose, because measuring it said so

**Decision.** Two cached sectors: one slot for the allocation table, one for
directory entries and file contents.

**Rejected: one slot.** This was the first implementation, and it shipped with
a comment confidently explaining that a second entry would buy nothing for
either access pattern. The comment was wrong, and the test suite is what said
so: reading a file in 64-byte chunks touches each 512-byte sector eight times,
so seven of every eight reads should be hits. Fewer than half were.

The reason, once measured, is obvious in hindsight. Reading a file alternates
between FAT sectors — to follow the chain — and data sectors, to copy bytes
out. With one slot each evicted the other on every step. 360 sector reads
against 321 hits became 150 reads against 1224 hits once the slots were
separated: 47% to 89% on the identical workload.

**Rejected: an LRU pair.** Two slots with a replacement policy would behave
the same here and would need the policy. Splitting by *purpose* needs none,
and it makes it structurally impossible for one stream to starve the other.

**Rejected: more slots.** There is no third stream. A directory entry and a
file's contents are both "data", and only one is being read at a time.

**Cost.** 1 KiB of static buffer instead of 512 bytes, and a `slot` parameter
on `read_sector()` that every caller has to get right — which is exactly two
places: `fat_next()` passes `CACHE_FAT`, everything else passes
`CACHE_DATA`.

**Why.** It is the one decision in this file that was made by measurement
rather than by reasoning, and the reasoning had been wrong. The test that
caught it was written to check the mechanism rather than the outcome: both
versions read the right bytes, and only one of them was doing a sensible
amount of work to get them.

---

## 24. ATA by polled programmed I/O

**Decision.** The disk driver moves data through the data register, 256 16-bit
reads per sector, and spins on the status register rather than sleeping on
IRQ 14. It even sets `nIEN` so the drive does not assert its interrupt at all.

**Rejected: bus-mastering DMA.** The fast answer, and the one a real kernel
uses. It needs a physical region descriptor table, a scatter list of physical
addresses, the controller's own BAR from PCI config space, and a completion
interrupt — four new things, none of which can be debugged until reading a
sector works at all.

**Rejected: interrupt-driven PIO.** Halfway, and the worst of both. It needs a
state machine to remember which sector of which request is in flight, a
decision about what happens when a request completes while its handler is
still being installed, and an answer for two channels sharing a line. All of
that for a driver that is still copying with the CPU.

**Cost.** A read burns the rest of a timeslice. On real hardware that is the
difference between a disk-bound workload working and not; under emulation it
is free, which is honest to say rather than hide. Every wait is bounded at 3
million status reads, because a drive that never clears BSY would otherwise
hang the kernel at boot with no message — the least debuggable failure a
driver can have.

**Why.** The read path is a straight line that can be read top to bottom, and
the two things that are actually easy to get wrong — the 400 ns settling delay
after a drive select, and one DRQ handshake *per sector* rather than per
command — are visible in it rather than buried in a state machine. The second
of those is the nastiest bug available here: transferring every sector after a
single wait works under emulation and fails on hardware. The test suite
therefore reads two sectors in one command, requires the result to equal two
single-sector reads, and requires the two sectors to *differ* — so the
comparison cannot pass on a driver that returned the same sector twice.

---

## 25. Errors the tests provoke are counted, not silenced

**Decision.** `log_expect_errors(true)` opens a window in which `ERROR`-level
lines are counted instead of printed. A test asserts the count afterwards.

**Rejected: softening the messages.** A read past the end of a disk, a
partition read that leaves its partition, an ELF with a corrupt header — each
of those is a genuine error when it happens for real, and CI is right to treat
an unexpected `ERROR` line as a failure. Downgrading them to warnings to keep
the test suite quiet would be letting the tests damage the thing being tested.

**Rejected: a FORBIDDEN exception for the known strings.** Then the exception
list grows with every test, and it stops distinguishing an error the suite
provoked from the same error happening somewhere it should not.

**Rejected: suppressing instead of counting.** Simpler, and strictly weaker.
Five refusals must produce five complaints: a driver that refuses *silently*
passes a suppression check and fails this one. A refusal nobody can see is
nearly as bad as no refusal at all.

**Cost.** A global flag and a counter in the logging layer, touched only by
test code, plus the discipline that a window has to be closed. It covers only
the two highest levels, and only while a test asks for it.

**Why.** The two requirements — "CI must fail on an unexpected error" and
"tests must provoke errors" — look like they conflict and do not. The window
makes the expectation explicit at the call site, and asserting the count turns
it from an exemption into an additional assertion.

---

## 26. The current processor comes from the task register, not the APIC

**Decision.** `smp_cpu_index()` executes `str`, reads the task register's
selector, and subtracts the base of the per-CPU TSS descriptors.

**Rejected: reading the local APIC's id register.** The obvious answer, and
what the first version did. It is an uncached access to a device, and it sits
on the context-switch path because `tss_set_kernel_stack()` has to write
*this* processor's TSS. Measured:

```
                 APIC read      task register
  cpu-index      453 cycles          3.6        125x
  spinlock       580 cycles         68           8.5x   (two per acquisition)
  ctxsw          886 cycles        580           1.5x
```

Medians under emulation, so the absolute figures are the emulator's — but the
ratio is real, and an uncontended spinlock costing about as much as a system
call was not a trade anyone would have chosen deliberately.

**Rejected: a per-CPU GDT descriptor reached as `%gs:offset`.** What Linux
does, and better still, because it yields the whole `struct cpu` rather than
an index. It needs the interrupt stubs to load a per-CPU GS on every kernel
entry — which needs the processor already identified, and `str` is exactly how
you would break that circle. Worth doing when there is a reason; there is not
one yet.

**Cost.** The index is derived from a GDT layout, so `SMP_MAX_CPUS` is baked
into the descriptor table and a processor's index can never be reassigned —
which is why failed processors leave gaps rather than being compacted away.
And it answers 0 before any `ltr` has run, which has to be correct rather than
merely harmless, because `gdt_init()` itself calls it.

**Why.** The task register was already per-CPU and already distinct on every
processor, because each one needs its own TSS for `ss0`/`esp0`. The identity
the kernel was obliged to set up anyway turned out to be the cheapest one to
read back.

---

## 27. LINT0 takes ExtINT on the boot processor, and is masked everywhere else

**Decision.** `configure_local()` masks every local vector table entry the
kernel does not handle — except LINT0 and LINT1, which on the boot processor
become ExtINT and NMI.

**Rejected: masking them too.** This is what the first version did, and it
reads as obviously correct: an unmasked LVT entry left over from firmware
delivers an interrupt on a vector nothing is installed for, so masking what
you do not handle is the right instinct.

It stopped the kernel's timer dead. Before the local APIC is enabled the 8259
pair drives the processor's INTR pin directly; enabling it puts that pin
behind LINT0, in the arrangement the specification calls virtual wire mode. A
masked LINT0 means no 8259 interrupt reaches the processor at all — and this
kernel's timer, keyboard and serial input all arrive that way.

The symptom is worth recording because it is so quiet: the kernel booted
perfectly, printed every line of its startup, and hung before the first test
suite with **every log line sharing one timestamp**. Nothing said "the timer
stopped"; the clock simply never advanced.

**Rejected: moving the timer to the I/O APIC first.** That is the real answer
and it is a bigger change — the MADT's interrupt source overrides have to be
honoured, which on QEMU means knowing that IRQ 0 arrives as GSI 2. Those
entries are parsed and reported; programming the I/O APIC is a separate item.

**Cost.** One of the eight processors is special, which is a thing the code
now has to say out loud: `configure_local(bool is_bsp)`. Application
processors mask both pins, because the 8259 has one output, it is already
going to the boot processor, and a second processor accepting ExtINT would
race it for the same interrupt and acknowledge a controller it was not talking
to.

**Why.** The `smp` suite now asserts LINT0 is ExtINT and unmasked **and** that
the timer is advancing. The second is the property that mattered; the first is
only how it is achieved, and a test that checked the configuration alone would
have passed on a kernel whose clock was stopped.

---

## 28. Page table entries are volatile, and map/unmap carry barriers

**Decision.** `pd_entries()` and `pt_entries()` return `volatile u32 *`, and
`temp_map()`/`temp_unmap()` each contain an explicit compiler barrier.

**Rejected: plain pointers, which is what this file had for three phases.**
The disassembly of what GCC produced:

```
  mov    %eax,0xfff3c000    <- temp_map:   write the PTE  (mapping ON)
  invlpg (%ecx)
  movl   $0x0,0xfff3c000    <- temp_unmap: clear the PTE  (mapping OFF)  *** HOISTED ***
  mov    0xcf000000,%eax    <- read through a mapping that is now gone   -> #PF
  movl   $0x0,0xcf000000
  invlpg (%ecx)             <- temp_unmap's invlpg, left behind
```

The store that tore down the mapping was hoisted above the code still using
it. The compiler is entitled to: it sees a store to one absolute address and
accesses to another, has no way to know the first changes where the second
*goes*, and reorders them freely.

**Rejected: relying on `invlpg`'s memory clobber.** It was already there, and
it is not enough. A `"memory"` clobber constrains ordering relative to *the
asm statement*, and each store did stay on its own side of its own `invlpg`.
Nothing connected `temp_unmap`'s store to the accesses that happened between
the two asm statements.

**Rejected: volatile alone.** It keeps page-table accesses ordered relative to
each other, which stops them being merged or elided — but it says nothing
about ordinary memory accesses, which is precisely what reads through the
mapping are.

**Cost.** Every local holding one of these pointers has to carry the
qualifier, and `memset` cannot be used on a page table any more — zeroing a
fresh one is now an explicit loop, because casting the qualifier away is
exactly the mistake being paid for. Two barriers that emit no instructions.

**Why.** The bug had been latent since the copy-on-write work:
`vmm_clone_current`, `cow_fault` and `vmm_destroy_address_space` all use the
same pattern and happened to survive whatever GCC decided about their
surrounding code. That is the worst kind of luck — the code was wrong for
three phases and the tests all passed. The `bootpd` suite is now the
regression test, and it fails immediately and fatally rather than subtly if
either half of the fix is removed.

---

## 29. The fuzz targets compile the real kernel sources, not a host-testable copy

**Decision.** `build/fuzz/fuzz_elf` links `kernel/core/elf.c` — byte for byte
the file that boots — against a shim that supplies what a kernel would:
`kmalloc`, a page table, a console, a block device.

**Rejected: extracting the parsers into host-testable copies.** The common
shape, and the reason for rejecting it is the whole argument for the harness:
a fuzzer that finds bugs in a rewritten copy of a parser is finding bugs in
the rewrite. The copy drifts from the original, the drift is invisible because
both pass their own tests, and the day it matters is the day the fuzzer reports
clean on code that no longer resembles what ships.

**Rejected: an in-kernel fuzzer, generating inputs under QEMU.** No
AddressSanitizer, no coverage feedback, no corpus minimisation, and a
one-byte heap overrun shows up as a triple fault twenty thousand inputs later
instead of a backtrace at the instruction that did it. The ring-3 syscall
fuzzer is in-kernel precisely because its target cannot be reached any other
way — and it is blind, which is what that costs.

**Cost.** The shim has to be convincing, and that is where the design went.
Three things in particular:

- `vmm_alloc_at()` has to `mmap(MAP_FIXED_NOREPLACE)` at the address the
  kernel asked for, because `elf_load_user()` writes through that address as a
  raw pointer. A stub returning `true` would turn its `memcpy` into a wild
  write into libFuzzer's own state — a spurious crash at best, a silent
  corruption at worst. This is also why the targets are `-m32`: a user ELF
  asking for `0x00400000` can only be honoured in a 32-bit address space.
- `cli`/`sti`/`hlt`/`invlpg` are stubbed in `arch/io.h` behind a macro the
  kernel build never defines, because `heap.c` takes interrupt-safe locks and
  `cli` in a user process is an immediate `SIGSEGV`. The interrupt flag is
  *modelled* rather than discarded, so `irq_save`/`irq_restore` still nest and
  a lock that forgets to restore is still findable.
- `phys_to_virt` is routed through the shim, so a parser following a pointer
  out of a table reads the fuzzer's bytes rather than the host's memory.

Each of those is a small, auditable accommodation in a header, next to a
comment explaining it. The alternative was a parallel copy of every file that
touches them, which is how a test suite stops testing the code that ships.

**Why.** Five bugs, all pre-existing, four of them in code that the in-kernel
suites covered and passed. The ones worth the trouble are the two that no
assertion about return values could have found: `memmove` violating `memcpy`'s
non-overlap contract (reported by ASan's interceptor, which enforces exactly
that contract) and the ELF loader leaking every mapped page on a mid-segment
failure (reported by the target's own "nothing may be left mapped" assertion).
Both were invisible to tests that checked the parsers answered correctly,
because they did.

See `docs/FUZZING.md`.

---

## 30. Warnings a user program can provoke are rate limited in the logging layer

**Decision.** `pr_warn_ratelimited()` allows five lines per second per call
site, counts what it drops, and reports the count when the window closes. The
eight warnings reachable from ring 3 use it.

**Rejected: leaving it alone.** The syscall fuzzer made the case. Every bad
pointer it passed produced a `WARN` line on a shared, slow, serial device, so
a program calling `write()` with a bad pointer in a loop makes the kernel print
on its behalf as fast as the loop goes. Tens of thousands of lines, during
which nothing else got a word in: a denial of service by an unprivileged
process against the one channel an operator uses to see what the machine is
doing. The console was the resource, and nothing was accounting for it.

**Rejected: lowering these messages to DEBUG.** It makes the flood
conditional on a log level, which is the same bug with an extra step — and it
removes the diagnostic from the default transcript, where it is genuinely
useful: `pid 7 passed an unreadable buffer 0xc0100000+16 to write()` is often
the whole explanation of a user-space bug.

**Rejected: one limiter for the whole kernel.** Then a flood of one warning
hides every other, which is worse than the flood: an attacker picks the
message that drowns the one you needed. Each call site gets its own, which is
what makes the static inside the macro the right shape.

**Cost.** Three things, each stated where it bites:

- The counters are plain words, not atomics. Under SMP two processors can race
  and allow a line more or fewer than the burst. The cost of being wrong is one
  line; taking a lock would put the console's lock ordering underneath every
  warning in the kernel.
- The limiter has to stand aside while a test's expected-error window is open
  (decision 25), because a test that counts error lines would otherwise count
  the wrong number. One `if` at the top of `log_ratelimit_allow()`.
- A burst of five is a guess. It is enough that the boot transcript and the
  `run-tests.py` assertions still see what they look for, and few enough that
  the limiter visibly engages during a fuzz run — which the `syscall-fuzz`
  scenario now asserts, because a limiter that never fires is a limiter nobody
  has tested.

**Why.** The alternative is a kernel whose console can be taken by any process
that can make a system call fail, and the fix belongs in the logging layer: a
`pr_warn` on a user-reachable path is a resource the kernel hands out, and
resources the kernel hands out get accounted for.

---

## 31. Long mode is a tested transition, not a half-finished port

**Decision.** The kernel can drive the boot processor from 32-bit protected
mode into 64-bit long mode, run a payload there that proves it, and come back
with the 32-bit kernel still running. The kernel itself stays 32-bit.

**Rejected: starting the port.** The tempting version of this phase is to
begin converting the kernel — a 64-bit IDT, `vaddr_t` widened to `u64`, the
interrupt stubs rewritten for the new calling convention — and get some way
in. It is the wrong shape of work to do partially, for a reason that is
specific rather than general: the pieces are not independently testable. A
64-bit IDT cannot be exercised without 64-bit interrupt stubs, which cannot
be exercised without a 64-bit scheduler to interrupt. A port lands as one
commit that either boots or does not, and "does not" has no diagnosis.

So the first deliverable is the thing every later piece needs and nothing
else depends on: the ability to get into the mode and back out, under test,
with a reported reason when it fails. The second 64-bit instruction this
kernel ever runs will run inside the window this phase built.

**Rejected: doing the transition at boot.** It would make the arc the project
is named for literal - real mode, protected mode, long mode, all before the
first log line. It would also put an unavoidable interrupt-free window on
every boot of every machine, in service of a demonstration, and it would make
the detection path - which is most of what runs on real 32-bit hardware -
the untested one. Detection is reported at boot; the transition happens on
demand.

**Cost.** Three things, each of which is a real limitation rather than a
rough edge:

- **No IDT is valid in the window.** Long mode's gate descriptors are 16
  bytes where the kernel's are 8, so one table cannot serve both halves of
  the transition. Interrupts are masked throughout and an NMI would be fatal.
  Acceptable for a demonstration, not for a port - which is precisely why a
  port needs its own IDT before anything else.
- **The payload is assembly, and small.** No 64-bit C, because that needs a
  second compilation target, a second linker script and a calling convention
  the rest of the kernel does not speak. What is in there is chosen to be
  *impossible* in 32-bit mode rather than merely different: a 64-bit
  immediate, a carry across bit 31 in one instruction, `r15`, RIP-relative
  addressing, and `EFER.LMA` read back from the processor.
- **It is one more thing that runs from a low physical copy.** `ap_boot.asm`
  already does this; now two files do, with the same `PHYS()` idiom and the
  same reason. A third would be an argument for a shared trampoline region
  with a real allocator rather than two hardcoded page numbers.

**Why.** Because the claim "this kernel can reach 64-bit mode" is either
tested or it is marketing, and the test is the hard part. `EFER.LMA` read
back from inside the window is the processor's own statement; a read through a
64-bit pointer at an address the 4 MiB identity map does not cover is the
four-level walk's. Both are assertions in a suite that runs on every push,
on two different processor models, with different expectations for each.

See `docs/LONGMODE.md`.

---

## 32. The long-mode probe frame is allocated above a floor

**Decision.** `pmm_alloc_frame_above(paddr_t floor)`, used by the long-mode
code to get its probe frame from above 4 MiB.

**Rejected: `pmm_alloc_frame()`, which is what the first version did.** The
probe is the strongest check in the suite: a read through a 64-bit pointer at
a physical address that *only* the four-level walk can resolve. The frame
allocator hands out frames in roughly ascending order, so the frame landed at
`0x0035B000` - 3.5 MiB, inside the 4 MiB identity map the transition runs
under, where the read would have succeeded whether the PML4 worked or not.

The test passed. It proved nothing. A warning that fired on the first run is
the only reason that is not still true, and the lesson is the one worth
keeping: a positive result from a test whose precondition was never checked
is indistinguishable from a positive result.

**Rejected: allocating repeatedly until a frame lands high enough.** It
works, needs no new API, and would hold a thousand frames at once on the way
past 4 MiB early in boot. It also hides the requirement inside a loop instead
of stating it.

**Cost.** One more entry point into the allocator, and it deliberately does
*not* move the search hint - so a caller with an unusual constraint cannot
degrade ordinary allocation for everyone else. That asymmetry is worth a
comment, which it has.

**Why.** The constraint is not a quirk of this one caller. "A frame from a
particular range" is why real kernels have memory zones: ISA DMA needs one
below 16 MiB, some devices below 4 GiB. Writing the general primitive cost
about fifteen lines more than the special case, and the warning became a hard
failure plus two assertions in the suite:

```c
KT_ASSERT(r, lm.probe_phys >= 4 * MIB);
KT_ASSERT(r, lm.probe_phys < (paddr_t)lm.identity_mib * MIB);
```

---

## 33. The network stack is verified against a packet capture, not its own log

**Decision.** The `network` CI scenario asks QEMU to dump every frame to a
pcap, and `tools/run-tests.py` parses it and recomputes the checksums with an
implementation of its own.

**Rejected: asserting on the kernel's log, like every other scenario.** It
would have passed. The driver's first working version transmitted every frame
with a source MAC of `00:00:00:00:00:00`: `struct net_device` was `const` and
its `.mac` was never filled, so the boot log printed the driver's own copy -
correctly - while `eth_output()` built frames from a second copy that was all
zeros. The peer replied to the zero address, the controller's receive filter
dropped the reply because it was not addressed to the card, and the result was
transmit working, 204 ARP requests leaving, the device's own counter
confirming them, receive completely silent, and every log line right.

No assertion inside the kernel could have found that, because every value the
kernel could compare was consistent with itself. The bug was in the one place
the kernel does not get to look: the bytes that left.

**Rejected: a loopback test.** The driver can be made to receive its own
frames, and that tests the rings without needing anything external. It also
cannot find this bug - a loopback frame with a zero source address is
accepted, because the filter only looks at the destination.

**Cost.** The harness has to understand libpcap's format and enough of
Ethernet, ARP, IPv4 and ICMP to find the frames it cares about - about 120
lines, which is real complexity in a test harness. And it has a second
checksum implementation, which is duplication *on purpose*: the two agreeing
is the whole value, so sharing the code would remove it.

**Why.** The two classic checksum bugs - a byte-order slip and mishandling an
odd trailing byte - both survive a test that compares the kernel's checksum
against the kernel's checksum. So do a dozen framing mistakes. The capture is
an external observer, and the test vectors in the `net` suite are real
headers lifted out of one, generated by somebody else's stack.

The scenario asserts specifically that no frame had a zero source hardware
address, so that particular bug can never return silently.

---

## 34. Both halves of a feature test are asserted by check count

**Decision.** The `longmode` and `net` suites each have two paths - one for a
machine with the hardware and one without - and the CI scenarios assert the
*number of checks*, not just that the suite passed.

```python
("the suite reached the hardware",
 r"ktest net: controller present, link up"),
("the net suite passed its wider path",
 r"ktest: net \.\.\. PASS \(9\d checks\)"),
```

**Rejected: asserting only that the suite passed.** A suite with a hardware
branch passes trivially when the hardware is absent, and that is exactly the
configuration CI is most likely to drift into: a scenario loses its `-device`
flag, or QEMU changes a default, and the suite quietly starts testing a third
of what it used to while every scenario stays green.

That is not hypothetical here. QEMU attaches a default e1000 unless told
otherwise, which was discovered while writing these tests - two runs intended
to exercise opposite paths both reported 98 checks, because both had a network
card. Without the count in the assertion there would have been nothing to
notice.

**Cost.** The expected count is in the test harness, so adding an assertion
to one of these suites means updating a regular expression as well. That is
deliberate friction in the right place: a changed count should need a
deliberate acknowledgement, because the alternative is a pattern loose enough
to match anything.

It is written as `9\d` and `6\d` rather than an exact number, which keeps a
single added assertion from breaking CI while still catching a branch that
took the wrong path entirely - the two paths differ by 35 checks, not by one.

**Why.** Because the thing being tested is not "does the suite pass" but
"which of its two bodies ran", and only the count can tell those apart. Each
suite also logs which path it took, so the transcript answers the question
without arithmetic.

---

## 35. The second architecture is measured, not claimed

**Decision.** `make portability` compiles every portable-by-intent kernel
source for riscv64 with `-Werror` and reports how many come out clean. The
number in `docs/PORTING.md` is generated by that tool.

**Rejected: writing the number in the document.** Every other phase of this
project added a subsystem; this one tests an assertion about the subsystems
that already existed - that most of `kernel/core` and `kernel/mm` was free of
x86. That assertion had been in `docs/ROADMAP.md` for several phases and was
a guess. Replacing one guess with one measurement written down in prose would
leave the same problem: it goes stale the first time somebody adds a file,
and nothing notices.

**Rejected: gating the build on it.** A file that fails to compile for a
second architecture is a finding, not a build break. Making it fatal would
mean either fixing `vmm.c` - which is *correctly* x86-specific and would be
replaced rather than ported - or adding it to an exclusion list, and an
exclusion list is where this kind of measurement goes to die.

**Cost.** A second definition of which files are "portable by intent", which
has to stay in step with the Makefile's `RV_SHARED`. The tool prints both the
count that compiles and the count actually linked, so overstating the result
by letting those drift apart is visible rather than silent.

**Why.** The measurement is the deliverable. The answer - 55% of the kernel
outside the architecture layer compiles unmodified, and the whole network
stack, filesystem, allocators and formatter are in that 55% - is more
interesting than the port itself, and it is only knowable by doing it.

---

## 36. paddr_t and vaddr_t are uintptr_t, and u64 is unsigned long long

**Decision.** Two typedefs in `kernel/include/kernel/types.h`.

**What they were.** `typedef u32 paddr_t; typedef u32 vaddr_t;` and
`typedef uint64_t u64;`

**What that cost.** Those three lines were the root cause of **88 of the 89**
portability errors the RISC-V port found, across seven files. Every one was
either a cast between an address and a pointer of a different width:

```
error: cast to 'void *' from smaller integer type 'u32'
```

or a format mismatch, because `uint64_t` is `unsigned long` on LP64 and every
`%llu` in the kernel therefore disagreed with its argument.

**Rejected: a `PRIu64` macro.** The standard answer to the format problem,
and it would mean threading a macro through several hundred call sites and
remembering it at every new one. Pinning `u64` to `unsigned long long` makes
one specifier correct on both architectures, and costs nothing: the two types
have identical width everywhere this kernel builds and only their *identity*
differed.

**Cost.** `uintptr_t` is wider than `u32` on a 64-bit target, so a structure
holding a `vaddr_t` changes size there - which is correct, and which would
matter if any on-disk or on-the-wire structure held one. None do: the ELF and
FAT16 structures use explicit `u32` fields because the formats specify 32
bits, and that distinction is now load-bearing rather than incidental.

**Why.** `uintptr_t` was available the whole time and is exactly the right
type. The interesting part is that nothing could have detected the problem
before the port: `paddr_t` being `u32` is invisible on i386, passes every one
of 633 assertions, and is obvious only in retrospect.

That is the argument for porting as a *testing* activity rather than a
feature one. It found a latent correctness bug in shared code that no amount
of testing on one architecture could have surfaced.

---

## 37. Portable code gets interrupt flags from <kernel/irqflags.h>

**Decision.** A header that names what portable code wants, dispatching to
whichever architecture is being built.

**What it replaced.** `kernel/mm/heap.c` opened with `#include <arch/io.h>`,
because that is where `irq_save()` and `irq_restore()` live on x86. It is
also where `inb`, `outb` and the privileged instructions live - and RISC-V
has no port I/O space at all, so there is no `arch/io.h` for it to find. The
heap failed to compile on its include line.

**Rejected: an `arch/io.h` for riscv64.** It would compile. It would also be
a header named after a facility the architecture does not have, existing only
so that a file which wants interrupt masking can find it - which is the
original mistake with a second copy.

**Rejected: leaving heap.c to include the arch header directly with an
`#ifdef`.** That puts the architecture dispatch in every file that needs it
rather than in one place, and the next such file gets it wrong.

**Cost.** One more header in the include graph, and eleven files still
include `<arch/io.h>` directly. Most of them legitimately want port I/O; the
ones that only want interrupt flags are listed as follow-up in
`docs/ROADMAP.md` rather than changed here, because doing all of them in the
same commit as the port would have obscured which change did what.

**Why.** This was the only portability failure the port found that was not a
cast of the wrong width, and it is the more interesting one because it is
structural: portable code was reaching into the architecture layer through a
door labelled with one architecture's name. The cast failures were a typo
repeated 88 times; this one was a design error, and it is the kind a second
architecture exists to expose.
