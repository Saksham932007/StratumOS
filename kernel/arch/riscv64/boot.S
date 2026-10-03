# =============================================================================
# StratumOS - RISC-V 64 entry, and the machine-to-supervisor transition
# =============================================================================
#
# QEMU's `virt` machine with `-bios none` loads the ELF and starts hart 0 in
# MACHINE MODE at the entry point, with no firmware underneath. Every other
# hart starts here too, which is the first thing that has to be dealt with:
# on x86 the application processors stay in reset until the boot processor
# sends them a startup message, and here they are already running.
#
# a0 holds the hart id and a1 a pointer to the device tree. Both are supplied
# by the loader rather than discovered, which is the opposite of the x86 boot
# where the kernel has to ask the BIOS for a memory map.
#
# The sequence:
#
#   1. park every hart but hart 0            (they arrive already running)
#   2. a stack                               (sp is undefined at entry)
#   3. clear .bss                            (the loader does not)
#   4. machine-mode setup: mtvec, the timer, and delegation
#   5. mret into supervisor mode             (because M-mode cannot use satp)
#
# Step 5 is the one worth attention. A machine-mode hart cannot switch paging
# on for itself: M-mode fetches and loads bypass translation entirely, so
# `satp` has no effect on the code that wrote it. Supervisor mode is where a
# kernel with an MMU has to live, and `mret` is how you get there - with
# mstatus.MPP naming the mode to return *to* and mepc the address. It is the
# same manoeuvre as the far jump that loads a 32-bit CS on x86, for the same
# reason: the mode you are in cannot do what comes next.
# =============================================================================

#define MSTATUS_MPP_S   (1 << 11)
#define MSTATUS_MPP_MASK (3 << 11)
#define MSTATUS_MPIE    (1 << 7)
#define MSTATUS_SIE     (1 << 1)

        .section .text.boot
        .globl _start
_start:
        # ---- 1. one hart ----------------------------------------------------
        # Every hart enters here simultaneously. Only hart 0 continues; the
        # rest park in a wfi loop. Bringing them up is a separate exercise
        # (and needs the CLINT's software-interrupt registers rather than
        # x86's INIT-SIPI-SIPI), so they are parked deliberately rather than
        # left to race through the same initialisation.
        csrr    t0, mhartid
        bnez    t0, park_hart

        # ---- 2. a stack -----------------------------------------------------
        # sp is undefined at entry. The stack grows down from the top of the
        # reserved region, and there is a guard of zero pages below it in the
        # linker script - the same discipline as the x86 kernel stacks.
        la      sp, boot_stack_top

        # Keep what the loader handed us; the C code reports both.
        mv      s0, a0                  # hart id
        mv      s1, a1                  # device tree pointer

        # ---- 3. clear .bss --------------------------------------------------
        # Nothing else does this. The ELF says .bss occupies space with no
        # file contents, and QEMU's loader writes only what the file has - so
        # every zero-initialised variable in the kernel holds whatever was in
        # RAM until this loop runs.
        la      t0, __bss_start
        la      t1, __bss_end
        bgeu    t0, t1, bss_done
bss_loop:
        sd      zero, 0(t0)
        addi    t0, t0, 8
        bltu    t0, t1, bss_loop
bss_done:

        # ---- 4. machine mode setup -----------------------------------------
        # Done from C, which is possible because a stack and .bss are now
        # real. It installs mtvec, starts the CLINT timer, and delegates to
        # supervisor mode the traps a supervisor kernel should see.
        mv      a0, s0
        mv      a1, s1
        call    machine_setup           # returns the supervisor entry point
        mv      t0, a0

        # ---- 5. mret into supervisor mode ----------------------------------
        # MPP = S says which mode mret returns to; mepc says where. MPIE
        # clear means supervisor interrupts stay off until the supervisor
        # code asks for them, which is the same reason the x86 kernel does
        # not `sti` until its IDT is installed.
        csrr    t1, mstatus
        li      t2, MSTATUS_MPP_MASK
        not     t2, t2
        and     t1, t1, t2              # clear MPP
        li      t2, MSTATUS_MPP_S
        or      t1, t1, t2              # MPP = supervisor
        li      t2, MSTATUS_MPIE
        not     t2, t2
        and     t1, t1, t2              # MPIE = 0
        csrw    mstatus, t1

        csrw    mepc, t0

        # A hart in supervisor mode must not be able to reach memory the
        # kernel has not mapped, but physical memory protection is off by
        # default on this machine and configuring a PMP that denies nothing
        # is the documented way to say "S-mode may access all of it". Without
        # an entry, some implementations deny everything to S-mode and the
        # first supervisor instruction faults.
        li      t1, 0x3fffffffffffff    # NAPOT, the whole address space
        csrw    pmpaddr0, t1
        li      t1, 0x1f                # NAPOT | R | W | X
        csrw    pmpcfg0, t1

        mv      a0, s0
        mv      a1, s1
        mret                            # -> supervisor_main, in S-mode

park_hart:
        # The harts that are not hart 0. A real port brings them up through
        # the CLINT's MSIP registers; this one parks them, and says so.
        wfi
        j       park_hart

        # ---- the boot stack -------------------------------------------------
        # In .bss so it costs nothing in the image, and 16-byte aligned
        # because the RISC-V ABI requires sp to be.
        .section .bss.stack
        .balign 16
        .globl boot_stack
boot_stack:
        .space 16384
        .globl boot_stack_top
boot_stack_top:
