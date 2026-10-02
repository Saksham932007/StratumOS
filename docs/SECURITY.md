# Hardening

What is switched on, how each thing is enforced, and — just as important —
what is not here. The `harden` command reads every line back from the
hardware or the page tables rather than from a flag the kernel set earlier:

```
stratum> harden
Kernel/user separation
  null page       : unmapped
  CR0.WP          : set - ring 0 honours read-only pages
  kernel .text    : read-only
  kernel .rodata  : read-only
  SMEP (CR4.20)   : enabled - ring 0 cannot execute user pages
  SMAP (CR4.21)   : enabled - ring 0 cannot touch user pages without EFLAGS.AC
  declared windows: 0 user accesses so far
Stacks
  task stacks at  : 0xe0000000, 16 KiB each
  guard pages     : one unmapped page below every stack
  canaries        : checked on every context switch (0 failures)
  this task's guard page at 0xe000a000 is unmapped, as it should be
Address spaces
  kernel half     : all 255 directory slots pre-backed, so every
                    address space sees identical kernel mappings

Not yet implemented: NX (needs PAE), KASLR. See docs/SECURITY.md.
```

Every one of those lines has an assertion in the `harden` suite, and the two
that can only be proved by dying have a CI scenario each. That split is the
point of this document: a mitigation has two halves, and a test that passes
can only check one of them.

---

## Testing a mitigation

The `harden` suite checks the configuration: that the page table entries
covering `.text` and `.rodata` have no write bit, that no kernel page
directory entry has the USER bit, that the page below each task stack is
unmapped, that CR4 agrees with CPUID about SMEP and SMAP.

None of that proves the CPU acts on any of it. The correct outcome of trying
is a dead kernel, so those are separate scenarios: `fault-text` and
`fault-stackguard` each boot a kernel, type one `fault` command, and require
the panic to name the right address, the right reason and the right region.

```
stratum> fault text
writing to the kernel's own .text from ring 0...

  faulting address: 0xc0103000
  access          : write from ring 0
  reason          : the page is mapped read-only (CR0.WP applies to ring 0 too)
  region          : the kernel's own code or constants, which are read-only
...
  at  cmd_fault+0x17e
  CR0 80010011  CR2 c0103000  CR3 00101000  CR4 00300000
```

CI asserts on each of those lines **and** on QEMU's exit status being 35, the
panic code. A clean exit there would mean the write succeeded, which is
exactly the regression the scenario exists to catch.

---

## W^X for the kernel's own image

`vmm_protect_kernel_text()` removes `PTE_WRITE` from every page covering
`.text` and `.rodata` — 36 pages, as the boot log says.

This is only worth anything because `CR0.WP` is set. The x86 default is that
ring 0 may write *any* present page regardless of its write bit; `CR0.WP`
is what makes a read-only kernel page binding on the kernel. The `harden`
suite asserts the bit directly, because without it the rest of this section
is decorative.

`.data` and `.bss` stay writable, and the suite checks that too — evidence
that the right ranges were narrowed rather than everything.

`.boot` is left alone. It holds the page directory `_start` built, which the
VMM edits; it does so through the recursive window rather than through this
mapping, but there is no reason to narrow a mapping the kernel has a use for.

Both ends of each range are rounded outward to a page boundary. That is safe
because the linker aligns the following section to 4 KiB, so the rounding can
only ever cover padding.

### What this is not

