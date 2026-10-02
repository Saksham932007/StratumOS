# Booting StratumOS

Two independent paths load the same `stratum.elf`. This document walks both.

```
┌─ Path A ─ custom bootloader ──────────────────────────────────────────────┐
│  BIOS → stage1 (LBA 0) → stage2 (LBA 1-24) → protected mode → ELF loader │
└───────────────────────────────────────────────────────────────────────────┘
┌─ Path B ─ Multiboot2 ─────────────────────────────────────────────────────┐
│  BIOS → GRUB → multiboot2 command → kernel loaded from the ISO           │
└───────────────────────────────────────────────────────────────────────────┘
                              both arrive at
                 _start, with EAX = magic and EBX = info pointer
```

---

## Path A: the custom bootloader

### Disk layout

`tools/mkimage.py` produces this, and asserts every constraint it depends on:

```
LBA 0        512 bytes     stage 1 — the MBR, ending in 0x55AA
LBA 1-24     12,288 bytes  stage 2 — padded to exactly 24 sectors
LBA 25+      ~95 KiB       the kernel ELF, verbatim
             padding       to a 2 MiB boundary
```

Stage 2's first 16 bytes are a header the assembler cannot fill in, because
the kernel's size is not known until link time:

```
+0    'S2OS'                    magic, checked by stage 1
+4    kernel LBA                patched by mkimage
+8    kernel length in sectors  patched by mkimage
+12   reserved
+16   command line (96 bytes)   patched by mkimage; this is how `autotest`
                                reaches the kernel on the disk image path
+112  entry point
```

### Stage 1 — 315 of 510 usable bytes

The BIOS loads one sector to `0x7C00` and jumps to it in 16-bit real mode.
Stage 1 has room for exactly one job: load stage 2.

```asm
cli
xor ax, ax
mov ds, ax          ; ORG is 0x7C00, so a label's value IS its linear
mov es, ax          ; address. Setting ds = 0x07C0 here as well would
mov ss, ax          ; double-count the base and send every reference
mov sp, 0x7C00      ; 0x7C00 bytes too high.
sti
mov [boot_drive], dl
```

That comment describes a real bug in the project this one is derived from. The
symptom was that the loading message printed garbage and `lgdt` loaded a
descriptor table from `0xF800`.

Then, in order:

1. **Probe for INT 13h extensions.** `AH=41h` with `BX=0x55AA`; success means
   `BX` comes back `0xAA55` and bit 0 of `CX` is set. Checking the bit matters:
   a BIOS can report the interface and not support packet access.
2. **Extended read.** `AH=42h` with a Disk Address Packet — 24 sectors from LBA
   1 to `0000:7E00`. LBA addressing needs no geometry guessing and works on
   hard disks, USB sticks and El Torito CD emulation alike.
3. **CHS fallback.** `AH=02h`, three attempts, with `AH=00h` (reset controller)
   between them. LBA 1 is cylinder 0, head 0, **sector 2** — CHS sectors are
   1-based while LBA is 0-based, an off-by-one that produces a working-looking
   loader that reads the wrong sector.
4. **Verify and jump.** Check stage 2's magic, restore `DL`, far jump to
   `0000:7E70`.

A `%if ($ - $$) > 510` guard makes outgrowing the sector a build error rather
than a silent truncation that triple-faults.

### Stage 2

#### 1. The A20 gate

On the original IBM PC, address line 20 was forced low so that addresses above
1 MiB wrapped to 0 — software depended on that wrap. Every PC since has
inherited the gate, and it is still closed at boot. With A20 closed,
`0x100000` reads back as `0x000000`, so a kernel loaded at 1 MiB does not
exist.

Three methods are tried in escalating order, and each is **verified** rather
than assumed:

| Method | Mechanism |
| --- | --- |
| BIOS | `INT 15h AX=2401h` |
| 8042 | read the keyboard controller's output port, set bit 1, write it back |
| Fast A20 | set bit 1 of System Control Port A (`0x92`) |

