; =============================================================================
; StratumOS - 32-bit protected mode to 64-bit long mode, and back
; =============================================================================
;
; The original repository this project merges was called
; "Advanced-Bootloader-16-bit-to-32-bit-C-Kernel". This file is the third
; step of that arc: 16-bit real mode -> 32-bit protected mode -> 64-bit long
; mode, and then all the way back, with the 32-bit kernel still running
; afterwards.
;
; WHAT THIS IS, AND WHAT IT IS NOT
; --------------------------------
; It is a complete, reversible mode transition with a payload that proves the
; processor really is in 64-bit mode: a register holding a value no 32-bit
; register can hold, arithmetic that carries across bit 31 in one
; instruction, the eight registers that do not exist in 32-bit mode,
; RIP-relative addressing, EFER.LMA read back from the processor, and a read
; through a 64-bit pointer resolved by a freshly built four-level page table.
;
; It is NOT a 64-bit kernel. StratumOS's own code - its scheduler, its
; allocators, its drivers - is 32-bit, and stays 32-bit. Porting it means a
; new interrupt descriptor format, a new calling convention, a new syscall
; mechanism (SYSCALL/SYSRET rather than a software interrupt), and auditing
; every place a pointer is assumed to be four bytes wide. That is the work
; docs/LONGMODE.md describes and docs/ROADMAP.md schedules; this file is the
; part that had to come first, because nothing else can be tested until the
; processor can be got into the mode and back out of it.
;
; WHY IT RUNS FROM A COPY AT A LOW PHYSICAL ADDRESS
; ------------------------------------------------
; The enable sequence requires turning paging *off* for a few instructions:
; CR4.PAE cannot be changed while CR0.PG is set. With paging off, EIP is a
; physical address - so the only code that can survive the switch is code
; whose virtual address and physical address are the same.
;
; The kernel lives at 0xC0100000 and is loaded at 0x00100000, so none of it
; qualifies. This file is therefore assembled into the kernel image but
; copied to LM_TRAMPOLINE_PHYS and executed there, with every absolute
; reference computed as
;
;       LM_TRAMPOLINE_PHYS + (label - lm_trampoline_start)
;
; exactly as kernel/arch/x86/ap_boot.asm does for the same reason. The caller
; switches to a page directory that identity-maps the first 4 MiB before
; calling in, and the four-level tables identity-map the first gigabyte, so
; this page is mapped at its own address under every regime the code passes
; through: 32-bit paging, no paging, 64-bit paging, and back.
;
; INTERRUPTS
; ----------
; Off for the entire window, and they have to be. Between loading our GDT and
; restoring the kernel's there is no interrupt descriptor table the processor
; could use - the kernel's IDT is 32-bit, and long mode's gate descriptors
; are sixteen bytes rather than eight, so the same table cannot serve both.
; An NMI or a machine check in the window would be fatal. That is an accepted
; cost of a demonstration and would not be acceptable in a port, which is one
; of the several reasons a port needs its own IDT before anything else.
; =============================================================================

%include "selectors.inc"

LM_TRAMPOLINE_PHYS equ 0x9000
LM_STACK_TOP       equ 0xA000      ; top of our own page; grows down into it

%define PHYS(label) (LM_TRAMPOLINE_PHYS + ((label) - lm_trampoline_start))

CR0_PG   equ 0x80000000
CR4_PAE  equ 0x00000020

MSR_EFER equ 0xC0000080
EFER_LME equ 0x00000100            ; long mode enable  (we set this)
EFER_LMA equ 0x00000400            ; long mode active  (the CPU sets this)

; Our own GDT deliberately uses the kernel's selector numbers for its 32-bit
; code and data descriptors, so that 0x08 and 0x10 mean the same thing on
; both sides of every transition below. 0x18 is the one the kernel's GDT has
; no equivalent for.
LM_SEL_CODE32 equ 0x08
LM_SEL_DATA   equ 0x10
LM_SEL_CODE64 equ 0x18

; Progress flags. Each is set by the code that reached that point, so a
; transition that dies partway through still says where - which during
; development was the difference between a bug and a reboot.
LM_F_ENTERED  equ 0x01             ; running on our GDT, 32-bit
LM_F_COMPAT   equ 0x02             ; CR0.PG set with the PML4: compat mode
LM_F_LONG     equ 0x04             ; executing 64-bit instructions
LM_F_VERIFIED equ 0x08             ; every probe written
LM_F_BACK32   equ 0x10             ; back to 32-bit, still 64-bit paging
LM_F_RETURNED equ 0x20             ; kernel CR3, kernel GDT, kernel stack

section .text

global lm_trampoline_start
global lm_trampoline_end
global lm_enter_long_mode_off

