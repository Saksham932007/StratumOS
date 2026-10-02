# Ring 3

The kernel runs a real user program: a separate ELF, built from `user/`,
linked for user space, and loaded into user-accessible pages by the kernel's
own ELF loader.

```
stratum> ring3
  [ring3] hello from user mode - privilege level 3
  [ring3] I am a separate ELF, loaded into my own pages
  [ring3] getpid() returned 4
  [ring3] uptime() returned 0 s
  [ring3] yielded and was rescheduled
  [ring3] slept 50 ms via syscall
  [ring3] my own stack is writable, as it should be
  [ring3] asking the kernel to read a kernel address on my behalf
  [ring3] kernel refused it (EFAULT) - the pointer check works
  [ring3] unmapped user pointer also refused
  [ring3] unknown syscall correctly rejected
  [ring3] calling exit(0)
```

---

## Why it is a separate program

The first version of this was a `.user` section inside the kernel image, with
the `USER` bit flipped on its pages. That works, and it is what most hobby
kernels do, but it is not really a test of anything: the code was linked at a
kernel address and ran at a kernel address.

Once the kernel moved to the higher half, that stopped working at all — and
the reason is instructive. A program linked at `0xC01xxxxx` and then mapped at
`0x00400000` has every absolute reference still pointing at `0xC01xxxxx`.
Every string literal. The first `u_puts()` would hand the kernel a pointer
into kernel space, and a correct kernel would refuse it.

So the program is built separately, with its own link script:

```ld
/* user/user.ld */
ENTRY(_start)
USER_IMAGE_BASE = 0x00400000;

SECTIONS
{
    . = USER_IMAGE_BASE;
    .text   ALIGN(4K) : { *(.text .text.*) }
    .rodata ALIGN(4K) : { *(.rodata .rodata.*) }
    .data   ALIGN(4K) : { *(.data .data.*) }
    .bss    ALIGN(4K) : { *(COMMON) *(.bss .bss.*) }
}
```

Linking it for the address it will run at is the only way its own addresses
are addresses it can use. Sections are page-aligned so that text and data do
not share a page and can carry different permissions.

The compiled ELF is stripped and embedded in the kernel image as a blob:

```make
$(USER_BLOB): $(BUILD)/user/init.stripped.elf
	cd $(dir $<) && $(OBJCOPY) -I binary -O elf32-i386 -B i386 \
		--rename-section .data=.rodata.userblob,... \
		init.stripped.elf $(notdir $@)
```

which gives the kernel `_binary_init_elf_start` and `_binary_init_elf_end`.
Embedding rather than reading from disk is a scoping decision — there is no
filesystem yet — and the loader is written so that swapping the blob for a
file read is the only change needed.

9 KiB of the kernel image, most of it page-alignment padding.

---

## The ABI is one header

`kernel/include/kernel/syscall_abi.h` is included verbatim by both sides. The
call numbers and error values are a contract, and a contract with two copies
is a contract with two versions.

```c
enum {
    SYS_EXIT = 0, SYS_WRITE = 1, SYS_GETPID = 2, SYS_YIELD = 3,
    SYS_SLEEP = 4, SYS_UPTIME = 5, SYS_GETKEY = 6, SYS_MAX
};
#define SYS_EBADCALL (-1)
#define SYS_EFAULT   (-2)
#define SYS_EINVAL   (-3)
```

Nothing in it depends on kernel-internal headers, which is what lets the user
program compile against it without being able to see anything else.

`user/syscall.h` wraps those numbers in inline stubs — `int 0x80` with the
call number in `EAX` and arguments in `EBX`/`ECX`/`EDX` — plus the few lines
of runtime a program with no libc needs (`u_strlen`, `u_puts`, `u_putu`).

---

## The ELF loader

`kernel/core/elf.c` maps a program's `PT_LOAD` segments into the current
address space. The bootloader already does this for the kernel, so the
mechanism is familiar; the difference that matters is **trust**.

Stage 2 loads a kernel the build produced. This loads a program that, in
general, the kernel did not write — and every field in an ELF header is an
offset or a length that something will be indexed by. So:

**Every segment is validated before any is mapped.** A half-loaded program is
harder to clean up than a rejected one, and leaves the address space in a
state the caller cannot describe.

**The program header table is bounds-checked with division, not
multiplication**, so the check itself cannot overflow:

```c
if ((size - eh->phoff) / eh->phentsize < eh->phnum)
    REJECT("program header table runs past the end of the image");
```

