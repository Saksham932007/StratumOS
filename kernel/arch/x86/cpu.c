/* StratumOS - processor identification via CPUID, and machine control.
 *
 * CPUID is not guaranteed to exist on a 386/486, which is why its presence is
 * probed by trying to flip the ID bit (bit 21) in EFLAGS: on a CPU without
 * CPUID the bit is hardwired and the write has no effect. Checking rather than
 * assuming is cheap and it is the difference between a kernel that boots on a
 * 486 and one that executes an invalid opcode.
 */
#define LOG_TAG "cpu"

#include <arch/cpu.h>
#include <arch/io.h>

#include <kernel/log.h>
#include <kernel/string.h>

static struct cpu_info info;

/* Leaf 1 EDX feature bits */
#define CPUID_FPU        (1u << 0)
#define CPUID_TSC        (1u << 4)
#define CPUID_MSR        (1u << 5)
#define CPUID_PAE        (1u << 6)
#define CPUID_PSE        (1u << 3)
#define CPUID_APIC       (1u << 9)
#define CPUID_PGE        (1u << 13)
#define CPUID_MMX        (1u << 23)
#define CPUID_SSE        (1u << 25)
#define CPUID_SSE2       (1u << 26)
/* Leaf 1 ECX feature bits */
#define CPUID_SSE3       (1u << 0)
#define CPUID_HYPERVISOR (1u << 31)

static bool cpuid_supported(void)
{
    u32 before, after;

    __asm__ volatile(
        "pushfl\n\t"
        "pushfl\n\t"
        "popl %0\n\t" /* original EFLAGS                  */
        "movl %0, %1\n\t"
        "xorl $0x200000, %1\n\t" /* toggle the ID bit               */
        "pushl %1\n\t"
        "popfl\n\t" /* try to write it back             */
        "pushfl\n\t"
        "popl %1\n\t" /* read what actually stuck         */
        "popfl"       /* restore                          */
        : "=&r"(before), "=&r"(after)
        :
        : "cc");

    return ((before ^ after) & 0x200000) != 0;
}

void cpu_detect(void)
{
    u32 a, b, c, d;

    memset(&info, 0, sizeof(info));
    strlcpy(info.vendor, "unknown", sizeof(info.vendor));
    strlcpy(info.brand, "unknown", sizeof(info.brand));

    if (!cpuid_supported()) {
        info.has_cpuid = false;
        pr_warn("CPUID not available - assuming a bare i386");
        return;
    }
    info.has_cpuid = true;

    /* Leaf 0: maximum leaf, and the vendor string spread across EBX/EDX/ECX
     * in that deliberately awkward order. */
    cpuid_raw(0, 0, &a, &b, &c, &d);
    info.max_leaf = a;
    memcpy(info.vendor + 0, &b, 4);
    memcpy(info.vendor + 4, &d, 4);
    memcpy(info.vendor + 8, &c, 4);
    info.vendor[12] = '\0';

    if (info.max_leaf >= 1) {
        cpuid_raw(1, 0, &a, &b, &c, &d);

        info.stepping = (u8)(a & 0xF);
        info.model = (u8)((a >> 4) & 0xF);
        info.family = (u8)((a >> 8) & 0xF);

        /* Extended family/model encoding, used once family reaches 0xF/6. */
        if (info.family == 0xF)
            info.family = (u8)(info.family + ((a >> 20) & 0xFF));
        if (info.family == 0xF || info.family == 6)
            info.model = (u8)(info.model + (((a >> 16) & 0xF) << 4));

        info.features_edx = d;
        info.features_ecx = c;
        info.cache_line_size = ((b >> 8) & 0xFF) * 8;

        info.has_fpu = (d & CPUID_FPU) != 0;
        info.has_tsc = (d & CPUID_TSC) != 0;
        info.has_msr = (d & CPUID_MSR) != 0;
        info.has_pae = (d & CPUID_PAE) != 0;
        info.has_pse = (d & CPUID_PSE) != 0;
        info.has_pge = (d & CPUID_PGE) != 0;
        info.has_apic = (d & CPUID_APIC) != 0;
        info.has_mmx = (d & CPUID_MMX) != 0;
        info.has_sse = (d & CPUID_SSE) != 0;
        info.has_sse2 = (d & CPUID_SSE2) != 0;
        info.has_sse3 = (c & CPUID_SSE3) != 0;
        info.has_hypervisor = (c & CPUID_HYPERVISOR) != 0;
    }

    /* Extended leaves: the human-readable brand string, if the CPU has one. */
    cpuid_raw(0x80000000u, 0, &a, &b, &c, &d);
    info.max_ext_leaf = a;

    if (info.max_ext_leaf >= 0x80000004u) {
        u32 *dst = (u32 *)info.brand;
        for (u32 leaf = 0x80000002u; leaf <= 0x80000004u; leaf++) {
            cpuid_raw(leaf, 0, &a, &b, &c, &d);
            *dst++ = a;
            *dst++ = b;
            *dst++ = c;
            *dst++ = d;
        }
        info.brand[48] = '\0';
    }
}