The verification compares `0000:0500` with `FFFF:0510`. Those two
`segment:offset` pairs resolve to the same linear byte when A20 is forced low,
so writing `0xFF` to one and reading it back from the other detects the wrap:

```asm
mov     byte [es:di], 0x00      ; es:di = 0000:0500
mov     byte [ds:si], 0xFF      ; ds:si = FFFF:0510
cmp     byte [es:di], 0xFF      ; did the far write alias?
```

Both original byte values are saved and restored, because `0x500` is in use.

One detail worth noting in the fast-A20 path: bit 0 of port `0x92` triggers a
CPU reset. The code masks it off explicitly.

#### 2. The memory map

`INT 15h AX=E820h` walks the firmware's map, 24 bytes per entry — a 64-bit
base, a 64-bit length, a type, and ACPI 3.0 extended attributes. Three things
that are easy to skip and are handled:

- `EBX` is both the continuation cookie and the loop terminator; zero on return
  means that was the last entry.
- Some BIOSes emit zero-length entries, which are discarded.
- If the BIOS filled in 24 bytes, bit 0 of the extended attributes means "this
  entry is valid". If it filled in 20, that field is ours and is pre-set to 1.

`AX=E801h` and `AH=88h` are also queried as fallbacks for firmware too old to
have E820, and recorded so the kernel can report which source it used.

#### 3. Loading the kernel

The ELF is read into a staging buffer at `0x20000` in 32 KiB chunks, the
destination segment advancing by `0x800` paragraphs each time so a transfer
never crosses a real-mode segment wrap. `mkimage.py` checks at build time that
the staged image fits below the EBDA at `0x9FC00`, and reports the headroom:

```
staged at 0x20000-0x36f80, 420 KiB of headroom
```

#### 4. Protected mode

```asm
cli
lgdt    [gdt_descriptor]
mov     eax, cr0
or      eax, 1                  ; CR0.PE
mov     cr0, eax
jmp     CODE_SEL:pm_entry       ; the far jump is what reloads CS
```

Setting `CR0.PE` does not change the hidden base and limit the CPU cached for
`CS`. Only a far transfer reloads it, which is why the jump is mandatory and
not merely conventional. The GDT is flat: two descriptors spanning the whole
4 GiB, so a linear address equals a logical offset and segmentation gets out
of the way.

#### 5. The ELF loader

This is the part that makes one kernel binary work under both loaders. In
32-bit code, stage 2 validates the header and copies each `PT_LOAD` segment:

```asm
cmp     dword [esi], 0x464C457F ; \x7F E L F
cmp     byte  [esi + 4], 1      ; ELFCLASS32
cmp     byte  [esi + 5], 1      ; ELFDATA2LSB
cmp     word  [esi + 16], 2     ; ET_EXEC
cmp     word  [esi + 18], 3     ; EM_386
...
mov     edi, [ebp + 12]         ; p_paddr      → destination
add     esi, [ebp + 4]          ; + p_offset   → source
mov     ecx, [ebp + 16]         ; p_filesz
rep     movsb
mov     ecx, [ebp + 20]         ; p_memsz
sub     ecx, [ebp + 16]         ; − p_filesz = the .bss tail
rep     stosb                   ; zero it; EDI is already in place
```

Zeroing `p_memsz − p_filesz` is what a Multiboot loader does for `.bss`, and
is why the kernel's BSS is usable before it has zeroed it itself.

Doing the parse in protected mode rather than real mode avoids needing unreal
mode to reach the destination at 1 MiB.

#### 6. Handover

```asm
mov     edx, [KERNEL_STAGE + 24]  ; e_entry
mov     eax, STRATUM_MAGIC        ; 0x53545241, 'STRA'
mov     ebx, BOOTINFO_ADDR        ; 0x4000
jmp     edx
```

The boot info block at `0x4000` carries the E820 array pointer and count, the
boot drive, the command line, and flags saying which memory-detection methods
worked. `struct stratum_boot_info` in `kernel/include/boot/bootinfo.h` mirrors
those offsets.

