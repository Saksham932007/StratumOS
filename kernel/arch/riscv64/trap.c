/* StratumOS - trap dispatch for both privilege modes.
 *
 * See trap.S for why the frame is hand-written, and riscv.h for why there
 * are two vectors rather than one IDT.
 */
#define LOG_TAG "trap"

#include <arch/riscv64/riscv.h>

#include <kernel/log.h>

void timer_tick(void);

/* Counted per cause rather than in total, because "a trap happened" is not a
 * diagnosis and the x86 side learned the same lesson with its per-IRQ
 * counters. */
#define MAX_CAUSE 24
static u64 counts[MAX_CAUSE];
static u64 total;

static const char *exception_name(u64 code)
{
    switch (code) {
    case EXC_INST_MISALIGNED:
        return "instruction address misaligned";
    case EXC_INST_FAULT:
        return "instruction access fault";
    case EXC_ILLEGAL_INST:
        return "illegal instruction";
    case EXC_BREAKPOINT:
        return "breakpoint";
    case EXC_LOAD_MISALIGNED:
        return "load address misaligned";
    case EXC_LOAD_FAULT:
        return "load access fault";
    case EXC_STORE_MISALIGNED:
        return "store address misaligned";
    case EXC_STORE_FAULT:
        return "store access fault";
    case EXC_ECALL_U:
        return "ecall from user mode";
    case EXC_ECALL_S:
        return "ecall from supervisor mode";
    case EXC_ECALL_M:
        return "ecall from machine mode";
    case EXC_INST_PAGE_FAULT:
        return "instruction page fault";
    case EXC_LOAD_PAGE_FAULT:
        return "load page fault";
    case EXC_STORE_PAGE_FAULT:
        return "store page fault";
    default:
        return "unknown";
    }
}

/* How long is the instruction at `pc`?
 *
 * RISC-V encodes the length in the first halfword: if its low two bits are
 * not both set the instruction is a 16-bit compressed one, otherwise it is
 * 32 bits. (Longer encodings are reserved and this kernel cannot generate
 * them.) Reading the instruction stream from a trap handler is safe here
 * because the kernel is identity-mapped and the fetch that trapped already
 * proved the address is readable. */
static u64 instruction_length(u64 pc)
{
    u16 first = *(const volatile u16 *)(uptr)pc;

    return ((first & 0x3u) == 0x3u) ? 4 : 2;
}

void trap_init_machine(void)
{
    extern void machine_trap_entry(void);

    /* The low two bits of mtvec select the mode: 0 is "direct", meaning
     * every cause arrives at the same address. The alternative, "vectored",
     * multiplies the cause by four and jumps into a table - closer to an x86
     * IDT, and not needed when one cause is expected here. */
    csr_write(mtvec, (u64)(uptr)machine_trap_entry);
}

void trap_init_supervisor(void)
{
    extern void supervisor_trap_entry(void);

    csr_write(stvec, (u64)(uptr)supervisor_trap_entry);
}

/* Machine mode. The timer, and nothing else should arrive. */
void machine_trap(u64 cause, u64 epc, u64 tval)
{
    total++;

    if (cause & CAUSE_INTERRUPT_FLAG) {
        u64 code = cause & ~CAUSE_INTERRUPT_FLAG;

        if (code < MAX_CAUSE)
            counts[code]++;

        if (code == IRQ_M_TIMER) {
            timer_tick();
            return;
        }

        pr_warn("unexpected machine interrupt %llu", (u64)code);
        return;
    }

    /* An exception in machine mode is a kernel bug by definition: there is
     * no more privileged mode to handle it and nothing to deliver it to. The
     * x86 side panics on the same reasoning. */
    if (cause < MAX_CAUSE)
        counts[cause]++;

    pr_err("machine-mode exception %llu (%s) at pc %p, tval %p", (u64)cause,
           exception_name(cause), (void *)(uptr)epc, (void *)(uptr)tval);
    machine_exit(false, 2);
}

/* Supervisor mode. Everything delegated: ecall, illegal instruction, page
 * faults. Returns the address to resume at. */
u64 supervisor_trap(u64 cause, u64 epc, u64 tval)
{
    total++;

    if (cause & CAUSE_INTERRUPT_FLAG) {
        u64 code = cause & ~CAUSE_INTERRUPT_FLAG;

        if (code < MAX_CAUSE)
            counts[code]++;
        pr_warn("supervisor interrupt %llu", (u64)code);
        return epc;
    }

    if (cause < MAX_CAUSE)
        counts[cause]++;

    switch (cause) {
    case EXC_ECALL_S:
    case EXC_BREAKPOINT:
    case EXC_ILLEGAL_INST:
        /* Resume *after* the faulting instruction.
         *
         * sepc points at the instruction that trapped, not the one after -
         * unlike an x86 trap gate, where the saved EIP is already past a
         * software interrupt. So returning to sepc unchanged re-executes it
         * and traps again, forever.
         *
         * How far "after" is has to be decoded, not assumed. This kernel is
         * built for rv64imac, and the C extension means an instruction may
         * be two bytes or four; the length is in the low bits of the opcode.
         *
         * The first version returned epc + 4 unconditionally, with a comment
         * claiming `ecall` and `unimp` were both four bytes. `ecall` is.
         * `unimp` assembles to the compressed `c.unimp`, which is two - so
         * the handler resumed two bytes into the *next* instruction, trapped
         * again on the garbage that decoded from there, and the illegal
         * instruction count came out as two. It also corrupted a global on
         * the way, because execution briefly continued through whatever
         * those bytes happened to mean.
         *
         * A wrong constant that is right for one of its two callers is the
         * worst kind, and x86 has no equivalent exposure here: its trap
         * frames carry the length implicitly by pointing past the
         * instruction. */
        return epc + instruction_length(epc);

    default:
        pr_err("supervisor exception %llu (%s) at pc %p, tval %p", (u64)cause,
               exception_name(cause), (void *)(uptr)epc, (void *)(uptr)tval);
        machine_exit(false, 3);
    }
}

u64 trap_count(u64 cause)
{
    return cause < MAX_CAUSE ? counts[cause] : 0;
}

u64 trap_total(void)
{
    return total;
}

NORETURN void machine_exit(bool pass, u16 code)
{
    /* The SiFive test device. Writing the command plus an exit code makes
     * QEMU terminate with a status a CI harness can read - the same job
     * `-device isa-debug-exit` does on x86, and the reason both
     * architectures can be tested unattended. */
    mmio_write32(VIRT_TEST, ((u32)code << 16) | (pass ? TEST_FINISHER_PASS
                                                      : TEST_FINISHER_FAIL));

    /* Not reached on QEMU. On hardware without the device the write is
     * inert, so there has to be something after it. */
    for (;;)
        wfi();
}
