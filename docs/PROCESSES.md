# Processes: address spaces, fork, copy-on-write, exec, wait

A kernel thread and a process differ in exactly one thing: a process has an
address space of its own. Everything in this document follows from that.

```
stratum> ring3
  [ring3] fork(): duplicating this process
sched: fork: pid 4 -> pid 6, address space 0x0024c000
  [ring3] fork() returned 6 here - one call, two return values
    [child] fork() returned 0 here; my pid is 6, my parent is 4
    [child] I inherited 0x5a5a5a5a and am about to write over it
    [child] my copy now reads 0x1234abcd
    [child] exiting with 7
  [ring3] wait() collected pid 6 with exit code 7
  [ring3] my own copy still reads 0x5a5a5a5a - copy-on-write gave the child a private page
  [ring3] wait() with no children returned -1, as it should
  [ring3] exec(): forking a child to replace its own image
sched: fork: pid 4 -> pid 7, address space 0x00247000
syscall: pid 7 asked to exec "nonexistent", which is not a program this kernel has
    [child] exec("nonexistent") failed cleanly and I am still here
user: pid 7 exec("hello"): replacing 7 user pages
elf: loaded a 2-segment program: entry 0x00400000, 2 pages mapped 0x00400000-0x00402000
user: pid 7 now running "hello" at 0x00400000, 6 user pages
  [exec] hello: a different image, running in the same process
  [exec] getpid() returned 7 - the pid survived exec, the image did not
  [exec] the rebuilt address space still refuses kernel and unmapped pointers
  [exec] calling exit(0)
  [ring3] the exec'd child (pid 7) exited with 0
```

Those sixteen lines contain the whole thing: one call returning twice, a
write that is visible in one address space and not the other, a pid that
outlives the image it was running, and a child whose exit code its parent
collects.

---

## An address space

```c
paddr_t vmm_create_address_space(void);
```

A new page directory, with the **kernel's half copied in** and the user half
empty.

Copying the kernel's half is not a convenience. The kernel's page directory
entries from index 768 (`0xC0000000 >> 22`) upward are identical in every
address space, so a kernel mapping means the same thing everywhere — and that
is the only reason an interrupt can be delivered no matter which process was
running. If each process had a private view of kernel memory, the timer IRQ
that arrived while process B was current would try to execute the handler
through B's tables, and whether that worked would depend on which process
happened to be running.

The same property is what makes `switch_to()` able to change `CR3` *before*
swapping stacks:

```c
/* kernel/core/sched.c */
if (next->page_dir && next->page_dir != prev->page_dir)
    vmm_switch_address_space(next->page_dir);

context_switch(&prev->saved_esp, next->saved_esp);
```

Every task's kernel stack lives in the kernel heap at `0xD0000000`, which is
mapped identically in every address space. So the stack the code is standing
on survives the `CR3` load, and the swap that follows is an ordinary one.

Entry 1023 points the directory at itself — the recursive mapping — so
`0xFFFFF000` is the current directory and `0xFFC00000 + i * 4096` is page
table *i*. That is how the VMM edits the *current* address space's tables
without a permanent mapping for each one.

The `ps` command shows which address space each task is in:

```
stratum> ps
   PID  PPID  NAME           STATE     RING VMSPACE      TICKS  SWITCH
     0     0  idle           ready     ring0 kernel          36       3
     1     0  statusd        sleeping  ring0 kernel           0       6
     2     0  shell          running   ring0 kernel         104       7
     3     2  init           zombie    ring3 -                2       4
```

Three kernel threads sharing the kernel's directory, and `init` — which by
the time the prompt came back had already forked twice, exec'd, and exited.
Its `VMSPACE` reads `-` rather than an address because the reaper has
released it; the slot is still there, still `zombie`, holding an exit code
for `shell` to collect. That is the whole lifecycle in four rows.

While a process is live the column shows its directory's physical address,
which is also what `fork` logs:

