# =============================================================================
# StratumOS - RISC-V trap entry, for both privilege modes
# =============================================================================
#
# A RISC-V trap does far less for you than an x86 one. There is no stack
# switch, no automatic register push, no error code and no separate vector
# per cause: control simply arrives at mtvec/stvec with the cause in
# mcause/scause and the faulting address in mepc/sepc, and *every* register
# still holds whatever the interrupted code had in it.
#
# So the entire frame is this file's responsibility. x86's `pushad` has no
# counterpart; 31 registers are saved by hand.
#
# The other difference that matters: because there is no stack switch, a trap
# taken in supervisor mode continues on the *same* stack. That is fine here -
# there is no user mode in this port - but it is why a real kernel keeps a
# per-hart kernel stack in the `sscratch` CSR and swaps to it on entry, which
# is the role x86's TSS.esp0 plays.
# =============================================================================

#define FRAME_SIZE 256

        .macro SAVE_REGS
        addi    sp, sp, -FRAME_SIZE
        sd      x1,  0*8(sp)
        sd      x3,  2*8(sp)
        sd      x4,  3*8(sp)
        sd      x5,  4*8(sp)
        sd      x6,  5*8(sp)
        sd      x7,  6*8(sp)
        sd      x8,  7*8(sp)
        sd      x9,  8*8(sp)
        sd      x10, 9*8(sp)
        sd      x11, 10*8(sp)
        sd      x12, 11*8(sp)
        sd      x13, 12*8(sp)
        sd      x14, 13*8(sp)
        sd      x15, 14*8(sp)
        sd      x16, 15*8(sp)
        sd      x17, 16*8(sp)
        sd      x18, 17*8(sp)
        sd      x19, 18*8(sp)
        sd      x20, 19*8(sp)
        sd      x21, 20*8(sp)
        sd      x22, 21*8(sp)
        sd      x23, 22*8(sp)
        sd      x24, 23*8(sp)
        sd      x25, 24*8(sp)
        sd      x26, 25*8(sp)
        sd      x27, 26*8(sp)
        sd      x28, 27*8(sp)
        sd      x29, 28*8(sp)
        sd      x30, 29*8(sp)
        sd      x31, 30*8(sp)
        .endm

        .macro RESTORE_REGS
        ld      x1,  0*8(sp)
        ld      x3,  2*8(sp)
        ld      x4,  3*8(sp)
        ld      x5,  4*8(sp)
        ld      x6,  5*8(sp)
        ld      x7,  6*8(sp)
        ld      x8,  7*8(sp)
        ld      x9,  8*8(sp)
        ld      x10, 9*8(sp)
        ld      x11, 10*8(sp)
        ld      x12, 11*8(sp)
        ld      x13, 12*8(sp)
        ld      x14, 13*8(sp)
        ld      x15, 14*8(sp)
        ld      x16, 15*8(sp)
        ld      x17, 16*8(sp)
        ld      x18, 17*8(sp)
        ld      x19, 18*8(sp)
        ld      x20, 19*8(sp)
        ld      x21, 20*8(sp)
        ld      x22, 21*8(sp)
        ld      x23, 22*8(sp)
        ld      x24, 23*8(sp)
        ld      x25, 24*8(sp)
        ld      x26, 25*8(sp)
        ld      x27, 26*8(sp)
        ld      x28, 27*8(sp)
        ld      x29, 28*8(sp)
        ld      x30, 29*8(sp)
        ld      x31, 30*8(sp)
        addi    sp, sp, FRAME_SIZE
        .endm

        .section .text

# ---- machine mode -----------------------------------------------------------
#
# Only the timer arrives here: the CLINT's mtimecmp is a machine-mode device
# and machine timer interrupts cannot be delegated, so this vector exists
# for one cause. Everything else is sent to the supervisor vector below.
        .balign 4
        .globl machine_trap_entry
machine_trap_entry:
        SAVE_REGS
        csrr    a0, mcause
        csrr    a1, mepc
        csrr    a2, mtval
        call    machine_trap
        RESTORE_REGS
        mret

# ---- supervisor mode --------------------------------------------------------
        .balign 4
        .globl supervisor_trap_entry
supervisor_trap_entry:
        SAVE_REGS
        csrr    a0, scause
        csrr    a1, sepc
        csrr    a2, stval
        call    supervisor_trap
        # supervisor_trap returns the address to resume at, which lets it
        # step over a faulting instruction - the deliberate illegal-
        # instruction test depends on that, because returning to the same
        # address would fault forever.
        csrw    sepc, a0
        RESTORE_REGS
        sret

        .section .note.GNU-stack,"",@progbits
