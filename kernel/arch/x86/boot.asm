; =============================================================================
; StratumOS - kernel entry point
; =============================================================================
;
; This object provides two things:
;
;   1. A Multiboot2 header, so GRUB (or `qemu -kernel`) can load this ELF
;      directly. The linker script places .multiboot first, which keeps the
;      header inside the first 32 KiB of the file as the spec requires.
;
;   2. A single `_start` that works for *both* supported boot protocols.
;      Neither GRUB nor our own stage 2 knows about the other; both leave a
;      magic value in EAX and a pointer in EBX, and C code sorts it out.
; =============================================================================

MB2_MAGIC       equ 0xE85250D6
MB2_ARCH_I386   equ 0

; -----------------------------------------------------------------------------
; Multiboot2 header
; -----------------------------------------------------------------------------
section .multiboot
                align   8
mb2_start:
                dd      MB2_MAGIC
                dd      MB2_ARCH_I386
                dd      mb2_end - mb2_start
                dd      -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_end - mb2_start))

                ; Information request tag: ask the loader for the things the
                ; kernel actually needs. Declaring them is how we find out at
                ; boot time, rather than at debug time, if a loader cannot
                ; supply one.
                align   8
                dw      1                       ; type: information request
                dw      0                       ; flags: 0 = required
                dd      8 + 4 * 4               ; size
                dd      1                       ; boot command line
                dd      2                       ; boot loader name
                dd      4                       ; basic memory info
                dd      6                       ; memory map

                ; End tag
                align   8
                dw      0
                dw      0
                dd      8
mb2_end:

; -----------------------------------------------------------------------------
; Kernel stack. Lives in .bss so it costs nothing in the image on disk.
; -----------------------------------------------------------------------------
section .bss
                align   16
global stack_bottom
global stack_top
stack_bottom:
                resb    32768                   ; 32 KiB
stack_top:

; -----------------------------------------------------------------------------
; Entry point
; -----------------------------------------------------------------------------
section .text
                extern  kmain
                extern  __bss_start
                extern  __bss_end
global _start
_start:
                cli
                cld

                ; EAX = boot protocol magic, EBX = info pointer.
                ; Stash the magic in ESI; EBX survives the loop below, so the
                ; info pointer can stay where the loader put it.
                mov     esi, eax

                ; Zero .bss before touching anything that lives in it. Both
                ; GRUB and our stage 2 already do this (p_memsz > p_filesz),
                ; but doing it here means the kernel does not *depend* on the
                ; loader having been careful. No stack is required, which is
                ; why this runs before ESP is valid.
                mov     edi, __bss_start
                mov     ecx, __bss_end
                sub     ecx, edi
                xor     eax, eax
                rep     stosb

                mov     esp, stack_top
                push    0
                popfd                           ; clear EFLAGS: IF, DF, NT...
                xor     ebp, ebp                ; terminates the backtrace walk

                push    ebx                     ; kmain(magic, info_addr)
                push    esi
                call    kmain

                ; kmain is not supposed to return. If it does, stop cleanly
                ; rather than executing whatever follows in memory.
                cli
.hang:
                hlt
                jmp     .hang

; Mark the stack non-executable for toolchains that look for this note.
section .note.GNU-stack noalloc noexec nowrite progbits
