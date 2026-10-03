# Fuzzing

A kernel's attack surface is the set of places where it parses something it
did not write: an ELF a user hands to `exec`, a FAT16 boot sector on a disk
anyone can image, an ACPI table from firmware, a system call argument from
ring 3. Every one of those is a stream of offsets and lengths that something
is about to be indexed by, and every one of them is reachable by an
unprivileged program or a removable disk.

The in-kernel suites (`docs/TESTING.md`) test those parsers with inputs
someone thought of. Fuzzing tests them with inputs nobody thought of, which
is a different and complementary claim.

Two harnesses, because the attack surface has two halves that need different
tools:

| | What it tests | Engine | Where it runs |
| --- | --- | --- | --- |
| `tests/fuzz/` | the parsers: ELF, FAT16, heap, ACPI | libFuzzer + ASan + UBSan | the host, 32-bit |
| `user/fuzz.c` | the system call boundary | a deterministic generator | ring 3, under QEMU |

```bash
make fuzz                            # the four host targets, bounded
make fuzz FUZZ_RUNS=5000000          # a real campaign
make fuzz-repro TARGET=elf CASE=crash-abc123   # one input, logging on
python3 tools/run-tests.py --only syscall-fuzz
```

Together they have found **six real bugs**, all of them pre-existing and all
of them listed below with the mechanism and the fix.

---

## Part 1: fuzzing the parsers on the host

### The decision that makes it worth doing

The targets compile **the real kernel sources**. `build/fuzz/fuzz_elf` links
`kernel/core/elf.c` — byte for byte the file that boots — against a shim
(`tests/fuzz/shim.c`) that supplies what a kernel would.

The alternative, extracting the parsing logic into a host-testable copy, is
the more common shape and it is worth being explicit about why it was
rejected: a fuzzer that finds bugs in a rewritten copy of a parser is finding
bugs in the rewrite. The copy drifts, the drift is invisible, and the day it
matters is the day the fuzzer reports clean on code that no longer resembles
what ships.

So nothing is reimplemented. The cost is that the shim has to be convincing,
and that is where most of the design went.

### What the shim supplies

```
kmalloc/kfree   -> the host allocator
```

This is most of the value, and it is not a shortcut. The kernel's heap has
guard magics around each block, which catch an overrun of four bytes or more
on the next `heap_check()`. AddressSanitizer has poisoned redzones, which
catch an overrun of **one** byte, at the instruction that does it, with a
backtrace. One of those two is a debugging aid; the other is a proof.

```
panic()         -> abort()
```

Which reads every `panic()` in the kernel as an assertion the fuzzer is
trying to violate — the correct reading. A kernel that panics on a malformed
input has not handled it.

```
vmm_alloc_at()  -> mmap(MAP_FIXED_NOREPLACE) at the address asked for
```

The one that took thought. `elf_load_user()` does this:

```c
if (!vmm_alloc_at(page, PTE_PRESENT | PTE_WRITE | PTE_USER))
        ...
memcpy((void *)ph->vaddr, (const u8 *)image + ph->offset, ph->filesz);
```

It maps a page at an address an untrusted header chose, and then writes
through that address as a raw pointer. A stub that returned `true` and
discarded the mapping would turn that `memcpy` into a wild write into the
fuzzer's own process — either a spurious crash or, worse, a silent
corruption of libFuzzer's state.

So the shim maps the page the kernel asked for, *at the address the kernel
asked for*, with `MAP_FIXED_NOREPLACE`. `elf.c` is then fuzzed completely
unmodified: its writes land in genuinely mapped memory, and a write one page
past what it mapped hits an unmapped page and takes `SIGSEGV` — which is
exactly what the same bug would do in the kernel.

This is why the targets are built `-m32`. The kernel's `vaddr_t` is 32 bits
and its pointer arithmetic assumes it, and a user ELF asking for `0x00400000`
can only be honoured in a 32-bit address space.

```
cli / sti / hlt / invlpg -> modelled, not ignored
```

`heap.c` takes interrupt-safe locks, which means `cli`, which in a user
process is an immediate `SIGSEGV`. `kernel/include/arch/io.h` stubs the
privileged instructions behind `STRATUM_FUZZING` — a macro the kernel build
never defines — and *models* the interrupt flag in a variable rather than
discarding it, so `irq_save()`/`irq_restore()` still nest correctly and a
lock that forgets to restore is still a bug the fuzzer can find.