**No segment may touch kernel space.** This is the decisive check. An image
claiming `p_vaddr = 0xC0100000` is asking the kernel to overwrite itself on
the program's behalf:

```c
if (is_kernel_address(start) || is_kernel_address(end - 1))
    goto kernel_space;
```

**Nor the null page**, so a NULL dereference keeps faulting.

**Permissions are applied in a second pass.** Segments are mapped writable so
their contents can be copied in, then tightened — read-only for anything
without `PF_W`. Doing it in a second pass means a page shared between a
read-only and a writable segment ends up writable, rather than depending on
which segment happened to be processed last.

`check-kernel.py` runs the same structural checks on the embedded blob at
build time, so a user image that could reach into kernel space fails the build
rather than the boot:

```
embedded user program: 9120 bytes, entry 0x400080, 2 segment(s),
                       all below 0xc0000000
```

The `elf` in-kernel test suite feeds the validator a deliberately corrupted
copy — bad magic, ELFCLASS64, `EM_X86_64`, `ET_DYN`, 60000 program headers, a
`phoff` of `0xFFFF0000`, a `phentsize` smaller than a program header — and
requires each to be refused, *with a reason*. A loader that rejects an image
without saying why is a loader nobody can debug.

---

## Getting to ring 3

There is no instruction for "lower my privilege level". The only way down is
to return to somewhere that was never privileged, so the kernel forges the
stack frame an inter-privilege `IRET` expects:

```asm
; arch/x86/usermode.asm
mov     cx, SEL_USER_DATA
mov     ds, cx                  ; data segments can be demoted directly
...
push    dword SEL_USER_DATA     ; SS
push    edx                     ; ESP
pushfd
pop     ecx
or      ecx, 0x200              ; IF: stay preemptible
and     ecx, ~0x100             ; TF: no single-stepping
push    ecx                     ; EFLAGS
push    dword SEL_USER_CODE     ; CS  (RPL 3 is what triggers
push    eax                     ; EIP  the stack switch)
iret
```

When `IRET` sees a target `CS` whose RPL is numerically greater than the
current CPL, it additionally pops `SS` and `ESP` — which is why a ring-3 frame
is five dwords and a ring-0 one is three.

Getting back is the CPU's problem. A trap, an IRQ or `int 0x80` switches to
the ring-0 stack recorded in the TSS's `esp0`, which `switch_to()` updates on
every context switch. Getting that wrong means a ring-3 interrupt corrupts
some other task's stack.

---

## The boundary, and testing it from the wrong side

`user_range_ok()` is the only thing between a hostile ring-3 pointer and
kernel memory. With the kernel in the higher half it is almost trivial:

```c
if (base + len < base)                 /* wraps the address space */
    return false;
if (is_kernel_address(base) || is_kernel_address(base + len - 1))
    return false;

for (each page in the range)
    if (!(pte & PTE_PRESENT) || !(pte & PTE_USER))
        return false;
```

Three properties, each of which has a test:

- **Wrapping is refused.** `base + len` overflowing is the classic way to make
  a bounds check pass for a range that does not exist.
- **Both ends are checked.** A range *straddling* the boundary must be refused
  on the strength of its end, not just its start — otherwise a user pointer a
  few bytes below `0xC0000000` becomes a kernel write of arbitrary length.
- **Every page must be present *and* user-accessible.** Being below the kernel
  boundary is not enough; an unmapped user address is still not readable.

The interesting part is that the program itself checks these, from the
untrusted side:

```c
/* user/init.c */
if (sys_write((const char *)0xC0100000u, 16) == SYS_EFAULT)
    u_puts("  [ring3] kernel refused it (EFAULT) - the pointer check works\n");
else
    u_puts("  [ring3] WARNING: kernel accepted a kernel pointer from ring 3!\n");
```

And CI asserts on both: that the refusal line appears, and that the warning
line never does.

---

## Limits

- **One address space.** There is a single page directory;
  `context_switch()` does not touch `CR3`. Two user programs would share a
  view of memory, and only one can be loaded at a time.
- **No `fork`, `exec` or `wait`.** A task enters ring 3 once and leaves
  through `exit`.
- **No demand paging.** A program's whole image is mapped eagerly.
- **No NX.** Without PAE, x86 cannot mark a page non-executable, so
  "read-only" is enforced but "non-executable" is not.
- **Seven system calls**, and no file descriptors — `write` goes to the
  console unconditionally.

The first two are what per-process address spaces fix, and they are the next
item in [ROADMAP.md](ROADMAP.md).
