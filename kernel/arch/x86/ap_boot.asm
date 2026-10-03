; =============================================================================
; StratumOS - the application processor trampoline
; =============================================================================
;
; A processor taken out of reset by INIT-SIPI-SIPI begins executing in 16-bit
; real mode at CS = vector << 8, IP = 0. So the first instruction an
; application processor runs is at a physical address below 1 MiB, with no
; paging, no GDT it can trust, and a 64 KiB addressing limit - which is the
; same place the boot processor started, a second and a half earlier.
;
; This code gets it from there to a C function in the higher half. It is
; assembled into the kernel image but *runs* from a copy at
; AP_TRAMPOLINE_PHYS, so every absolute reference is computed as
;
;       AP_TRAMPOLINE_PHYS + (label - ap_trampoline_start)
;
; rather than written as a symbol. A symbol here would be a kernel virtual
; address, which is exactly what is not mapped yet.
;
; The parameter block
; -------------------
; The C code fills in four words before sending the startup message: the page
; directory to load, the entry point to jump to, the stack to use, and a flag
; the trampoline sets so the boot processor can tell "reached 32-bit mode"
; apart from "reached C". Their offsets are exported as absolute symbols, so
; there is one definition of the layout rather than one here and one in C.
;
; The bootstrap page directory
; ----------------------------
; Not the kernel's. The kernel's page directory has no identity mapping - it
; was dropped in vmm_init(), which is what frees the bottom of the address
; space for user processes - so enabling paging with it would unmap the
; instruction after `mov cr0`. The boot processor solved this at startup by
; having one temporarily; an application processor is handed a directory that
; identity-maps the first 4 MiB *and* contains the kernel's half, and C
; switches to the real one once it is running at a kernel address.
;
; Doing it this way rather than briefly adding a low mapping to the kernel's
; own directory means the kernel's address space is never in a state its own
; test suite would reject.
; =============================================================================

%include "selectors.inc"

AP_TRAMPOLINE_PHYS equ 0x8000

%define PHYS(label) (AP_TRAMPOLINE_PHYS + ((label) - ap_trampoline_start))

CR0_PE equ 0x00000001
CR0_WP equ 0x00010000
CR0_PG equ 0x80000000

section .text

global ap_trampoline_start
global ap_trampoline_end
global ap_param_pagedir_off
global ap_param_entry_off
global ap_param_stack_off
global ap_param_flag_off

[bits 16]
align 16
ap_trampoline_start:
                jmp     short ap_entry16        ; 2 bytes
                times   14 db 0                 ; pad the parameters to +16

; ---- the parameter block, at +16 -------------------------------------------
ap_param_pagedir:
                dd      0
ap_param_entry:
                dd      0
ap_param_stack:
                dd      0
ap_param_flag:
                dd      0

ap_param_pagedir_off equ ap_param_pagedir - ap_trampoline_start
ap_param_entry_off   equ ap_param_entry - ap_trampoline_start
ap_param_stack_off   equ ap_param_stack - ap_trampoline_start
ap_param_flag_off    equ ap_param_flag - ap_trampoline_start

; ---- 16-bit real mode ------------------------------------------------------
align 16
ap_entry16:
                cli
                cld

                ; A processor out of reset has CS = vector << 8 and every
                ; other segment at zero, but the BIOS may have left something
                ; else behind. Zeroing them makes the addresses below mean
                ; what they say.
                xor     ax, ax
                mov     ds, ax
                mov     es, ax
                mov     ss, ax
                mov     sp, AP_TRAMPOLINE_PHYS  ; scratch, below our own code

                ; A20 is already enabled: the boot processor's loader did it,
                ; and the gate is a property of the chipset rather than of a
                ; processor. Without that, every address below would alias.

                lgdt    [PHYS(ap_gdtr)]

                mov     eax, cr0
                or      eax, CR0_PE
                mov     cr0, eax

                ; The far jump is what actually loads CS with a 32-bit
                ; descriptor; setting CR0.PE alone leaves the CPU executing
                ; 16-bit code with a stale CS.
                jmp     dword SEL_KERNEL_CODE:PHYS(ap_entry32)

; ---- 32-bit protected mode, still at a physical address --------------------
[bits 32]
align 16
ap_entry32:
                mov     ax, SEL_KERNEL_DATA
                mov     ds, ax
                mov     es, ax
                mov     fs, ax
                mov     gs, ax
                mov     ss, ax

                ; Paging, with the bootstrap directory. Its identity map is
                ; what keeps the next instruction fetchable.
                mov     eax, [PHYS(ap_param_pagedir)]
                mov     cr3, eax

                mov     eax, cr0
                or      eax, CR0_PG | CR0_WP
                mov     cr0, eax

                ; Still executing at a low physical address, which the
                ; identity map still covers. Both halves of the address space
                ; are now live, so the switch to kernel addresses is a jump.
                mov     esp, [PHYS(ap_param_stack)]
                mov     eax, [PHYS(ap_param_entry)]

                ; Tell the boot processor that 32-bit mode and paging worked.
                ; If this is set and the CPU never comes online, the fault is
                ; between here and the first line of C.
                mov     dword [PHYS(ap_param_flag)], 1

                ; A zero frame pointer terminates a backtrace cleanly, which
                ; matters more here than usual: a fault on an application
                ; processor during bring-up has nothing above it on the stack.
                xor     ebp, ebp
                push    ebp
                jmp     eax

; ---- the GDT the trampoline uses -------------------------------------------
;
; Three descriptors: null, flat 32-bit code, flat 32-bit data. Deliberately
; not the kernel's GDT, whose address is a kernel virtual one; the real GDT is
; loaded from C, once there is a mapping for it.
align 16
ap_gdt:
                dq      0x0000000000000000      ; null
                dq      0x00CF9A000000FFFF      ; code: base 0, 4 GiB, ring 0
                dq      0x00CF92000000FFFF      ; data: base 0, 4 GiB, ring 0
ap_gdt_end:

ap_gdtr:
                dw      ap_gdt_end - ap_gdt - 1
                dd      PHYS(ap_gdt)

align 16
ap_trampoline_end:

section .note.GNU-stack noalloc noexec nowrite progbits