global lm_off_flags
global lm_off_cs
global lm_off_wide
global lm_off_crossed
global lm_off_r15
global lm_off_rip
global lm_off_efer
global lm_off_cr4
global lm_off_probe_value
global lm_off_probe_phys
global lm_off_pml4
global lm_off_return_cr3

[bits 32]
align 16
lm_trampoline_start:
                jmp     lm_entry32              ; 5 bytes
                times   11 db 0                 ; pad the block to +16

; ---- the parameter and result block, at +16 --------------------------------
;
; In: pml4, return_cr3, probe_phys. Out: everything else. One page, one
; layout, offsets exported below so the C side has no second copy of it.
align 16
lm_flags:       dd      0          ; +0   progress bitmap (see LM_F_*)
lm_cs:          dd      0          ; +4   the CS selector 64-bit code ran on
lm_wide:        dq      0          ; +8   a value wider than 32 bits
lm_crossed:     dq      0          ; +16  0xFFFFFFFF + 1, in one instruction
lm_r15:         dq      0          ; +24  a register that only exists here
lm_rip:         dq      0          ; +32  lea rax, [rel marker]
lm_efer:        dq      0          ; +40  EFER, for LMA
lm_cr4:         dq      0          ; +48  CR4, for PAE
lm_probe_value: dq      0          ; +56  read through a 64-bit pointer
lm_probe_phys:  dq      0          ; +64  IN: the physical address to read
lm_pml4:        dd      0          ; +72  IN: CR3 for long mode
lm_return_cr3:  dd      0          ; +76  IN: CR3 to restore on the way out
lm_saved_esp:   dd      0          ; +80  the kernel stack we were called on
lm_saved_cr0:   dd      0          ; +84
lm_saved_cr4:   dd      0          ; +88
lm_saved_gdtr:  dw      0          ; +92  six bytes: limit then base
                dd      0

lm_off_flags       equ lm_flags - lm_trampoline_start
lm_off_cs          equ lm_cs - lm_trampoline_start
lm_off_wide        equ lm_wide - lm_trampoline_start
lm_off_crossed     equ lm_crossed - lm_trampoline_start
lm_off_r15         equ lm_r15 - lm_trampoline_start
lm_off_rip         equ lm_rip - lm_trampoline_start
lm_off_efer        equ lm_efer - lm_trampoline_start
lm_off_cr4         equ lm_cr4 - lm_trampoline_start
lm_off_probe_value equ lm_probe_value - lm_trampoline_start
lm_off_probe_phys  equ lm_probe_phys - lm_trampoline_start
lm_off_pml4        equ lm_pml4 - lm_trampoline_start
lm_off_return_cr3  equ lm_return_cr3 - lm_trampoline_start

; The entry point, as an offset rather than an address: C calls
; LM_TRAMPOLINE_PHYS + this.
lm_enter_long_mode_off equ lm_entry32 - lm_trampoline_start

; ---- 32-bit protected mode, on the kernel's GDT ----------------------------
align 16
lm_entry32:
                ; Called with a near call from kernel C, so the return
                ; address is a kernel virtual one on the kernel's stack. Both
                ; stay mapped: the caller switched to a page directory that
                ; has the kernel's half as well as the identity map.
                pushad
                pushfd

                ; Everything needed to undo this, saved before anything is
                ; changed. CR4 is saved whole rather than having PAE cleared
                ; on the way back, because this kernel also sets SMEP, SMAP
                ; and PSE in there and restoring the register verbatim cannot
                ; get any of them wrong.
                mov     [PHYS(lm_saved_esp)], esp
                mov     eax, cr0
                mov     [PHYS(lm_saved_cr0)], eax
                mov     eax, cr4
                mov     [PHYS(lm_saved_cr4)], eax
                sgdt    [PHYS(lm_saved_gdtr)]

                ; Our own GDT. The kernel's has no 64-bit code descriptor,
                ; and - the part that actually forces the issue - its GDTR
                ; holds a kernel virtual base address, which is unmapped the
                ; moment paging goes off.
                lgdt    [PHYS(lm_gdtr)]
                jmp     dword LM_SEL_CODE32:PHYS(lm_on_our_gdt)