### The leak assertion

Each target ends by checking that the shim has nothing outstanding:

```c
/* Accepted or rejected, nothing may be left mapped. */
assert(shim_mapped_pages() == 0);
```

That one line found one of the six bugs, and it is the kind no sanitizer can
find: the pages were legitimately allocated, so there is nothing invalid to
report.

The heap target has the matching assertion in the other direction —
`shim_heap_outstanding()` — for a parser that allocates and forgets. A parser that leaks on its
*rejection* path leaks once per hostile input, which is a denial of service
that repeats — and it is invisible to a test that only checks the return
value.

### The four targets

| Target | Compiles | Input is |
| --- | --- | --- |
| `fuzz_elf` | `kernel/core/elf.c` | a program image handed to `exec` |
| `fuzz_fat` | `kernel/fs/fat16.c` | a disk, from sector 0 |
| `fuzz_heap` | `kernel/mm/heap.c` | a program of alloc/free/realloc opcodes |
| `fuzz_acpi` | `kernel/arch/x86/acpi.c` | physical memory, with an RSDP planted at 0xE0000 |

`fuzz_heap` is the one target that uses the kernel's **own** allocator rather
than the shim's, because the allocator is the thing being fuzzed. It calls
`heap_init()` once, in `LLVMFuzzerInitialize`, and deliberately lets
fragmentation carry across inputs: an allocator bug that needs a particular
heap shape to appear will never be reached by a target that starts clean every
time.

`fuzz_acpi` plants a valid RSDP at physical `0xE0000` with a correct checksum
and lets the fuzzer own everything the RSDP points at. Fuzzing the RSDP search
itself would spend the whole campaign failing a checksum.

### Seeding

```
make fuzz-seed
```

40 inputs, generated by `tools/fuzz-seed.py` from artefacts the build itself
produces — the project's own `init.stripped.elf`, a trimmed copy of its own
`stratum-fs.img` — plus hand-written edge cases: an ELF with two segments
sharing a page, one with a `.bss` tail, a FAT16 image with zero sectors per
cluster, an MADT with a zero-length entry.

A fuzzer seeded with `""` spends its first hour rediscovering what an ELF
header looks like. Coverage after seeding starts where an unseeded run gets to
after a long campaign.

---

## Part 2: fuzzing the system call boundary from ring 3

The host targets cannot test the system call boundary, because that boundary
is a privilege transition and a host process has nowhere to transition from.
`user_range_ok()` reads page table entries; its whole job is to be the thing
between ring 3 and the kernel. Testing it means being in ring 3.

So `user/fuzz.c` is an ordinary ELF, loaded off the FAT16 disk by the ordinary
loader, which issues 40,000 system calls with arguments chosen to be wrong.

### What it is, and what it is not

It is **not** coverage-guided. A ring-3 program cannot see which kernel
branches it reached, there is no instrumentation in the kernel and there is no
corpus. What it has instead is a generator aimed at the boundaries and a large
number of iterations, which finds the shallow bugs — an unvalidated pointer, a
length that is not bounded, a call number that indexes a table.

It is **deterministic**, which matters more than it sounds. The seed is fixed
(`0x5EED5EED`, xorshift32), so iteration 31,416 is the same call with the same
arguments on every boot and on every machine. A fuzzer seeded from the clock
produces crashes nobody can reproduce, and a kernel bug found once and never
again is barely better than not finding it.

### The generator

Pointers are drawn from twelve cases rather than uniformly at random, because
the interesting addresses are a vanishing fraction of 2^32:

| | |
| --- | --- |
| `0x00000000` | the null page, which stays unmapped so a NULL dereference faults |
| `0xC0000000` | exactly the kernel boundary |
| `0xBFFFFFFF` | one byte below it, so a length straddles |
| `0xC0100000` | the kernel's own text |
| `0xFFFFF000` | the recursive page-table window |
| `0xFFFFFFFF` | so that base + length wraps |
| `0xD0000000` | the kernel heap |
| `0x00400000` | our own image — legitimate |
| `0xAFFFF000` | our own stack — legitimate |
| random page | aligned |
| random | unaligned |

Lengths the same way: `0`, `1`, `1024` (exactly `SYS_WRITE_MAX`), `1025` (one
past it), `0xFFFFFFFF`, `0x80000000`, `4096`, random.

Call numbers are mostly inside the table and sometimes far outside it, because
the `default:` case of the dispatcher is as much a target as the calls are.