const struct cpu_info *cpu_get_info(void)
{
    return &info;
}

void cpu_print_info(void)
{
    const struct cpu_info *i = &info;

    kprintf("Vendor      : %s\n", i->vendor);
    kprintf("Brand       : %s\n", i->brand);
    kprintf("Family/Model: %u/%u stepping %u\n", i->family, i->model,
            i->stepping);
    kprintf("CPUID leaves: max %u, max extended %08x\n", i->max_leaf,
            i->max_ext_leaf);
    if (i->cache_line_size)
        kprintf("Cache line  : %u bytes\n", i->cache_line_size);

    kprintf("Features    :");
    if (i->has_fpu)
        kprintf(" fpu");
    if (i->has_tsc)
        kprintf(" tsc");
    if (i->has_msr)
        kprintf(" msr");
    if (i->has_pse)
        kprintf(" pse");
    if (i->has_pae)
        kprintf(" pae");
    if (i->has_pge)
        kprintf(" pge");
    if (i->has_apic)
        kprintf(" apic");
    if (i->has_mmx)
        kprintf(" mmx");
    if (i->has_sse)
        kprintf(" sse");
    if (i->has_sse2)
        kprintf(" sse2");
    if (i->has_sse3)
        kprintf(" sse3");
    kprintf("\n");

    kprintf("Running on  : %s\n",
            i->has_hypervisor ? "a hypervisor (CPUID hypervisor bit set)"
                              : "bare metal (no hypervisor bit)");
    kprintf("Mode        : 32-bit protected mode, ring 0\n");
}

void cpu_qemu_exit(u8 code)
{
    /* `-device isa-debug-exit,iobase=0xf4,iosize=0x04` makes QEMU terminate
     * with status (code << 1) | 1. Writing to the port is inert on any machine
     * that does not have the device, so this is safe to call unconditionally -
     * which is what lets the same kernel binary be both interactive and
     * CI-testable. */
    outl(0xF4, code);
}

NORETURN void cpu_halt_forever(void)
{
    for (;;) {
        cli();
        hlt();
    }
}

NORETURN void cpu_reset(void)
{
    /* Politest option first: ask the 8042 to pulse the CPU's reset line. */
    for (int spin = 0; spin < 10000; spin++) {
        if ((inb(0x64) & 0x02) == 0)
            break;
        io_wait();
    }
    outb(0x64, 0xFE);

    /* If that did nothing, force a triple fault: with a zero-length IDT, the
     * next interrupt cannot be delivered, nor can the resulting #GP, nor the
     * #DF after that - and the CPU resets. Crude but universal. */
    struct {
        u16 limit;
        u32 base;
    } PACKED null_idt = {0, 0};

    __asm__ volatile("lidt %0" : : "m"(null_idt));
    __asm__ volatile("int $0x03");

    cpu_halt_forever();
}
