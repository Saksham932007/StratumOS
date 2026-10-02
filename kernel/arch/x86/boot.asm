; =============================================================================
; StratumOS - kernel entry point and the jump to the higher half
; =============================================================================
;
; Everything in this file lives in .boot, whose virtual address equals its load
; address (see linker/kernel.ld). That is a hard requirement: this code runs
; with paging disabled, so it cannot be at 0xC0100000 - nothing is mapped there
; until it does the mapping.
;
; What it does, in order:
;
;   1. Publishes a Multiboot2 header, so GRUB can load the ELF directly. It is
;      in .boot because a loader searches the first 32 KiB of the *file*.
;   2. Builds a page directory with three entries:
;        - PDE 0    identity-maps the first 4 MiB, so that the instruction
;                   after `mov cr0` is still fetchable
;        - PDE 768  maps 0xC0000000 -> 0x00000000, which is where the kernel
;                   is linked and is also how it reaches low physical memory
;                   (the VGA framebuffer, the loader's tables) afterwards
;        - PDE 1023 points at the directory itself, so the VMM's recursive
;                   window works from the very first instruction of C
;   3. Enables paging and long-jumps into the higher half.
;   4. Clears the BSS, sets up the stack, and calls kmain.
;
; Register discipline matters here. The loader hands over `EAX` = protocol
; magic and `EBX` = info pointer, and neither may be clobbered before kmain.
; The page-table loop uses EAX/ECX/EDX/EDI, so the magic is parked in ESI and
; EBX is simply left alone.
; =============================================================================

KERNEL_VIRT_BASE equ 0xC0000000
MB2_MAGIC        equ 0xE85250D6
MB2_ARCH_I386    equ 0

PDE_PRESENT      equ 0x001
PDE_WRITE        equ 0x002
CR0_PG           equ 0x80000000
CR0_WP           equ 0x00010000

; The directory slot covering 0xC0000000: 0xC0000000 >> 22 == 768.
KERNEL_PDE_INDEX equ (KERNEL_VIRT_BASE >> 22)
RECURSIVE_SLOT   equ 1023

; -----------------------------------------------------------------------------
; Multiboot2 header. Must be 8-byte aligned and inside the first 32 KiB.
; -----------------------------------------------------------------------------
section .multiboot
                align   8
mb2_start:
                dd      MB2_MAGIC
                dd      MB2_ARCH_I386
                dd      mb2_end - mb2_start
                dd      -(MB2_MAGIC + MB2_ARCH_I386 + (mb2_end - mb2_start))

                ; Ask the loader for what the kernel actually needs.
                ; Declaring it is how a loader that cannot supply one fails at
                ; boot rather than at debug time.
                align   8
                dw      1                       ; information request
                dw      0                       ; flags: 0 = required
                dd      8 + 4 * 4
                dd      1                       ; boot command line
                dd      2                       ; boot loader name
                dd      4                       ; basic memory info
                dd      6                       ; memory map

                align   8
                dw      0                       ; end tag
                dw      0
                dd      8
mb2_end:

; -----------------------------------------------------------------------------
; The boot page directory and its single page table.
;
; One page table is enough for both mappings, and it is deliberately shared:
; the identity window maps 0x00000000-0x003FFFFF, and the kernel window maps
; 0xC0000000-0xC03FFFFF to the same physical range, so the same 1024 entries
; serve both. That is also what makes low physical memory reachable from the
; higher half once the identity map is gone.
; -----------------------------------------------------------------------------
section .boot.pagetables
                align   4096
global boot_page_directory
boot_page_directory:
                times   4096 db 0
global boot_page_table
boot_page_table:
                times   4096 db 0

; -----------------------------------------------------------------------------
; Kernel stack. In .bss, so it costs nothing in the image.
; -----------------------------------------------------------------------------
section .bss
                align   16
global stack_bottom
global stack_top
stack_bottom:
                resb    32768                   ; 32 KiB
stack_top:

; -----------------------------------------------------------------------------
; Entry point. Physical addresses only until the jump.
; -----------------------------------------------------------------------------
section .text.boot
                extern  kmain
                extern  __bss_start
                extern  __bss_end
global _start
_start:
                cli
                cld

                ; EAX = magic, EBX = info pointer. Park the magic in ESI; the
                ; loop below does not touch ESI or EBX.
                mov     esi, eax

                ; --- fill the page table: 1024 entries covering 0-4 MiB ----
                ; .boot has VMA == LMA, so these symbols are already physical
                ; and need no adjustment. That is the whole reason this code
                ; lives in its own section.
                mov     edi, boot_page_table
                xor     eax, eax                ; physical address being mapped
                mov     ecx, 1024
.fill_pt:
                mov     edx, eax
                or      edx, PDE_PRESENT | PDE_WRITE
                mov     [edi], edx
                add     edi, 4
                add     eax, 4096
                dec     ecx
                jnz     .fill_pt

                ; --- three directory entries -------------------------------
                mov     edi, boot_page_directory

                mov     eax, boot_page_table
                or      eax, PDE_PRESENT | PDE_WRITE

                ; PDE 0: identity. Needed only for the handful of instructions
                ; between enabling paging and jumping high; dropped later by
                ; vmm_init() once nothing holds a low pointer.
                mov     [edi], eax

                ; PDE 768: the kernel's own window, and its view of low
                ; physical memory from then on.
                mov     [edi + KERNEL_PDE_INDEX * 4], eax

                ; PDE 1023: the directory itself, so that page tables are
                ; reachable at 0xFFC00000 + i*4096 as soon as paging is on.
                ; See the comment at the top of kernel/mm/vmm.c.
                mov     eax, boot_page_directory
                or      eax, PDE_PRESENT | PDE_WRITE
                mov     [edi + RECURSIVE_SLOT * 4], eax

                ; --- enable paging -----------------------------------------
                mov     eax, boot_page_directory
                mov     cr3, eax

                mov     eax, cr0
                ; WP as well as PG: without it, ring 0 may write to pages it
                ; marked read-only and the protection is decorative.
                or      eax, CR0_PG | CR0_WP
                mov     cr0, eax

                ; Still executing through the identity map. An absolute jump
                ; is what moves execution to the linked addresses - a relative
                ; one would stay down here.
                lea     eax, [higher_half]
                jmp     eax

; -----------------------------------------------------------------------------
; From here on, EIP is in the higher half and every symbol means what it says.
; -----------------------------------------------------------------------------
section .text
higher_half:
                ; The stack lives in .bss, so it is only addressable now.
                mov     esp, stack_top

                ; Clear the BSS. Both loaders already do this (p_memsz exceeds
                ; p_filesz), but doing it here means the kernel does not depend
                ; on either having been careful.
                mov     edi, __bss_start
                mov     ecx, __bss_end
                sub     ecx, edi
                xor     eax, eax
                rep     stosb

                push    0
                popfd                           ; clear EFLAGS: IF, DF, NT...
                xor     ebp, ebp                ; terminates the backtrace walk

                push    ebx                     ; kmain(magic, info_addr)
                push    esi
                call    kmain

                ; kmain does not return. If it does, stop rather than
                ; executing whatever follows in memory.
                cli
.hang:
                hlt
                jmp     .hang

section .note.GNU-stack noalloc noexec nowrite progbits