lm_on_our_gdt:
                mov     ax, LM_SEL_DATA
                mov     ds, ax
                mov     es, ax
                mov     fs, ax
                mov     gs, ax
                mov     ss, ax

                ; Our own stack, at a low physical address. The kernel's is
                ; at 0xE8xxxxxx and the four-level tables below map only the
                ; first gigabyte, so from here to the way back out there is
                ; no kernel stack to stand on.
                mov     esp, LM_STACK_TOP

                or      dword [PHYS(lm_flags)], LM_F_ENTERED

                ; ---- the enable sequence (Intel SDM vol 3A, 10.8.5) -------

                ; 1. Paging off. Legal here and nowhere else in this kernel,
                ;    because this code is identity-mapped: EIP becomes a
                ;    physical address and it is the physical address it
                ;    already was.
                mov     eax, cr0
                and     eax, ~CR0_PG
                mov     cr0, eax

                ; 2. CR4.PAE. Long mode is PAE-only - its four-level walk is
                ;    PAE's 64-bit entry format with one more level on top -
                ;    and this bit cannot be written while paging is on, which
                ;    is the whole reason for step 1.
                mov     eax, cr4
                or      eax, CR4_PAE
                mov     cr4, eax

                ; 3. CR3 = the PML4. Page tables are walked by physical
                ;    address, so the tables themselves need no mapping.
                mov     eax, [PHYS(lm_pml4)]
                mov     cr3, eax

                ; 4. EFER.LME, the one step that is a model-specific
                ;    register. rdmsr/wrmsr clobber EDX, which is why nothing
                ;    is being held in it.
                mov     ecx, MSR_EFER
                rdmsr
                or      eax, EFER_LME
                wrmsr

                ; 5. Paging on. The processor is now in IA-32e
                ;    *compatibility* mode: 64-bit paging under 32-bit code.
                ;    EFER.LMA has gone set by itself.
                mov     eax, cr0
                or      eax, CR0_PG
                mov     cr0, eax

                or      dword [PHYS(lm_flags)], LM_F_COMPAT

                ; 6. The step that is easy to leave out. Setting LME and PG
                ;    gets compatibility mode, not 64-bit mode; the processor
                ;    decodes 64-bit instructions only once CS holds a
                ;    descriptor whose L bit is set, and the only way to load
                ;    CS is a far transfer.
                jmp     LM_SEL_CODE64:PHYS(lm_entry64)

; ---- 64-bit long mode ------------------------------------------------------
[bits 64]
align 16
lm_entry64:
                ; In 64-bit mode DS, ES and SS are largely ignored, but
                ; leaving stale 32-bit selectors in them is the kind of thing
                ; that works until something reads one.
                mov     eax, LM_SEL_DATA
                mov     ds, ax
                mov     es, ax
                mov     ss, ax
                mov     rsp, LM_STACK_TOP

                ; One base register for the block, so every store below is
                ; unambiguously absolute. NASM's default in 64-bit mode is
                ; absolute [disp32] rather than RIP-relative, which is a
                ; difference worth not relying on either way.
                mov     edi, PHYS(lm_flags)

                or      dword [rdi + 0], LM_F_LONG

                ; --- a value no 32-bit register can hold -------------------
                ; movabs: a 64-bit immediate, an encoding that does not exist
                ; in 32-bit mode.
                mov     rax, 0x0123456789ABCDEF
                mov     [rdi + (lm_wide - lm_flags)], rax

                ; --- a carry across bit 31, in a single instruction --------
                ; In 32-bit mode the same two lines leave 0 in EAX. Here the
                ; result needs 33 bits and gets them.
                mov     rax, 0x00000000FFFFFFFF
                add     rax, 1
                mov     [rdi + (lm_crossed - lm_flags)], rax

                ; --- the eight registers that only exist here --------------
                mov     r15, 0xFEEDFACECAFEBEEF
                mov     [rdi + (lm_r15 - lm_flags)], r15

                ; --- RIP-relative addressing ------------------------------
                ; New in 64-bit mode, and the reason position-independent
                ; code is cheap there. The C side checks this against the
                ; address it copied the trampoline to, which makes it a test
                ; of the addressing mode rather than of a constant.
                lea     rax, [rel lm_marker]
                mov     [rdi + (lm_rip - lm_flags)], rax

                ; --- EFER, so LMA is read back from the processor ---------
                ; Not inferred from having set LME: LMA is the processor's
                ; own statement that it is in long mode.
                mov     ecx, MSR_EFER
                rdmsr                           ; EDX:EAX, both zero-extended
                shl     rdx, 32
                or      rax, rdx
                mov     [rdi + (lm_efer - lm_flags)], rax

                ; --- CR4, for PAE -----------------------------------------
                mov     rax, cr4
                mov     [rdi + (lm_cr4 - lm_flags)], rax

                ; --- a read through a 64-bit pointer ----------------------
                ; The address is a frame the C side allocated and filled with
                ; a magic number. Nothing but the four-level walk can resolve
                ; it: the 32-bit page directory this kernel was using does not
                ; map it, and the identity map the caller switched to covers
                ; only the first 4 MiB.
                mov     rax, [rdi + (lm_probe_phys - lm_flags)]
                mov     rbx, [rax]
                mov     [rdi + (lm_probe_value - lm_flags)], rbx

                ; --- what CS actually is ----------------------------------
                xor     eax, eax
                mov     ax, cs
                mov     [rdi + (lm_cs - lm_flags)], eax

                or      dword [rdi + 0], LM_F_VERIFIED

                ; ---- leaving, which is the enable sequence backwards ------
                ; A far jump to a descriptor with L clear and D set first.
                ; Clearing LME while the processor is decoding 64-bit
                ; instructions is not a defined thing to do.
                ;
                ; 64-bit mode has no direct far jump - opcode EA is invalid -
                ; so the destination goes through memory.
                jmp     far dword [rel lm_far_back32]