Three calls are handled specially, and each is a judgement worth stating:

- `exit()` would end the run on the first call.
- `fork()` would multiply the fuzzer rather than the coverage.
- `sleep()` with a random argument would spend the whole run asleep. The
  kernel bounds it to 10 seconds, which is correct, and is not a thing to do
  40,000 times.

### The pass condition

The kernel is still running afterwards. That is the whole assertion, and it is
why the program reports its own completion: a kernel that panicked says
nothing at all, so the absence of a failure message is not evidence.

```
  [fuzz] ring-3 syscall fuzzer: 40000 calls, deterministic seed
  [fuzz] 8192 calls, uptime 5 s
  [fuzz] 16384 calls, uptime 10 s
  [fuzz] 24576 calls, uptime 15 s
  [fuzz] 32768 calls, uptime 20 s
  [fuzz] survived: 18230 accepted, 21770 refused, 0 panics
  [fuzz] the kernel is still running - that is the result
```

The `syscall-fuzz` CI scenario requires both counts to be non-zero. All-refused
would mean the generator never produced a valid call; all-accepted would mean
the kernel never refused one. Either way the run proved nothing. It then runs
`stress 2 20` afterwards, because a kernel that survived the fuzzer and came
out of it with a corrupted heap has not survived the fuzzer.

### One thing the fuzzer had to be taught

`sys_write()` *clamps* an oversized length to `SYS_WRITE_MAX` rather than
refusing it — a short write, which is legitimate. The first version of the
generator assumed a length of `0xFFFFFFFF` with a valid pointer would be
rejected. It is not: it becomes 1024, and the kernel dutifully prints a
kilobyte of the fuzzer's own machine code to the console. 400 such calls
turned a 25-second run into five minutes and buried the transcript CI reads
underneath it.

The fix is three lines in `user/fuzz.c`: writes with a legitimate pointer are
aimed at a one-byte printable buffer instead. The rejection paths — which are
the ones under test — are untouched, and the dots in the transcript are the
accepted writes.

---

## The six bugs

All six were pre-existing. Each is marked with the target that found it.

### 1. `memmove` called `memcpy` with overlapping ranges — `fuzz_heap`

`memmove`'s forward-copy path delegated to `memcpy`. `memcpy`'s arguments are
specified as non-overlapping, so that call is undefined behaviour. It worked
only because this file's `memcpy` happens to be a forward byte loop — and the
compiler is entitled to recognise that loop and replace it with something that
copies in blocks, or in a different order, either of which corrupts the
overlapping tail.

A latent bug of the worst kind: it would have appeared as data corruption after
an unrelated change to an optimisation flag. AddressSanitizer intercepts
`memcpy` and refuses overlapping ranges, which is precisely the contract being
violated, so it was reported on the first input that resized a block downward.

Fixed in `kernel/core/string.c` by spelling the forward copy out.

### 2. ACPI read past the end of the RSDP — `fuzz_acpi`

ACPI 2.0 added a `length` field to the RSDP and a second checksum over
`length` bytes. The parser trusted the field:

```c
if (candidate->revision >= 2 && !checksum_ok(candidate, candidate->length))
```

`length` comes from firmware — or, on a machine where firmware is the thing
you do not trust, from whatever wrote those 36 bytes. A value of `0xFFFFFFFF`
reads four gigabytes from `0xE0000`, which on real hardware walks off into
MMIO.

Fixed by bounding `length` to the structure's actual size before using it, and
by logging the rejection rather than silently ignoring the 64-bit fields.

### 3. The heap stopped coalescing after a shrinking `krealloc` — `fuzz_heap`

`split_block()` carved a remainder off the end of a block and put it on the
free list without checking whether the block *after* it was also free. So a
shrink that happened to be followed by a free left two adjacent free blocks
that would never merge, and the heap fragmented one block at a time under a
realloc-heavy workload until an allocation failed with megabytes free.

Not a memory-safety bug, which is why no sanitizer caught it. It was caught by
the kernel's **own** integrity check — `heap_check()` walks the arena and
reports "adjacent free blocks were not merged" — which `fuzz_heap` calls after
every input. The in-kernel `heap` suite calls the same function; what it did
not have was an input that shrank a block and then freed its neighbour in that
order.

Fixed with a `coalesce_forward(rest)` at the end of `split_block()`.

