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
;   2. Checks that the processor has PAE, and says so and stops if it does
;      not. The kernel is PAE-only because the no-execute bit does not exist
;      in the 32-bit page-table entry format - see mm/vmm.h.
;   3. Builds a three-level PAE page table set:
;        - two page tables covering the first 4 MiB, one per 2 MiB
;        - a page directory per gigabyte, each with entry 511 pointing at
;          itself, which is the window the VMM writes page tables through
;        - gigabyte 0's directory maps the first 4 MiB identity, so that the
;          instruction after `mov cr0` is still fetchable
;        - gigabyte 3's directory maps 0xC0000000 -> 0x00000000, which is
;          where the kernel is linked and is also how it reaches low physical
;          memory (the VGA framebuffer, the loader's tables) afterwards
;        - a PDPT of four entries, one per directory
;   4. Enables PAE, enables paging, and long-jumps into the higher half.
;   5. Clears the BSS, sets up the stack, and calls kmain.
;
; Nothing here sets the no-execute bit, although every table it builds is in
; the format that has one. Bit 63 is *reserved* until EFER.NXE is set, and a
; reserved bit in a live entry faults every walk through it - so the bit
; cannot be set before C has read CPUID and written the MSR. vmm_init() goes
; back and sets it on the self-map entries once that is done.
;
; Register discipline matters here, and it is not free. The loader hands over
; `EAX` = protocol magic and `EBX` = info pointer, and neither may reach kmain
; clobbered. The magic is parked in ESI. EBX cannot simply be left alone any
; more, because the PAE check uses CPUID and CPUID writes EBX - so it is
; parked in EBP across that one instruction. The page-table loops use
; EAX/ECX/EDX/EDI and touch neither.
;
; The first version of this file did leave EBX alone, and adding the CPUID
; check broke the handover: the kernel panicked with "no usable boot
; information" on both loaders at once, which is what a clobbered info pointer
; looks like from the far end.
; =============================================================================

KERNEL_VIRT_BASE equ 0xC0000000
MB2_MAGIC        equ 0xE85250D6
MB2_ARCH_I386    equ 0

PDE_PRESENT      equ 0x001
PDE_WRITE        equ 0x002
CR0_PG           equ 0x80000000
CR0_WP           equ 0x00010000
CR4_PAE          equ 0x00000020
CPUID_PAE        equ (1 << 6)             ; leaf 1, EDX

; A PAE entry is eight bytes, so an index is scaled by 8 and a table holds 512
; of them. The PDPT slot covering 0xC0000000 is 0xC0000000 >> 30 == 3, and the
; self-map lives in slot 511 of every directory.
ENTRY_SIZE       equ 8
PD_ENTRIES       equ 512
KERNEL_PDPT_SLOT equ (KERNEL_VIRT_BASE >> 30)
PD_SELF_SLOT     equ 511

; Point a directory's slot 511 at the directory itself. A macro rather than a
; subroutine because there is no stack yet: Multiboot leaves ESP undefined,
; and the kernel's own stack is in .bss, which is not addressable until the
; jump into the higher half.
%macro SELF_MAP 1
                mov     eax, %1
                or      eax, PDE_PRESENT | PDE_WRITE
                mov     [%1 + PD_SELF_SLOT * ENTRY_SIZE], eax
                mov     dword [%1 + PD_SELF_SLOT * ENTRY_SIZE + 4], 0
%endmacro

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
; The boot paging structures.
;
; Seven pages, built once and then owned by the VMM. The two page tables are
; deliberately shared between the identity window (0x00000000-0x003FFFFF) and
; the kernel window (0xC0000000-0xC03FFFFF), which map the same physical
; range: the same 1024 entries serve both, and that sharing is also what makes
; low physical memory reachable from the higher half once the identity map is
; gone.
;
; All four directories exist from the start, even though gigabytes 1 and 2 are
; empty, so that "every PDPT entry is present" holds in the kernel's own
; address space as well as in the ones the VMM creates. The page-table window
; relies on it, and the alternative is a PDPT written at runtime - which the
; processor caches in four internal registers and does not re-read until CR3
; is reloaded. Writing it exactly once, here, removes that trap.
; -----------------------------------------------------------------------------
section .boot.pagetables
                align   4096
global boot_pdpt
boot_pdpt:
                times   4096 db 0
boot_pd0:                                       ; gigabyte 0: the identity map
                times   4096 db 0
boot_pd1:                                       ; gigabyte 1: empty
                times   4096 db 0
boot_pd2:                                       ; gigabyte 2: empty
                times   4096 db 0
global boot_page_directory
boot_page_directory:                            ; gigabyte 3: the kernel
                times   4096 db 0
global boot_page_table
boot_page_table:                                ; 0x000000-0x1FFFFF
                times   4096 db 0
boot_page_table2:                               ; 0x200000-0x3FFFFF
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
                ; loops below do not touch ESI or EBX.
                mov     esi, eax

                ; --- PAE or nothing ----------------------------------------
                ; CPUID itself is assumed here, which is safe: a processor
                ; without CPUID is a 486 or earlier and has no PAE either, so
                ; the check would reach the same conclusion by a longer
                ; route. The message goes straight to the VGA text buffer,
                ; because there is no console, no paging and no C yet.
                ; CPUID clobbers EBX, which is the loader's information
                ; pointer and must survive to kmain. EBP is the one register
                ; nothing between here and the higher-half jump touches.
                mov     ebp, ebx

                mov     eax, 1
                cpuid
                test    edx, CPUID_PAE
                jnz     .has_pae

                mov     edi, 0xB8000
                mov     esi, no_pae_msg
.say:
                mov     al, [esi]
                test    al, al
                jz      .said
                mov     [edi], al
                mov     byte [edi + 1], 0x4F    ; white on red
                inc     esi
                add     edi, 2
                jmp     .say
.said:
                cli
.dead:
                hlt
                jmp     .dead

.has_pae:
                mov     ebx, ebp                ; the information pointer back

                ; --- fill the two page tables: 1024 entries, 0-4 MiB -------
                ; .boot has VMA == LMA, so these symbols are already physical
                ; and need no adjustment. That is the whole reason this code
                ; lives in its own section.
                ;
                ; The high dword of every entry is zero: the frame is below
                ; 4 GiB, and bit 63 must stay clear until EFER.NXE is set.
                mov     edi, boot_page_table
                xor     eax, eax                ; physical address being mapped
                mov     ecx, 1024
.fill_pt:
                mov     edx, eax
                or      edx, PDE_PRESENT | PDE_WRITE
                mov     [edi], edx
                mov     dword [edi + 4], 0
                add     edi, ENTRY_SIZE
                add     eax, 4096
                dec     ecx
                jnz     .fill_pt

                ; --- the four directories ----------------------------------
                ; Entry 511 of each points at the directory itself, so that
                ; every page table is reachable at a computable address from
                ; the first instruction of C. See the comment at the top of
                ; kernel/mm/vmm.c.
                SELF_MAP boot_pd0
                SELF_MAP boot_pd1
                SELF_MAP boot_pd2
                SELF_MAP boot_page_directory

                ; The first 4 MiB, twice: identity in gigabyte 0, and at
                ; 0xC0000000 in gigabyte 3. 0xC0000000 is 1 GiB-aligned, so
                ; its directory index is 0 and the two look identical.
                mov     eax, boot_page_table
                or      eax, PDE_PRESENT | PDE_WRITE
                mov     edx, boot_page_table2
                or      edx, PDE_PRESENT | PDE_WRITE

                mov     [boot_pd0], eax
                mov     dword [boot_pd0 + 4], 0
                mov     [boot_pd0 + ENTRY_SIZE], edx
                mov     dword [boot_pd0 + ENTRY_SIZE + 4], 0

                mov     [boot_page_directory], eax
                mov     dword [boot_page_directory + 4], 0
                mov     [boot_page_directory + ENTRY_SIZE], edx
                mov     dword [boot_page_directory + ENTRY_SIZE + 4], 0

                ; --- the PDPT ----------------------------------------------
                ; A PDPT entry has a present bit, two cache-control bits and
                ; an address. Bits 1, 2 and 5-8 are reserved and a non-zero
                ; one faults, so there is no WRITE bit to set at this level.
                mov     edi, boot_pdpt
                mov     eax, boot_pd0
                or      eax, PDE_PRESENT
                mov     [edi], eax
                mov     eax, boot_pd1
                or      eax, PDE_PRESENT
                mov     [edi + 1 * ENTRY_SIZE], eax
                mov     eax, boot_pd2
                or      eax, PDE_PRESENT
                mov     [edi + 2 * ENTRY_SIZE], eax
                mov     eax, boot_page_directory
                or      eax, PDE_PRESENT
                mov     [edi + KERNEL_PDPT_SLOT * ENTRY_SIZE], eax

                ; --- enable paging -----------------------------------------
                ; PAE first, and necessarily before CR0.PG: the processor
                ; refuses to change CR4.PAE while paging is on, and loading
                ; CR3 with a PDPT while it is off would have it read the
                ; 32-bit entry format out of 64-bit tables.
                mov     eax, cr4
                or      eax, CR4_PAE
                mov     cr4, eax

                mov     eax, boot_pdpt
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

no_pae_msg:
                db      "StratumOS needs a processor with PAE (Pentium Pro "
                db      "or later): the no-execute bit does not exist "
                db      "without it.", 0

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
