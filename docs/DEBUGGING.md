# Debugging

## GDB against QEMU

```bash
make debug      # terminal 1: QEMU starts stopped, listening on :1234
make gdb        # terminal 2: attaches with symbols, breaks at kmain
```

`make gdb` loads `build/x86/stratum.debug.elf`, which keeps full DWARF. The
bootable `build/x86/stratum.elf` is stripped so stage 2 has room to stage it below
the EBDA — same link, two outputs.

Useful once attached:

```gdb
break vmm_init
break page_fault_handler
break *0x7c00                 # stage 1's first instruction
info registers
p/x read_cr3()
x/16xw 0x100000               # the kernel's first 16 words
x/8i $eip                     # disassemble around the current instruction
p *(struct task *)task_current()
p/x *(u32 *)0xFFFFF000        # the page directory, via the recursive window
```

### Debugging the bootloader

Real mode needs telling, because GDB assumes the architecture the ELF
declares:

```gdb
set architecture i8086
break *0x7c00         # stage 1
break *0x7e70         # stage 2's entry (header is 112 bytes)
```

Watching the protected-mode switch:

```gdb
break *0x7e70
continue
# step until `mov cr0, eax`, then:
p/x $cr0              # bit 0 should go from 0 to 1
set architecture i386 # the far jump has reloaded CS
```

---

## Reading a panic

```
================================================================
 KERNEL PANIC - StratumOS 0.3.0
================================================================
 page fault at 0x00000000 in the null page - almost certainly a NULL dereference

  EAX 00000000  EBX 00120466  ECX 0000000a  EDX 0000002d
  ESI 00000110  EDI 00000000  EBP d0007fb8
  EIP 0010c760  CS  0008      EFLAGS 00000286
  DS  0010      ES  0010      FS 0010      GS 0010
  vector 14 (Page Fault)  error 00000002
  (trap from ring 0; SS/ESP unchanged)
  CR0 80010011  CR2 00000000  CR3 00123000  CR4 00000000

Call trace (return addresses; the faulting frame is EIP above):
  [0] 0x0010dbb6
  [1] 0x0010e157
  [2] 0x00101b02
================================================================
```

Read it in this order:

1. **`EIP`** is the faulting instruction. Resolve it first.
2. **`vector`** says what kind of fault. For a page fault, `CR2` is the address
   and the decoded `reason:` line above the banner says why.
3. **The call trace** is *return* addresses, so it names the callers of the
   faulting function, not the function itself — that is `EIP`. This confuses
   everyone once.
4. **`CS & 3`** tells you the privilege level. `0x0008` is ring 0; `0x001B`
   would be ring 3, and then `SS`/`ESP` are also meaningful.
5. **`EBP`** tells you which stack you were on. `0xd0xxxxxx` is a task stack
   (they come from the heap); `0x0011xxxx` is the boot/idle stack in `.bss`.

### Resolving addresses

```bash
addr2line -f -e build/x86/stratum.debug.elf 0x0010c760 0x0010dbb6 0x0010e157
```

```
cmd_fault        kernel/shell/shell.c:641
shell_run_line   kernel/shell/shell.c:712
shell_task       kernel/shell/shell.c:752
```

All at once:

```bash
grep -oE '0x[0-9a-f]{8}' panic.txt | xargs addr2line -f -e build/x86/stratum.debug.elf | paste - -
```

### Decoding a page-fault error code

The kernel prints this in words, but to read it raw — bits of
`err_code`:

| Bit | Set means |
| --- | --- |
| 0 | the page was present, so a protection check failed (clear = nothing mapped) |
| 1 | the access was a write |
| 2 | it came from ring 3 |
| 3 | a reserved bit was set in a paging-structure entry |
| 4 | it happened on an instruction fetch |

`error 00000002` is therefore a ring-0 write to an unmapped page.

---

## Tools inside the kernel