### Serial output from the bootloader

Both stages talk to the 16550 directly, in addition to the BIOS teletype call:

```asm
ser_init:                       ; 115200 8N1 — the same settings the kernel
    mov     dx, COM1_IER        ; expects, so output continues seamlessly
    ...                         ; across the handover into protected mode
```

It costs about 40 bytes and makes the entire boot observable with no display
attached. That is what lets CI assert on `[ok] A20 gate` — and it is the
reason an early failure in this project was diagnosable at all, since the BIOS
teletype writes to a screen nobody is looking at.

---

## Path B: Multiboot2 via GRUB

`kernel/arch/x86/boot.asm` opens with a header GRUB searches for in the first
32 KiB of the file:

```asm
mb2_start:
    dd  0xE85250D6              ; magic
    dd  0                       ; architecture: i386 protected mode
    dd  mb2_end - mb2_start     ; header length
    dd  -(0xE85250D6 + 0 + (mb2_end - mb2_start))   ; checksum: all four
                                                    ; words must sum to 0
    align 8
    dw  1                       ; information request tag
    dw  0                       ;   flags: 0 = required
    dd  8 + 4*4
    dd  1, 2, 4, 6              ;   cmdline, loader name, meminfo, mmap
    align 8
    dw  0, 0                    ; end tag
    dd  8
mb2_end:
```

Requesting the tags explicitly means a loader that cannot supply one fails at
boot rather than leaving the kernel to discover a missing memory map later.

`tools/check-kernel.py` verifies at build time that the header exists, is
8-byte aligned, sits inside the 32 KiB window, and that its checksum really
sums to zero. A wrong checksum produces "no multiboot header found" from GRUB
and nothing else to go on.

GRUB loads the ELF, places an information block of tags somewhere in low
memory, and jumps to `e_entry` with `EAX = 0x36D76289` and `EBX` pointing at
that block. `parse_multiboot2()` walks the tags — each 8-byte aligned, the
sequence ending at a type-0 tag — and extracts the command line, loader name,
basic meminfo and memory map, validating sizes as it goes.

> **`qemu -kernel` does not work here.** QEMU's built-in loader implements
> Multiboot *1*, not 2, and refuses a kernel that only declares a v2 header
> ("Error loading uncompressed kernel without PVH ELF Note"). Use
> `make run-iso`, or `make run` for the custom bootloader.

---

## The unified entry point

```asm
_start:
    cli
    cld
    mov     esi, eax            ; stash the magic; EBX already holds the
                                ; info pointer and survives the loop below

    mov     edi, __bss_start    ; zero .bss ourselves. Both loaders already
    mov     ecx, __bss_end      ; do this, but doing it here means the kernel
    sub     ecx, edi            ; does not *depend* on either having been
    xor     eax, eax            ; careful. No stack is needed, which is why
    rep     stosb               ; it runs before ESP is valid.

    mov     esp, stack_top
    push    0
    popfd                       ; clear EFLAGS: IF, DF, NT, everything
    xor     ebp, ebp            ; terminates the backtrace walk

    push    ebx                 ; kmain(magic, info_addr)
    push    esi
    call    kmain
```

The BSS is cleared before `ESP` is set because the kernel stack lives in the
BSS. `EBP` is zeroed so that a panic during early boot produces a backtrace
that terminates rather than one that walks off into whatever the loader left
behind.

From there, `boot_parse()` is the only code in the kernel that knows which
loader ran. Everything else reads one `struct boot_params`.

---

## Verifying both paths

```bash
make test-boot
```

boots the custom bootloader from a raw disk image and GRUB from an ISO,
asserting on the protocol each reports:

```
("native protocol detected",  r"boot protocol\s+\[ok\] StratumOS native"),
("multiboot2 protocol detected", r"boot protocol\s+\[ok\] Multiboot2"),
```

A regression that made one path silently fall back to the other, or that broke
one while the other was being worked on, fails CI.