It is not NX. Without PAE, a 32-bit page table entry has no
execute-disable bit, so a page that is writable is also executable. The
kernel's text is read-only and executable; the kernel's data is writable and —
as far as the hardware is concerned — executable too. Marking data
non-executable needs PAE, which means 64-bit page table entries and a
three-level walk. See [what is missing](#what-is-missing).

---

## SMEP and SMAP

Paging already says which pages ring 3 may touch. SMEP and SMAP are the other
direction: they stop the **kernel** from touching user memory by accident,
which is the direction that matters once an attacker controls what is in
those pages.

| | CR4 bit | Effect |
|---|---|---|
| SMEP | 20 | ring 0 may not *execute* from a user page |
| SMAP | 21 | ring 0 may not *read or write* a user page unless `EFLAGS.AC` is set |

SMEP is free: the kernel never executes user memory, so enabling it costs
nothing and turns a corrupted function pointer that lands in an
attacker-controlled buffer into a fault instead of a shellcode execution.

SMAP has a cost, and the cost is the point.

### The stac/clac discipline

The kernel does legitimately touch user memory. With SMAP on, each of those
places has to say so:

```c
user_access_begin();            /* stac: EFLAGS.AC = 1  */
console_write((const char *)ptr, len);
user_access_end();              /* clac: EFLAGS.AC = 0  */
```

The complete list, which is the real benefit — the places where the kernel
dereferences a ring-3 pointer are now enumerable rather than "anywhere":

| Where | Why |
|---|---|
| `sys_write` | reads the user's buffer |
| `user_copy_string` | reads a user string, one byte at a time |
| `SYS_WAIT` | writes the user's `int *status` |
| `elf_load_user` | zeroes and fills the new image's pages |
| `cow_fault` | reads the user page being copied |

Each window is as short as the access inside it. While one is open, an
*accidental* user dereference is no longer caught, so a wide window gives
back most of what the feature buys.

Nesting is fine and happens on purpose: `upoke()` in the test suite opens a
window and writes to a copy-on-write page, which faults, and `cow_fault`
opens another. `AC` is part of `EFLAGS`, so the fault pushes it and the
`IRET` at the end of the handler restores it.

### Detected, never assumed

Neither feature exists before Ivy Bridge (SMEP) or Haswell (SMAP), and an
emulator may expose neither — QEMU's default i386 model does not even
implement CPUID leaf 7. So `harden_init()` probes, sets what it finds, and
then **reads CR4 back**:

```c
write_cr4(cr4);

/* A hypervisor may advertise a feature in CPUID and refuse the CR4 bit, and
 * a kernel that then believed SMAP was on would skip the stac/clac pairs
 * and fault on its first legitimate user access. */
cr4 = read_cr4();
state.smep_enabled = (cr4 & CR4_SMEP) != 0;
state.smap_enabled = (cr4 & CR4_SMAP) != 0;
```

`stac` and `clac` are `#UD` when CPUID does not advertise SMAP, so
`user_access_begin()` checks the flag before executing anything. The opcodes
are written as bytes (`0f 01 cb`, `0f 01 ca`) because older assemblers reject
the mnemonics and this kernel has to build with whatever is on the machine.

The fallback branch is not hypothetical — it is what runs on any pre-2012
machine, and on QEMU's default CPU. CI therefore runs the test image twice:
once on the default model, which exercises the fallback, and once with
`-cpu max`, which exercises the real thing.

### It found a bug immediately

The first boot with SMAP enabled panicked. Not in the kernel — in the
`vmspace` test suite, which writes to a user page from ring 0 to prove
copy-on-write works, and had never declared the access:

```
  faulting address: 0x00400000
  access          : write from ring 0
  region          : user space
```

That is SMAP doing exactly its job, on the first attempt, against code
written by the same person who enabled it. The test now uses `upeek()` and
`upoke()`.

---

## Stack guard pages

Task stacks used to come from `kmalloc`. That put them in the heap with the
next allocation's header immediately below, so an overflowing stack silently
ate a heap block's metadata and the damage surfaced somewhere else entirely —
a `kfree` three subsystems away, complaining about a guard magic.

They now live at fixed slots in a dedicated region, each slot beginning with
a page that is deliberately left unmapped:

```
  KSTACK_BASE + i*KSTACK_SLOT                  guard page (unmapped)
  KSTACK_BASE + i*KSTACK_SLOT + PAGE_SIZE      lowest usable word, canary
  ... + PAGE_SIZE + TASK_STACK_SIZE            esp0, the top
```

Slot `i`'s guard page sits immediately above slot `i-1`'s top, so a stack that
grows the wrong way is caught as well. The slot index is the task's index in
`tasks[]`, which is unique and stable for the life of the slot — so there is
no second allocator to get wrong.

An overflow is now a page fault whose faulting address names the guard page
and whose EIP names the function that overran:

```
  faulting address: 0xe000aff0
  region          : a kernel-stack guard page - a task overran its stack
  at  cmd_fault+0x1f6
```

### And a canary anyway

The lowest usable word of every stack holds `KSTACK_CANARY`, and
`switch_to()` checks it on the task being switched *away* from — the one that
has just been running, so the panic names the culprit.

With a guard page below, this is nearly redundant, and that "nearly" is the
reason it is there. The guard page catches a stack that *grew* too far. The
canary catches a wild write that landed past it — a `memcpy` with a bad
length, which skips over the guard entirely and lands below it. One load and
one compare per context switch.

---

## Every address space sees the same kernel

An address space copies the kernel's page directory entries once, when it is
created. A kernel page table created *afterwards* therefore exists only in
whichever address space happened to be current.

This was a live bug, found by adding the assertion that catches it. The heap
window spans directory slots 832 to 847; `heap_init()` maps 1 MiB, so only
slot 832 existed at boot. Growing the heap past 4 MiB creates slot 833 — and
a process forked before that growth would never see it. The symptom would
have been a kernel page fault on a perfectly valid heap pointer, in one
process and not another, long after the allocation that caused it.

The fix is to claim a page table for **every** slot in the kernel half during
`vmm_init()`, while there is exactly one address space to put them in:

```
vmm: kernel half fully backed: 255 page tables (1020 KiB), so every address
     space sees identical kernel mappings
```

`ensure_table()` then panics if anything tries to create a kernel page table
later, which can now only mean a bug in that reservation:

```c
if (initialised && pdi >= KERNEL_PDE_FIRST)
    panic("a kernel page table for directory slot %u (covering %p) is being "
          "created after vmm_init(); it would be missing from every existing "
          "address space - add the region to vmm_reserve_kernel_tables()", ...);
```

Reserving only the regions the kernel currently uses would work and would
cost 84 KiB instead of 1020 KiB, but it leaves a list to keep in step with the
layout — and the failure mode for forgetting an entry is the bug above.
1 MiB on a 127 MiB machine buys the removal of a category.

The `harden` suite also checks that no kernel directory entry has the `USER`
bit. That matters because `ensure_table()` widens a directory entry to `USER`
when any page inside it becomes user-accessible, and a directory entry gates
its whole 4 MiB: one wrong bit would expose 4 MiB of kernel space to ring 3.

---

## The things that were already here

Listed because they belong in this document, not because they are new:

- **The null page is never mapped**, so a NULL dereference faults rather than
  landing on the real-mode interrupt vector table. The boot identity map that
  covered it is dropped in `vmm_init()`, and both the `vmm` and `harden`
  suites check page zero is gone.
- **`user_range_ok()`** validates every pointer that arrives from ring 3:
  refuses address-space wraps, refuses anything at or above
  `KERNEL_VIRT_BASE` at *either* end, and requires every page in the range to
  be present and user-accessible. The ring-3 program itself checks this from
  the untrusted side — see
  [USERSPACE.md](USERSPACE.md#the-boundary-and-testing-it-from-the-wrong-side).
- **The ELF loader validates before it maps.** No segment may touch kernel
  space or the null page, the program header table is bounds-checked with
  division so the check cannot overflow, and a rejected image leaves the
  address space untouched. `check-kernel.py` runs the same structural checks
  at build time on every embedded program.
- **The syscall gate is the only DPL 3 vector.** Every other IDT entry stays
  DPL 0, so ring 3 cannot, for example, fake a page fault.
- **The heap has guard magics** at both ends of every block, validated on
  free, and `heap_check()` walks the whole arena.
- **`sleep` is bounded** to 10 seconds and `write` to 1 KiB, so ring 3 cannot
  make the kernel look hung or pass an unbounded length.

---

## What is missing

Scope honesty, in rough order of how much each would cost.

- **NX, and therefore real W^X.** A 32-bit page table entry has no
  execute-disable bit. Getting one means PAE: 64-bit entries, a three-level
  walk, and every function in `mm/vmm.c` rewritten around a different entry
  format. It is the single largest piece of hardening still outstanding, and
  it is a project rather than a patch.
- **KASLR.** On a higher-half kernel this means relocating at load time. The
  loader already parses ELF program headers, so the mechanics exist; the work
  is emitting and applying relocations, and keeping the embedded symbol table
  correct across them.
- **Stack canaries for C functions** (`-fstack-protector`). Needs
  `__stack_chk_fail` and a per-task canary in a segment register, which in
  turn wants a per-CPU data area — so it is entangled with the SMP work.
- **UMIP** (CR4 bit 11), which stops ring 3 reading `GDT`/`IDT`/`LDT`/`TR`
  base addresses with `sgdt`/`sidt`. Cheap, and worth doing next.
- **Speculative-execution mitigations.** Nothing here addresses Meltdown,
  Spectre or their descendants. A kernel-page-table-isolation scheme would be
  interesting and is a long way outside the current scope.
- **A hardened allocator.** The heap is first-fit with guard magics, not a
  slab allocator with randomised placement and quarantined frees.
- **No secrets to protect.** There are no credentials, no cryptography and no
  network, so the threat model here is "a bug, or a hostile ring-3 program" —
  not a remote attacker. That is worth saying out loud: these mitigations are
  implemented because implementing them teaches what they do, and because a
  kernel that claims them should be able to show them working.
