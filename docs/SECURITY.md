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
W^X
  paging          : PAE, three levels, 64-bit entries
  no-execute      : EFER.NXE set; data pages are mapped bit-63 no-execute
  read-only       : .text and .rodata, enforced on ring 0 by CR0.WP
  try it          : `fault nx` jumps into a data page, `fault text`
                    writes to .text; both must fault
Address spaces
  kernel half     : one page directory, shared by pointer, so every
                    address space sees identical kernel mappings

Not yet implemented: KASLR. See docs/SECURITY.md.
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

`.boot` is left alone. It holds the paging structures `_start` built, which
the VMM edits; it does so through the self-mapping window rather than through
this mapping, but there is no reason to narrow a mapping the kernel has a use
for. It is marked no-execute like the rest of the data, though — it holds
`_start`'s code too, and that code has run for the last time.

Both ends of each range are rounded outward to a page boundary. That is safe
because the linker aligns the following section to 4 KiB, so the rounding can
only ever cover padding.

### And the other half: NX

Read-only `.text` says the kernel's code cannot be rewritten. It says nothing
about whether the kernel's *data* can be executed, and on classic 32-bit
paging there was no way to say anything about it: a 32-bit page table entry
has no execute-disable bit, so every writable page was also executable as far
as the hardware was concerned. `W^X` was half a mitigation and the boot line
said so.

Bit 63 of a PAE entry is the other half, and getting at it is why this kernel
uses PAE. The rule is one function, and the polarity is the point:

```c
static pte_t leaf_flags(u32 flags)
{
    pte_t e = (pte_t)(flags & 0xFFF) | PTE_PRESENT;

    if (nx_ok && !(flags & PTE_EXEC))
        e |= PTE_NX;

    return e;
}
```

A caller that says nothing gets a non-executable page. So the kernel is W^X
by default rather than by inspection, and `grep PTE_EXEC` is the complete
list of what is allowed to be code:

| What | Where | Why |
| --- | --- | --- |
| the kernel's `.text` | `vmm_protect_kernel_text()` | it is the kernel |
| a user segment with `PF_X` | `elf_load_user()` | the program said so |

Everything else — `.rodata`, `.data`, `.bss`, `.boot`, the heap, every kernel
and per-processor stack, the MMIO window, the temporary mapping slots, the
page-table windows, user data, every user stack — is mapped with bit 63 set.
`vmm_protect_kernel_text()` makes three passes over the kernel's linear map
and reports the count:

```
vmm: kernel .text and .rodata mapped read-only (93 pages); CR0.WP makes that binding on ring 0 too
vmm: W^X: 4039 pages of the kernel's linear map marked no-execute, so only .text is executable
```

The `harden` suite asserts it as an exclusive-or rather than as two separate
facts: across every page of the linear map, at most one of *writable* and
*executable* may be true, and the number of executable pages must equal the
size of `.text`. It also checks the current task's own kernel stack, which is
outside the linear map and is the single most useful page in the address
space to someone who can overflow one.

`vmm_protect()` **replaces** permissions instead of merging them, so a page
made writable again loses `PTE_EXEC` unless it is asked for again. The
copy-on-write handler copies bit 63 across from the old entry, so breaking a
shared page cannot quietly turn data into code — a fork-heavy program would
otherwise do exactly that, one page at a time.

### Earning the bit

On a processor where `EFER.NXE` is clear, bit 63 is **reserved**, and a
reserved bit set anywhere on the walk makes every access through that entry
fault forever. So nothing writes it on faith:

```c
cpuid_raw(0x80000001u, 0, &a, &b, &c, &d);
if (!(d & CPUID_EXT_NX))
    return false;

u64 efer = rdmsr(MSR_EFER);

if (!(efer & EFER_NXE))
    wrmsr(MSR_EFER, efer | EFER_NXE);

/* Read it back rather than trusting the write. */
return (rdmsr(MSR_EFER) & EFER_NXE) != 0;
```

The read-back is not pedantry: a hypervisor may decline to set a bit it does
not implement, and the consequence of believing it did is a fault on the next
mapping the kernel makes.

`EFER` is per-processor, so every application processor sets `NXE` for itself
in the trampoline — *before* `mov cr3`, because by then the tables it is
about to load already have the bit in them. A core that got this ordering
wrong would triple-fault on its first instruction fetch and look exactly like
a core that never came out of reset, which is the hardest kind of bring-up
bug to find. It is five instructions and a comment explaining why.

And where the feature is absent the kernel says so rather than pretending:

```
vmm: no NX bit on this processor: the kernel's data pages are writable *and*
     executable, and W^X is only a claim
boot: hardening          [ok] W^X (read-only only, no NX), guard pages
```

The `legacy-cpu` CI scenario boots the same image on a Pentium II — PAE, no
NX — and asserts both that the fallback is taken and that the full test suite
still passes on it. Same arrangement as `hardened-cpu` for SMEP/SMAP, for the
same reason: a feature test that silently skips is a feature test that stops
being read.

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

For most of this kernel's life, an address space **copied** the kernel's page
directory entries once, when it was created. A kernel page table created
afterwards therefore existed only in whichever address space happened to be
current.

That was a live bug, found by adding the assertion that catches it. The heap
window spanned directory slots 832 to 847; `heap_init()` mapped 1 MiB, so only
slot 832 existed at boot. Growing the heap past 4 MiB created slot 833 — and
a process forked before that growth would never see it. The symptom would
have been a kernel page fault on a perfectly valid heap pointer, in one
process and not another, long after the allocation that caused it.

The fix at the time was to claim a page table for **every** slot in the
kernel half during `vmm_init()`, while there was exactly one address space to
put them in — 1 MiB of page tables — and to panic if anything ever created a
kernel table later.

PAE removed the problem instead. The kernel's gigabyte is exactly one entry
of the four-entry top-level table, so every address space holds the *same*
kernel page directory rather than a copy of its entries:

```c
/* The kernel's gigabyte, by pointer. Not a copy: a kernel mapping made
 * after this address space exists has to be visible in it, and the only
 * way to guarantee that without tracking every address space is for there
 * to be one kernel page directory. */
top[KERNEL_PDPT_INDEX] = PDPTE(kernel_pd_phys);
```

A table created at runtime is in every address space the instant the entry is
written. The 1 MiB of pre-allocated tables and the panic both went away, and
what is left is a single reservation for the one region that genuinely cannot
be created on demand — the two temporary frame-mapping slots, whose whole
point is to write a page table entry directly without depending on anything
that might itself need a temporary mapping.

The `bootpd` suite asserts the identity directly, which is a one-line test
where it used to be a loop comparing 255 entries:

```c
KT_EQ(r, gigabytes[KERNEL_PDPT_INDEX], vmm_kernel_pd_phys());
```

The `harden` suite still checks that no kernel directory entry has the `USER`
bit. That matters because `ensure_table()` widens a directory entry to `USER`
when any page inside it becomes user-accessible, and a directory entry gates
its whole 2 MiB: one wrong bit would expose 2 MiB of kernel space to ring 3.

It also checks the PDPT entries for reserved bits. Bits 1, 2 and 5-8 of a
PDPT entry are reserved — there is no `WRITE` or `USER` bit at that level —
and a non-zero one faults every walk through it. Writing one out of habit is
the obvious mistake to make when four levels of table look alike, so it is
asserted rather than remembered.

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

- **The user ELF loader allows a writable, executable page** when a program's
  own segments overlap within one page. It is logged by address when it
  happens and it does not happen for anything this kernel builds, because the
  linker page-aligns the segments; refusing would reject a valid ELF. A real
  loader would either refuse or split the page.
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