```
sched: fork: pid 4 -> pid 6, address space 0x0024c000
```

Two live processes showing the same `VMSPACE` would mean `fork()` handed out
a shared address space, which is the sort of bug that is invisible until it
is catastrophic.

### Editing an address space that is not current

The recursive window only reaches the current directory, and `fork` has to
build the child's tables while the parent's are loaded. So the VMM reserves
two kernel pages as temporary mapping slots:

```c
#define VMM_TEMP_BASE  0xCF000000u
#define VMM_TEMP_SLOTS 2
```

`temp_map(slot, frame)` makes an arbitrary frame writable for a moment.
Two slots, because cloning needs the child's directory and one of its page
tables visible at the same time. The page table backing those slots is
allocated during `vmm_init()`, before anything could need it — allocating it
lazily would mean `temp_map` could fail inside the clone path, which is
precisely where there is nothing useful to do about a failure.

---

## fork

```c
int task_fork(const struct regs *parent_frame);
```

Three things get duplicated and one gets changed.

**A kernel stack.** A fresh 16 KiB allocation, with a hand-built frame on it
(below).

**The address space, copy-on-write.** See the next section.

**The register frame.** The child has to resume where the parent called, with
the same registers — so the parent's trap frame is copied onto the child's
kernel stack. This is why `SYS_FORK` is one of the two syscalls that receive
`struct regs *`: the frame is the thing being duplicated.

**And `EAX`.** The child's copy gets zero. That single assignment is what
makes one call return twice:

```c
*frame = *parent_frame;
frame->eax = 0;
```

### The child's first context switch

A forked child has never run, so there is no saved state to resume — it has to
be manufactured such that the ordinary `context_switch()` epilogue delivers
control somewhere useful. The stack is laid out so that it does:

```
   high
   [struct regs]                     <- the copied frame, eax = 0
   [&frame]                          <- popped by fork_trampoline
   [fork_trampoline]                 <- context_switch's `ret` lands here
   [eflags = 0x202]                  <- popfd
   [ebx] [esi] [edi] [ebp]           <- four pops
                              ^-- child->saved_esp
   low
```

`context_switch()` pops the four registers, `popfd`s, and `ret`s — into
`fork_trampoline`, which is six bytes of assembly:

```asm
fork_trampoline:
        pop     eax                     ; -> struct regs
        mov     esp, eax
        jmp     isr_restore_and_return
```

It moves `ESP` onto the copied frame and jumps into the *same* restore
sequence every interrupt returns through:

```asm
isr_restore_and_return:
        pop     gs
        pop     fs
        pop     es
        pop     ds
        popa
        add     esp, 8                  ; int_no and err_code
        iret
```

Sharing that code rather than writing a second copy is the point. There is
exactly one piece of assembly in the kernel that knows how to get from a
`struct regs` back to the code it describes, so a forked child returns to user
space through the identical instructions as a page fault does. A second
implementation would be a second thing to keep in step with the frame layout.

The `popa` is what delivers the zeroed `EAX`.

---

## Copy-on-write

```c
paddr_t vmm_clone_current(void);
```

For every mapped page in the user half:

1. Clear `PTE_WRITE` and set `PTE_COW` — **in both the parent and the child**.
2. Bump the frame's reference count.
3. `invlpg` the parent's entry, because it just changed.

Step 1 is the one to get right. Marking only the child leaves the parent
writable, and the parent's next write goes straight through into a page the
child is still reading. The two processes would share one page until the
*child* wrote to it, at which point they would stop sharing — a bug whose
symptom is that a parent's data is occasionally, quietly wrong.

`PTE_COW` is bit 10. The x86 architecture leaves bits 9–11 of a page table
entry for software, and this kernel uses two of them:

| Bit | Name | Meaning |
|-----|------|---------|
| 9 | `PTE_OWNED` | this mapping allocated its frame, so unmapping frees it |
| 10 | `PTE_COW` | shared copy-on-write; the next write faults |