lm_marker:      dq      0x4C4F4E474D4F4445      ; "LONGMODE", for the lea above

align 8
lm_far_back32:  dd      PHYS(lm_back32)
                dw      LM_SEL_CODE32

; ---- 32-bit compatibility mode, on the way out -----------------------------
[bits 32]
align 16
lm_back32:
                mov     ax, LM_SEL_DATA
                mov     ds, ax
                mov     es, ax
                mov     fs, ax
                mov     gs, ax
                mov     ss, ax
                mov     esp, LM_STACK_TOP

                or      dword [PHYS(lm_flags)], LM_F_BACK32

                ; 1. Paging off again, for the same reason as before: PAE and
                ;    LME are both unwritable while CR0.PG is set.
                mov     eax, cr0
                and     eax, ~CR0_PG
                mov     cr0, eax

                ; 2. LME off. LMA follows it down on its own.
                mov     ecx, MSR_EFER
                rdmsr
                and     eax, ~EFER_LME
                wrmsr

                ; 3. CR4 back to exactly what the kernel had, which clears
                ;    PAE and restores SMEP, SMAP and PSE in one write.
                mov     eax, [PHYS(lm_saved_cr4)]
                mov     cr4, eax

                ; 4. The caller's page directory - the one with both the
                ;    identity map and the kernel's half. Not the kernel's own
                ;    directory: that has no low mapping, so loading it here
                ;    would unmap the instruction after the next one. C
                ;    switches back to the real one once there is a kernel
                ;    address to run at.
                mov     eax, [PHYS(lm_return_cr3)]
                mov     cr3, eax

                ; 5. CR0 verbatim, which puts PG and WP back together.
                mov     eax, [PHYS(lm_saved_cr0)]
                mov     cr0, eax

                ; 6. The kernel's GDT. Only possible now: its base is a
                ;    kernel virtual address, and the kernel's half has been
                ;    mapped again for exactly one instruction.
                lgdt    [PHYS(lm_saved_gdtr)]
                jmp     dword SEL_KERNEL_CODE:PHYS(lm_on_kernel_gdt)

lm_on_kernel_gdt:
                mov     ax, SEL_KERNEL_DATA
                mov     ds, ax
                mov     es, ax
                mov     fs, ax
                mov     gs, ax
                mov     ss, ax

                mov     esp, [PHYS(lm_saved_esp)]
                or      dword [PHYS(lm_flags)], LM_F_RETURNED

                ; The flags were pushed with interrupts already off, so this
                ; does not re-enable them behind the caller's back.
                popfd
                popad
                ret

; ---- the GDT this file runs on ---------------------------------------------
;
; Five descriptors. The 32-bit pair deliberately sit at the kernel's own
; selector numbers; the 64-bit code descriptor is the one that matters, and
; the only bit in it that does any work is L.
;
;   0x00  null
;   0x08  32-bit code    L=0 D=1    0x00CF9A000000FFFF
;   0x10  data           flat       0x00CF92000000FFFF
;   0x18  64-bit code    L=1 D=0    0x00AF9A000000FFFF
;   0x20  64-bit data    unused, present for symmetry
;
; In 64-bit mode base and limit are ignored for CS, DS, ES and SS - the
; 0000FFFF and the granularity bit below are there because the descriptor
; format still has the fields, not because anything reads them.
align 16
lm_gdt:
                dq      0x0000000000000000      ; null
                dq      0x00CF9A000000FFFF      ; 0x08 code32, D=1, L=0
                dq      0x00CF92000000FFFF      ; 0x10 data
                dq      0x00AF9A000000FFFF      ; 0x18 code64, L=1, D=0
                dq      0x00AF92000000FFFF      ; 0x20 data64
lm_gdt_end:

lm_gdtr:
                dw      lm_gdt_end - lm_gdt - 1
                dd      PHYS(lm_gdt)

align 16
lm_trampoline_end:

section .note.GNU-stack noalloc noexec nowrite progbits
