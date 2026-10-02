; =============================================================================
; StratumOS - GDT / TSS activation
; =============================================================================

%include "selectors.inc"

section .text

; void gdt_flush(const struct gdt_ptr *ptr)
;
; Loading the GDT does not change the hidden base/limit the CPU cached for each
; segment register. The data selectors are refreshed with plain MOVs; CS can
; only be reloaded by a far transfer, hence the far jump to the next
; instruction.
global gdt_flush
gdt_flush:
                mov     eax, [esp + 4]
                lgdt    [eax]

                mov     ax, SEL_KERNEL_DATA
                mov     ds, ax
                mov     es, ax
                mov     fs, ax
                mov     gs, ax
                mov     ss, ax

                jmp     SEL_KERNEL_CODE:.reload
.reload:
                ret

; void tss_flush(void) - load the task register with our one TSS descriptor.
global tss_flush
tss_flush:
                mov     ax, SEL_TSS
                ltr     ax
                ret

; void idt_flush(const struct idt_ptr *ptr)
global idt_flush
idt_flush:
                mov     eax, [esp + 4]
                lidt    [eax]
                ret

section .note.GNU-stack noalloc noexec nowrite progbits