```
stratum> pagemap 0xd0000000    resolve a virtual address and show its PTE flags
stratum> hexdump 0x100000 64   dump memory, refusing unmapped pages
stratum> meminfo               all three allocators, plus a heap integrity check
stratum> ps                    tasks, states, tick counts, stack addresses
stratum> irq                   interrupt counters and the spurious count
stratum> log debug             raise the log level at runtime
stratum> selftest vmm          run one suite
stratum> stress 4 500          hammer the heap from four tasks, then check it
```

`pagemap` is usually the fastest way to answer "why did that address fault":

```
stratum> pagemap 0x0
virtual   : 0x00000000
dir index : 0    table index: 0
mapping   : not present

stratum> pagemap 0x1000
physical  : 0x00001000
pte       : 00001003
flags     : present write supervisor
```

`hexdump` checks the page tables before dereferencing, so inspecting a bad
pointer does not itself crash the kernel.

---

## Headless, and how to script it

Everything the kernel prints goes to COM1, and so does everything both
bootloader stages print. The shell reads from it too.

```bash
make run-serial                                    # interactive, no window
qemu-system-i386 -display none -serial file:/tmp/boot.log \
    -drive format=raw,file=build/x86/stratum.img,index=0,media=disk
```

To script input, wait for the prompt rather than piping — the UART's 16-byte
FIFO overruns before the kernel starts reading. `SerialSession` in
`tools/run-tests.py` is a working 50-line example.

---

## Symptoms and their usual causes

| Symptom | Where to look |
| --- | --- |
| Nothing at all on serial | stage 1 did not run. Check the 0x55AA signature and that QEMU is given the image as a disk, not a CD. |
| `E: stage2 read failed` | the image is truncated, or the BIOS rejected both LBA and CHS. |
| `E: stage2 corrupt` | the magic at LBA 1 is wrong — usually stage 2 was rebuilt but the image was not. |
| `FATAL: kernel is not a valid 32-bit x86 ELF` (white on red) | stage 2 reached protected mode but the staged file is not `ET_EXEC`/`EM_386`. `make sections` would have caught it. |
| GRUB says "no multiboot header found" | the header moved past 32 KiB or its checksum is wrong. `check-kernel.py` verifies both. |
| Triple fault immediately after `sti` | an interrupt arrived before the PIC was remapped. |
| Triple fault right after enabling paging | the identity map does not cover the kernel, the stack, or the IDT. |
| `unhandled CPU exception 6: Invalid Opcode` | either an SSE instruction (`make sections` scans for these) or GCC turning undefined behaviour into `ud2`. |
| A handler reading nonsense from its `struct regs` | the frame layout in `isr.asm` and `struct regs` disagree. The `irq` test suite exercises this path on purpose. |
| Timer stops after the first context switch | preemption happened before the PIC's EOI. See the comment at the top of `core/sched.c`. |
| Heap panic naming a pointer you freed long ago | an overrun corrupted a neighbouring block's footer. The panic names the size, which usually identifies the allocation. |
| Keys arrive as digits instead of arrows | the `0xE0` scancode prefix is not being tracked. |

---

## A worked example

The panic above came from `fault null`, but at first it was not a page fault
at all — it was an invalid opcode with `CR2 = 0`:

```
 unhandled CPU exception 6: Invalid Opcode
  EIP 0010c5f1
```

`EIP` resolved into `cmd_fault`, where the source said:

```c
volatile u32 *p = (volatile u32 *)0;
*p = 1;
```

Disassembling settled it:

```bash
objdump -d --no-show-raw-insn build/x86/stratum.debug.elf | awk '/<cmd_fault>:/{f=1} f&&/ud2/{print}'
  10c7ec:	ud2
```

GCC had proved the store was undefined behaviour and compiled it to `ud2`. The
kernel caught and reported that perfectly; the test was asking for the wrong
thing. Routing the address through a file-scope `volatile` made it opaque, and
the real page fault appeared.

The lesson generalises: when a fault does not match the source, check what was
actually emitted before suspecting the hardware.