`PTE_OWNED` is what stops `vmm_unmap()` from "freeing" the VGA framebuffer.
`PTE_COW` is what tells the fault handler that a write fault on a present,
read-only page is a copy to perform rather than a fault to report.

### Reference counts

Sharing frames means a frame can be freed while somebody is still reading it,
so the PMM keeps one byte per frame:

```c
void pmm_frame_ref(paddr_t frame);
u8   pmm_frame_refs(paddr_t frame);
```

Allocation sets the count to 1, `pmm_frame_ref()` raises it, `pmm_free_frame()`
lowers it, and the frame is reclaimed at zero. One byte per frame is 32 KiB
per GiB of RAM, next to the allocation bitmap.

The count saturates at 255 rather than wrapping. 255 sharers does not happen
here, but if it did, saturating leaks a page and wrapping frees a page that is
still in use — and when a counter has to fail, it should fail in the direction
that loses memory rather than the direction that loses data.

### The fault

```c
/* kernel/mm/vmm.c */
static bool cow_fault(struct regs *r, vaddr_t addr)
{
    if (!(r->err_code & 0x02) || !(r->err_code & 0x01))
        return false;                   /* not a write to a present page */
    if (is_kernel_address(addr))
        return false;

    u32 pte = vmm_pte(addr);
    if (!(pte & PTE_PRESENT) || !(pte & PTE_COW))
        return false;                   /* not ours to handle */

    paddr_t old = pte & PTE_ADDR_MASK;

    /* The last sharer needs no copy - just its write permission back. */
    if (pmm_frame_refs(old) <= 1) {
        set_pte(page, (pte & ~PTE_COW) | PTE_WRITE);
        return true;
    }

    paddr_t fresh = pmm_alloc_frame();
    ...
    temp_map(0, fresh);
    memcpy(dst, (const void *)page, PAGE_SIZE);
    temp_unmap(0);

    set_pte(page, fresh | (pte & 0xFFF & ~PTE_COW) | PTE_WRITE | PTE_OWNED);
    pmm_free_frame(old);
    return true;
}
```

The `refs <= 1` shortcut matters more than it looks. When one of two sharers
exits, the survivor's page is still marked COW with a reference count of 1 —
copying it would allocate a frame, duplicate 4 KiB and immediately drop the
original, which is work to arrive at the page it already had. Handing back the
write bit is the same outcome for a fraction of the cost, and it is why a
process that forks in a loop does not accumulate copies.

`cow_fault()` returns `false` for anything that is not its business, and the
ordinary page-fault reporter takes over. A COW handler that swallowed faults
it did not understand would turn every genuine bug into a silent retry loop.

### Proving it works

From ring 3, in `user/init.c`:

```c
static volatile u32 inherited = 0x5A5A5A5Au;   /* .data: a writable page */

if (sys_fork() == 0) {
    inherited = 0x1234ABCDu;                   /* the COW fault */
    sys_exit(7);
}
sys_wait(&status);
/* and now: inherited must still read 0x5A5A5A5A here */
```

`volatile` is not decoration. Without it the compiler may keep the initial
value in a register across the `fork`, print the right answer, and pass on a
kernel with no copy-on-write at all.

From inside the kernel, the `vmspace` suite checks the mechanism rather than
the outcome — 42 assertions covering that the clone marked the page in both
address spaces, that the reference count rose to 2, that the write allocated
exactly one new frame, that a *second* write does not fault again, that
destroying the child dropped the shared frame to zero references, and that the
whole test returns the frame count to where it started.

That last check is the one that catches leaks. Without it, a COW
implementation that forgets `pmm_free_frame(old)` passes every other
assertion in the suite.

---

## exec

```c
bool usermode_exec(const char *name, struct regs *r);
```

`exec` keeps the process and replaces its image: same pid, same parent, same
kernel stack, a brand-new user half.

The return path is the interesting part. `exec` cannot return a value to its
caller, because the caller no longer exists — the code that executed
`int 0x80` was in the image that has just been unmapped. So instead of
returning, it **rewrites the trap frame the syscall is about to `IRET`
through**:

```c
r->eip = loaded.entry;
r->user_esp = USER_STACK_TOP;
r->eax = 0;
r->ebx = r->ecx = r->edx = 0;
r->esi = r->edi = r->ebp = 0;
```

and lets the ordinary interrupt-return path deliver control into the new
program. The CPU cannot tell the difference between that and a process that
was always running this image. In the dispatcher, success is a `return`
without touching `r->eax`:

```c
case SYS_EXEC:
    ...
    if (!usermode_exec(name, r)) { ret = SYS_ENOENT; break; }
    return;     /* the frame now describes the new image; do not overwrite it */
```

The general registers are cleared rather than inherited. A fresh image has no
business starting with its predecessor's register contents, and leaving them
would make a program's startup state depend on what the program before it
happened to be doing.

### Order of operations

1. **Copy the name out of user memory.** The string lives in the image that
   step 3 unmaps. Reading it afterwards is a dereference of a page that no
   longer exists.
2. **Look the name up and fail if it is unknown** — before anything is torn
   down, so a bogus name leaves the process running its current image. The
   `nonexistent` line in the transcript above is that check.
3. **Clear the user half** (`vmm_clear_user_space()`), load the new image,
   map a fresh stack.

Past step 3 there is no going back: the process has no image. So a failure
there kills the process rather than returning an error code — which is exactly
how a real `exec` behaves, and why the order above is not negotiable.

### Copying a string from user space

```c
static bool user_copy_string(u32 ptr, char *dst, size_t max)
{
    for (size_t i = 0; i < max; i++) {
        if (!user_range_ok(ptr + i, 1))
            return false;
        dst[i] = *(const char *)(ptr + i);
        if (dst[i] == '\0')
            return true;
    }
    return false;       /* unterminated: refuse rather than truncate */
}
```

Validated one byte at a time rather than `max` bytes up front, because a
string that ends two bytes into the last mapped page is perfectly legal and
validating the whole buffer would reject it. Unterminated input fails rather
than being silently truncated — a truncated program name is a different
program name.

### exec's namespace

There is no filesystem yet, so the programs a process can `exec` into are the
ones embedded in the kernel image:

```c
static const struct {
    const char *name;
    const u8 *start;
    const u8 *end;
} programs[] = {
    { "init",  _binary_init_elf_start,  _binary_init_elf_end  },
    { "hello", _binary_hello_elf_start, _binary_hello_elf_end },
};
```

```
stratum> programs
Programs embedded in the kernel image (exec's namespace):
  init   (started at boot)
  hello
  1 exec() calls so far this boot
```

This table is the one piece `exec` genuinely needs and this kernel does not
have. Everything else about the call is real — the old address space is torn
down, a new image is loaded and validated by the same loader that rejects
hostile ELFs, a fresh stack is mapped, and the process keeps its identity.
Replacing the table with a path lookup is the only change a filesystem would
require, which is why it is named here rather than hard-wired into the loader.

`hello` exists for one reason: `exec` has to be shown replacing an image, and
a program re-executing *itself* would produce identical output whether exec
worked or silently did nothing. `hello` prints its own pid, and that pid
matching the caller's from before the `exec` is the proof.

---

## wait, and what a zombie is

```c
int task_wait(int *status_out);
```

A task's resources and its *exit status* have different lifetimes, and that
difference is what a zombie process is.

When a task exits, it cannot free its own stack — it is standing on it — nor
its own address space, since it is running in it. So `task_exit()` marks it
`TASK_ZOMBIE`, picks another task, and switches away. Two contexts are then
entitled to clean up after it:

- the **idle task's reaper**, so a process whose parent never calls `wait()`
  still has its memory returned;
- **`wait()` itself**, so that collecting a child is the point at which its
  memory is definitely gone.

Both go through one function:

```c
static void release_task_resources(struct task *t)
{
    if (t->resources_freed || t == current) return;   /* idempotent */
    /* unlink from the run queue, then: */
    kfree(stack);
    if (pd && pd != vmm_kernel_pd_phys())
        vmm_destroy_address_space(pd);
}
```

Idempotent, because the two callers race for it, and `resources_freed` is set
inside the same interrupts-off window that unlinks the task from the run
queue. The **slot** survives all of this, still `TASK_ZOMBIE`, holding the
exit code until a parent collects it. That is the zombie: resources gone,
exit status still owed to somebody.

Having both paths matters in a way that is easy to get wrong. An earlier
version of this released resources only in the reaper, and `wait()` simply
marked the slot `TASK_UNUSED`. If `wait()` won the race — which it usually
does, since the parent is runnable the moment its child exits — the slot went
back into circulation while the task was **still linked into the run queue**,
holding a stack and a page directory. The next `task_create()` would hand out
that slot and splice it into the list a second time. The in-kernel `proc`
suite now drains children and checks the accounting on the way through.

`wait()` blocks rather than spinning: it sets itself `TASK_BLOCKED` and
yields, and `task_exit()` moves a blocked parent back to `TASK_READY`. With no
children at all it returns -1 immediately, which the ring-3 program checks —
a `wait()` that blocked on a child that does not exist is a hang, and hangs
are harder to diagnose than error codes.

### Kernel threads are children too

`task_create()` records `parent_pid = current->pid`, so a kernel thread is a
child of whoever created it and `wait()` will collect it. This surfaced as a
test failure: the `proc` suite asserted `task_wait() == -1` and got a pid,
because the `sched` suite had created worker threads earlier in the same run
and they were this task's children. The test was wrong, not the kernel — but
it is the kind of thing that is only ever discovered by writing the assertion.

---

## The system call surface

Eleven calls. `fork`, `wait` and `exec` are the three that touch `struct regs`.

| # | Call | Notes |
|---|------|-------|
| 0 | `exit(code)` | does not return; the frame is never restored |
| 1 | `write(buf, len)` | pointer validated, bounded to 1 KiB |
| 2 | `getpid()` | |
| 3 | `yield()` | |
| 4 | `sleep(ms)` | bounded to 10 s so ring 3 cannot fake a hang |
| 5 | `uptime()` | |
| 6 | `getkey()` | |
| 7 | `fork()` | returns twice: child pid in the parent, 0 in the child |
| 8 | `wait(&status)` | blocks; -1 with no children; status pointer validated |
| 9 | `getppid()` | |
| 10 | `exec(name)` | does not return on success |

Errors are negative: `SYS_EBADCALL` (-1), `SYS_EFAULT` (-2), `SYS_EINVAL`
(-3), `SYS_ENOENT` (-4).

Every pointer in that table is validated by `user_range_ok()` before the
kernel dereferences it — including `wait`'s optional status pointer, which is
checked before being written through. See
[USERSPACE.md](USERSPACE.md#the-boundary-and-testing-it-from-the-wrong-side).

---

## What is still missing

- **No `execve` arguments or environment.** `exec` takes a name and nothing
  else, so there is no `argv`.
- **No file descriptors.** `write` goes to the console unconditionally, and
  `fork` therefore has no descriptor table to duplicate.
- **No `fork` from a kernel thread.** `fork` needs a user trap frame to copy;
  a kernel thread has none, and `task_fork(NULL)` is refused rather than
  guessed at.
- **No demand paging.** A program's image is mapped eagerly. Copy-on-write is
  the only laziness in the memory manager.
- **No process groups, signals or `kill`.** A process leaves through `exit`
  or a fault.
- **32 task slots, statically allocated.** `fork` fails with -1 when they run
  out, which the caller is expected to handle.
- **No NX.** Without PAE, x86 cannot mark a page non-executable, so
  "read-only" is enforced and "non-executable" is not. That is the next item
  in [ROADMAP.md](ROADMAP.md).