### 4. Any ring-3 page fault panicked the kernel — `fuzz_elf`

The headline one, and the fuzzer reached it by a route nobody would have
guessed: an ELF whose entry point lay outside every segment it mapped. The
loader accepted it, `usermode_enter()` IRETed to an unmapped address, the
process took a page fault on its first instruction — and the page fault
handler panicked on *any* unresolved fault, with no test for the ring it was
taken in.

So a malformed program file took the machine down. That is not a loader bug,
it is a kernel that let an unprivileged process halt it, and the two bits that
fix it are the privilege level in the saved `CS`:

```c
if ((r->cs & 3) != 0) {
        /* ring 3 cannot have corrupted kernel state - every way it could
         * have was already checked at the syscall boundary. */
        pr_err(... "killing it" ...);
        task_exit(-11);
}
```

Fixed independently of the loader, because a program should not be able to
panic a kernel whatever its entry point says.

### 5. The ELF loader leaked every mapped page on a mid-segment failure — `fuzz_elf`

`elf_load_user()` recorded `image_low`/`image_high` once per segment, *after*
its page loop. The out-of-memory path inside that loop called
`elf_unload_user()`, which unmaps `image_low..image_high` — still at its
initial empty value, so it unmapped nothing and every page mapped so far
leaked.

An image with many segments can provoke that deliberately, and leak on every
attempt. Found by the leak assertion, which asserts nothing is left mapped
whether the load succeeded or failed; the accounting had been one statement too
late since the loader was written.

Fixed by widening the range as each page is mapped. The same target also found
that the loader accepted an entry point outside every mapped segment, which is
now rejected — a loader that accepts an image it knows cannot start is doing
the wrong thing, independently of bug 4.

### 6. A ring-3 process could flood the kernel console — `user/fuzz.c`

Every pointer the fuzzer passed produced a `WARN` line. The console is a
shared, slow, serial device, so a program calling `write()` with a bad pointer
in a loop makes the kernel print on its behalf as fast as the loop goes. The
40,000-call run produced tens of thousands of lines, during which nothing else
got a word in — a denial of service by an unprivileged process against the one
channel an operator uses to see what the machine is doing.

Fixed in the logging layer rather than at each call site, because there are
eight of them and the ninth would forget:

```c
#define pr_warn_ratelimited(...)   /* 5 lines per second, per call site */
```

Each call site gets its own limiter, so a flood of one warning never hides a
different one, and the suppressed count is reported when the window closes —
the information that something is being hit hard is the part worth keeping, not
the thousandth copy of the message:

```
[   24.090] WARN  syscall: (463 more like the next line in the last 1000 ms)
[   24.090] WARN  syscall: unknown syscall 14 from EIP 0x004001fd
```

The limiter stands aside while a test's expected-error window is open, because
a test that counts error lines would otherwise count the wrong number.

---

## What is still not covered

Stated plainly, because a fuzzing document that does not bound its claim is
advertising:

- **No kernel coverage feedback.** The ring-3 fuzzer is blind. Giving it
  feedback means instrumenting the kernel and exporting the counters through a
  syscall — the right design, and not done.
- **The drivers are not fuzzed.** ATA and PS/2 take input from hardware, and
  fuzzing them needs a device model that lies. The block layer *is* reachable
  through `fuzz_fat`, which is where the untrusted bytes actually come from.
- **No concurrency fuzzing.** The scheduler, the locks and the TLB shootdown
  path are tested, not fuzzed. Finding a race this way needs a deterministic
  scheduler the fuzzer can steer, which is a project of its own.
- **`exec()` of a path that does not exist does a disk lookup every time**, so
  a ring-3 loop can make the kernel do unbounded synchronous polled-PIO I/O.
  Found by the syscall fuzzer — it is most of the 600 µs average cost per call
  — and *not* fixed: the fix is a negative-lookup cache, which belongs with the
  rest of the VFS work in `docs/ROADMAP.md`. It is a resource-exhaustion bug,
  not a memory-safety one, and it is listed here rather than quietly left out.

---

## Reproducing a crash

libFuzzer writes the input to `crash-<sha1>`. Then:

```bash
make fuzz-repro TARGET=elf CASE=crash-deadbeef
```

which runs that one input against that one target with the kernel's own log
output turned on (`STRATUM_FUZZ_VERBOSE`), which is most of what makes a
reproducer readable — the kernel usually says exactly what it objected to one
line before it died.
