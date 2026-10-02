; =============================================================================
; StratumOS - cooperative/preemptive context switch
; =============================================================================
;
; void context_switch(u32 *save_esp, u32 load_esp)
;
; The entire "context" of a kernel thread is its stack pointer. Push the
; callee-saved registers and the flags, record ESP in the outgoing task, load
; ESP from the incoming one, pop its registers back, and `ret` - which returns
; into whatever the *new* task was doing when it last gave up the CPU.
;
; Caller-saved registers (EAX, ECX, EDX) need no handling: the C compiler has
; already assumed they are destroyed across a call.
;
; Both arguments are read before ESP is touched; reading them afterwards would
; address the new stack and is a favourite way to produce an unreproducible
; crash.
; =============================================================================

section .text
global context_switch
context_switch:
                mov     eax, [esp + 4]          ; where to store the old ESP
                mov     edx, [esp + 8]          ; the ESP to switch to

                pushfd
                push    ebx
                push    esi
                push    edi
                push    ebp

                mov     [eax], esp              ; outgoing task is now parked
                mov     esp, edx                ; incoming task takes over

                pop     ebp
                pop     edi
                pop     esi
                pop     ebx
                popfd
                ret

; -----------------------------------------------------------------------------
; void thread_trampoline(void)
;
; The first thing a brand-new task "returns" into. task_create() lays out its
; stack so that `ret` above lands here with the entry point and argument
; waiting just above, which avoids needing a special case in the switch itself.
;
; Stack on arrival:  [esp+0] unused return slot
;                    [esp+4] entry function
;                    [esp+8] argument
; -----------------------------------------------------------------------------
                extern  task_exit
global thread_trampoline
thread_trampoline:
                sti                             ; new tasks run preemptible
                mov     eax, [esp + 4]
                push    dword [esp + 8]         ; argument
                call    eax

                ; A task entry point that returns is treated as exit(0).
                add     esp, 4
                push    dword 0
                call    task_exit
.unreachable:
                hlt
                jmp     .unreachable

section .note.GNU-stack noalloc noexec nowrite progbits
