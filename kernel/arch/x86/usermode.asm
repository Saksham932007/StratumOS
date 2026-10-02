; =============================================================================
; StratumOS - ring 0 -> ring 3 transition
; =============================================================================
;
; There is no instruction for "lower my privilege level". The only way down is
; to return to somewhere that was never privileged, so we forge the stack frame
; an inter-privilege interrupt return expects and execute IRET against it.
;
; When IRET sees a target CS whose RPL is numerically greater than the current
; CPL, it additionally pops SS and ESP - which is why a ring-3 frame is five
; dwords and a ring-0 one is three.
;
; Getting back is the CPU's problem: a trap, an IRQ or `int 0x80` switches to
; the ring-0 stack recorded in the TSS's esp0 field.
; =============================================================================

%include "selectors.inc"

section .text

; NORETURN void usermode_enter(u32 entry, u32 user_stack_top)
global usermode_enter
usermode_enter:
                mov     eax, [esp + 4]          ; entry point
                mov     edx, [esp + 8]          ; top of the user stack

                ; Data segments can be demoted directly; only CS and SS need
                ; the IRET dance.
                mov     cx, SEL_USER_DATA
                mov     ds, cx
                mov     es, cx
                mov     fs, cx
                mov     gs, cx

                push    dword SEL_USER_DATA     ; SS
                push    edx                     ; ESP

                pushfd                          ; EFLAGS, with adjustments
                pop     ecx
                or      ecx, 0x200              ; IF: stay preemptible
                and     ecx, ~0x100             ; TF: no single-stepping
                push    ecx

                push    dword SEL_USER_CODE     ; CS  (RPL 3 triggers the
                push    eax                     ; EIP  stack switch)
                iret

section .note.GNU-stack noalloc noexec nowrite progbits
