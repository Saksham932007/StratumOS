/* StratumOS - unrecoverable failures.
 *
 * A panic has one job: leave behind enough evidence to diagnose the fault
 * without a debugger attached. That means the message, the full register
 * state when there is one, and a stack trace.
 *
 * The backtrace walks the saved frame-pointer chain. It only works because
 * the kernel is compiled with -fno-omit-frame-pointer, which is a real
 * (small) cost paid deliberately to buy debuggability.
 */
#include <arch/cpu.h>
#include <arch/idt.h>
#include <arch/io.h>

#include <drivers/vga.h>

#include <kernel/console.h>
#include <kernel/kernel.h>
#include <kernel/ksyms.h>
#include <kernel/panic.h>
#include <kernel/printf.h>
#include <kernel/smp.h>

/* Set while a panic is in progress so that a fault *inside* the panic handler
 * stops rather than recursing until the stack is gone. */
static bool panicking;

static bool plausible_code_address(u32 addr)
{
    return addr >= (u32)__text_start && addr < (u32)__text_end;
}

void backtrace(u32 ebp, unsigned max_frames)
{
    /* The frames below are *return* addresses, so the list names the callers
     * of the faulting function, not the function itself - that one is EIP in
     * the register dump above. Saying so saves the reader the confusion of
     * looking for a function they can see is missing.
     *
     * Resolve any of these with:
     *     addr2line -f -e build/stratum.debug.elf <address>
     */
    kprintf(
        "Call trace (return addresses; the faulting frame is EIP above):\n");

    for (unsigned depth = 0; depth < max_frames; depth++) {
        u32 *frame = (u32 *)ebp;

        /* Stop at an implausible frame rather than following it into the
         * weeds. Frame pointers are 4-byte aligned and the chain grows
         * upward; _start zeroes EBP so a complete walk terminates on 0. */
        if (ebp == 0 || (ebp & 3) != 0 || ebp < 0x1000)
            break;

        u32 ret = frame[1];
        if (!plausible_code_address(ret))
            break;

        kprintf("  [%u] %p  ", depth, (void *)ret);
        ksym_print(ret);
        kprintf("\n");

        u32 next = frame[0];
        if (next <= ebp) /* must move up the stack, or we would loop forever */
            break;
        ebp = next;
    }
}

static void panic_banner(void)
{
    vga_set_attr(vga_attr(VGA_WHITE, VGA_RED));
    kprintf("\n");
    kprintf(
        "================================================================\n");
    kprintf(" KERNEL PANIC - " STRATUM_NAME " " STRATUM_VERSION "\n");
    kprintf(
        "================================================================\n");
}

static NORETURN void panic_finish(void)
{
    kprintf(
        "================================================================\n");
    vga_status_line(" KERNEL PANIC - system halted ",
                    vga_attr(VGA_WHITE, VGA_RED));

    /* Signal failure to an automated runner, then stop for good. */
    cpu_qemu_exit(0x11);
    cpu_halt_forever();
}

NORETURN void panic(const char *fmt, ...)
{
    va_list ap;
    u32 ebp;

    cli();

    if (panicking) {
        /* Nested panic: say so and stop immediately. */
        console_puts("\n[double panic - halting]\n");
        cpu_qemu_exit(0x12);
        cpu_halt_forever();
    }
    panicking = true;

    /* Stop every other processor before printing anything. A kernel that has
     * decided it cannot continue should not leave three other CPUs running in
     * the state that made it decide that - and more immediately, two
     * processors interleaving output through the same console would make the
     * one message that matters unreadable. */
    smp_halt_others();

    panic_banner();
    kprintf(" ");
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n\n");

    __asm__ volatile("movl %%ebp, %0" : "=r"(ebp));
    backtrace(ebp, 16);

    panic_finish();
}

NORETURN void panic_with_regs(const struct regs *r, const char *fmt, ...)
{
    va_list ap;

    cli();

    if (panicking) {
        console_puts("\n[double panic - halting]\n");
        cpu_qemu_exit(0x12);
        cpu_halt_forever();
    }
    panicking = true;

    /* Stop every other processor before printing anything. A kernel that has
     * decided it cannot continue should not leave three other CPUs running in
     * the state that made it decide that - and more immediately, two
     * processors interleaving output through the same console would make the
     * one message that matters unreadable. */
    smp_halt_others();

    panic_banner();
    kprintf(" ");
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n\n");

    regs_dump(r);
    kprintf("\n");
    backtrace(r->ebp, 16);

    panic_finish();
}
