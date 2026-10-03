/* StratumOS - bringing up the other processors.
 *
 * See kernel/smp.h for what is and is not claimed here. In short: every
 * processor runs kernel code, locks are real, processors can interrupt each
 * other - and the scheduler still runs only on the boot processor.
 *
 * The sequence, once ACPI has said how many processors there are:
 *
 *   1. copy the trampoline to a page below 1 MiB, because a processor out of
 *      reset starts in 16-bit real mode and cannot reach anything higher;
 *   2. build a page directory that identity-maps that page as well as the
 *      kernel, because enabling paging with the kernel's own directory would
 *      unmap the instruction after `mov cr0`;
 *   3. for each processor: give it a stack, point the trampoline at it, send
 *      INIT-SIPI-SIPI, and wait for it to announce itself;
 *   4. free the bootstrap directory once they are all up.
 *
 * Step 3 is serialised deliberately. The trampoline has one parameter block,
 * so two processors starting at once would race for the stack pointer in it -
 * and the failure would be two CPUs sharing a stack, which is not a bug
 * anybody wants to debug. Starting them one at a time costs about 10 ms each
 * and is what every kernel does.
 */
#define LOG_TAG "smp"

#include <arch/acpi.h>
#include <arch/apic.h>
#include <arch/cpu.h>
#include <arch/gdt.h>
#include <arch/idt.h>
#include <arch/io.h>
#include <arch/irq.h>

#include <kernel/kernel.h>
#include <kernel/layout.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/smp.h>
#include <kernel/spinlock.h>
#include <kernel/string.h>

#include <mm/vmm.h>

/* Where the trampoline is copied to. Must be page aligned and below 1 MiB,
 * because the startup message carries a page number in a byte. The whole
 * first mebibyte is reserved by the PMM, so nothing else can be using it. */
#define TRAMPOLINE_PHYS 0x8000
#define TRAMPOLINE_PAGE (TRAMPOLINE_PHYS >> 12)

#define AP_STACK_SIZE   (16 * KIB)
#define AP_STACK_SLOT   (PAGE_SIZE + AP_STACK_SIZE)

_Static_assert(SMP_MAX_CPUS *AP_STACK_SLOT <= CPUSTACK_REGION,
               "the per-CPU stack region cannot hold SMP_MAX_CPUS stacks");

/* From arch/x86/ap_boot.asm. The offsets are absolute symbols, so their
 * *addresses* are the values - which is why they are declared as arrays and
 * then cast, rather than as integers. */
extern const u8 ap_trampoline_start[];
extern const u8 ap_trampoline_end[];
extern const u8 ap_param_pagedir_off[];
extern const u8 ap_param_entry_off[];
extern const u8 ap_param_stack_off[];
extern const u8 ap_param_flag_off[];

static struct cpu cpus[SMP_MAX_CPUS];
static u32 cpu_count;    /* online  */
static u32 cpus_present; /* reported */
static bool smp_up;

static struct smp_stats stats;

void smp_get_stats(struct smp_stats *out)
{
    if (out)
        *out = stats;
}

bool smp_enabled(void)
{
    return smp_up && cpu_count > 1;
}

u32 smp_cpu_count(void)
{
    return cpu_count ? cpu_count : 1;
}

u32 smp_cpus_present(void)
{
    return cpus_present ? cpus_present : 1;
}

struct cpu *smp_this_cpu(void)
{
    return &cpus[smp_cpu_index()];
}

const struct cpu *smp_cpu(u32 index)
{
    if (index >= ARRAY_SIZE(cpus) || !cpus[index].present)
        return NULL;
    return &cpus[index];
}

/* ------------------------------------------------------------------------- */
/* Inter-processor interrupts                                                */
/* ------------------------------------------------------------------------- */

/* The address the current shootdown is for. Written by the sender before the
 * IPI goes out and read by every receiver, so it is `volatile` and the
 * handshake below is what orders it. */
static volatile vaddr_t shootdown_addr;
static volatile u32 shootdown_acks;

static void ping_handler(struct regs *r)
{
    UNUSED(r);

    smp_this_cpu()->ipi_ping++;
    apic_eoi();
}

static void tlb_handler(struct regs *r)
{
    UNUSED(r);

    invlpg(shootdown_addr);

    struct cpu *self = smp_this_cpu();

    self->ipi_tlb++;

    /* Acknowledge after the invalidation, not before: the sender waits on
     * this count precisely so that it knows every other processor has already
     * dropped the translation. An ack sent first would make the wait
     * meaningless. */
    __atomic_add_fetch(&shootdown_acks, 1, __ATOMIC_SEQ_CST);
    __atomic_add_fetch(&stats.shootdowns_served, 1, __ATOMIC_RELAXED);

    apic_eoi();
}

static void halt_handler(struct regs *r)
{
    UNUSED(r);

    /* No EOI and no return. The kernel has panicked; this processor's job is
     * to stop touching memory. */
    cpu_halt_forever();
}

void smp_ping_others(void)
{
    if (!smp_enabled())
        return;

    stats.pings_sent++;
    apic_broadcast_ipi(APIC_VECTOR_IPI_PING);
}

void smp_tlb_shootdown(vaddr_t va)
{
    /* Always invalidate locally, whether or not there is anyone to tell. A
     * caller should not have to ask whether SMP is up to get correct
     * behaviour on its own processor. */
    invlpg(va);

    if (!smp_enabled())
        return;

    /* One shootdown at a time. The address is passed through a single global,
     * so two concurrent senders would each invalidate the other's page and
     * neither would invalidate their own. */
    static spinlock_t shootdown_lock = SPINLOCK_INIT("tlb-shootdown");

    spin_lock(&shootdown_lock);

    shootdown_addr = va;
    __atomic_store_n(&shootdown_acks, 0, __ATOMIC_SEQ_CST);

    stats.shootdowns_sent++;
    apic_broadcast_ipi(APIC_VECTOR_IPI_TLB);

    /* Wait for every other processor to confirm. Bounded, because a processor
     * that is wedged with interrupts disabled would otherwise hang the sender
     * - and an unacknowledged shootdown is a correctness problem worth a loud
     * complaint rather than a silent continue. */
    u32 want = cpu_count - 1;

    for (u32 spin = 0; spin < 10000000; spin++) {
        if (__atomic_load_n(&shootdown_acks, __ATOMIC_SEQ_CST) >= want)
            break;
        cpu_relax();
    }

    if (__atomic_load_n(&shootdown_acks, __ATOMIC_SEQ_CST) < want)
        pr_err("TLB shootdown for %p: %u of %u processors acknowledged",
               (void *)va, shootdown_acks, want);

    spin_unlock(&shootdown_lock);
}

void smp_halt_others(void)
{
    if (!smp_up || cpu_count <= 1 || !apic_available())
        return;

    apic_broadcast_ipi(APIC_VECTOR_IPI_HALT);
}

/* ------------------------------------------------------------------------- */
/* The application processor's first C                                       */
/* ------------------------------------------------------------------------- */

/* Set by the BSP before each startup, read by the AP that it starts. Only one
 * processor is being started at a time, which is what makes a single slot
 * safe - see the file header. */
static volatile u32 starting_index;

NORETURN void ap_main(void)
{
    u32 index = starting_index;
    struct cpu *self = &cpus[index];

    /* Descriptor tables first. Until the IDT is loaded this processor cannot
     * take an exception, so a fault anywhere above would be a triple fault
     * with nothing to report it - which is why there is nothing above. */
    gdt_init_ap(index);
    idt_load();

    /* Off the bootstrap directory and onto the kernel's. Both have identical
     * kernel halves, so this changes nothing about what is reachable; it just
     * stops this processor holding a reference to a directory the boot
     * processor is about to free. */
    vmm_switch_address_space(vmm_kernel_pd_phys());

    apic_init_ap();

    /* Only now is the processor's own APIC id meaningful, and it must match
     * what ACPI said - the whole index-to-APIC-id mapping depends on it. */
    u32 reported = apic_id();

    if (reported != self->apic_id)
        panic("cpu %u came up with APIC id %u; the MADT said %u", index,
              reported, self->apic_id);

    tss_set_kernel_stack(self->stack_top);

    /* Published last, with a release so that everything above is visible to
     * the boot processor before it sees `online`. Without the barrier the
     * compiler is entitled to hoist this store, and the BSP would proceed to
     * free the bootstrap directory while this processor was still on it. */
    __atomic_store_n(&self->online, true, __ATOMIC_RELEASE);

    /* Interrupts on. From here this processor services IPIs and whatever the
     * APIC delivers to it. It does not run the scheduler: see kernel/smp.h. */
    sti();

    for (;;) {
        self->idle_loops++;
        hlt();
    }
}

/* ------------------------------------------------------------------------- */
/* Bring-up                                                                  */
/* ------------------------------------------------------------------------- */

static void install_trampoline(paddr_t bootstrap_pd)
{
    size_t size = (size_t)(ap_trampoline_end - ap_trampoline_start);
    u8 *dst = phys_to_virt(TRAMPOLINE_PHYS);

    if (size > PAGE_SIZE)
        panic("the AP trampoline is %u bytes; it has to fit in a page",
              (unsigned)size);

    memcpy(dst, ap_trampoline_start, size);

    /* Patch in the two parameters that are the same for every processor. The
     * stack is per-processor and is written just before each startup. */
    *(volatile u32 *)(dst + (u32)ap_param_pagedir_off) = bootstrap_pd;
    *(volatile u32 *)(dst + (u32)ap_param_entry_off) = (u32)&ap_main;

    pr_debug("trampoline: %u bytes at phys %p, entry %p, bootstrap pd %p",
             (unsigned)size, (void *)TRAMPOLINE_PHYS, (void *)&ap_main,
             (void *)bootstrap_pd);
}

/* Map a per-CPU stack with a guard page below it, exactly as the task stacks
 * are. Returns the top, or 0. */
static u32 map_cpu_stack(u32 index)
{
    vaddr_t base = CPUSTACK_BASE + index * AP_STACK_SLOT + PAGE_SIZE;

    for (u32 off = 0; off < AP_STACK_SIZE; off += PAGE_SIZE) {
        if (!vmm_alloc_at(base + off, PTE_PRESENT | PTE_WRITE)) {
            pr_err("cannot map a stack for cpu %u", index);
            while (off > 0) {
                off -= PAGE_SIZE;
                vmm_unmap(base + off);
            }
            return 0;
        }
    }

    memset((void *)base, 0, AP_STACK_SIZE);

    return base + AP_STACK_SIZE;
}

/* Wait for a processor to announce itself. The timeout is generous because
 * the startup sequence itself spends 10 ms in INIT, and because an emulated
 * processor's first few thousand instructions are slow. */
static bool wait_for_online(struct cpu *target, volatile u32 *flag)
{
    for (u32 ms = 0; ms < 500; ms++) {
        if (__atomic_load_n(&target->online, __ATOMIC_ACQUIRE))
            return true;

        /* No scheduler yet, and interrupts may not be the thing to wait on,
         * so this is a calibrated-enough spin: 1000 reads of the unused POST
         * port is on the order of a millisecond. */
        for (u32 i = 0; i < 1000; i++)
            (void)inb(0x80);
    }

    if (*flag)
        pr_err("cpu %u (APIC id %u) reached 32-bit mode but never reached C",
               target->index, target->apic_id);
    else
        pr_err("cpu %u (APIC id %u) never started: the trampoline's flag is "
               "still clear, so it did not get past real mode",
               target->index, target->apic_id);

    return false;
}

void smp_init(void)
{
    const struct acpi_info *acpi = acpi_get_info();

    memset(cpus, 0, sizeof(cpus));
    memset(&stats, 0, sizeof(stats));

    /* The boot processor exists whatever ACPI says. Registering it first, and
     * unconditionally, means every path below has a valid cpu 0 - including
     * the paths that give up. */
    u32 bsp_apic = apic_available() ? apic_id() : 0;

    cpus[0].present = true;
    cpus[0].online = true;
    cpus[0].is_bsp = true;
    cpus[0].index = 0;
    cpus[0].apic_id = (u8)bsp_apic;
    cpus[0].stack_top = 0; /* it is on the boot stack, which it did not map */
    cpu_count = 1;
    cpus_present = 1;
    smp_up = true;

    isr_install_handler(APIC_VECTOR_IPI_PING, ping_handler);
    isr_install_handler(APIC_VECTOR_IPI_TLB, tlb_handler);
    isr_install_handler(APIC_VECTOR_IPI_HALT, halt_handler);

    if (!apic_available()) {
        pr_info("no local APIC; running on the boot processor alone");
        return;
    }

    if (!acpi->available || !acpi->madt_found) {
        pr_info("no MADT; there is no way to find other processors, so "
                "running on the boot processor alone");
        return;
    }

    cpus_present = acpi->cpus_reported;

    if (acpi->cpus_reported > ARRAY_SIZE(cpus))
        pr_warn("the MADT reports %u processors; this kernel is built for %u",
                acpi->cpus_reported, (unsigned)ARRAY_SIZE(cpus));

    /* Register every processor ACPI described, before starting any of them.
     * An AP needs its own `struct cpu` to exist and its APIC id to be in the
     * index map *before* it runs, because the first thing it does is look
     * itself up. */
    for (u32 i = 0; i < acpi->cpu_count; i++) {
        const struct acpi_cpu *acpi_cpu = &acpi->cpus[i];

        if (acpi_cpu->apic_id == bsp_apic)
            continue; /* already registered as cpu 0 */

        if (!acpi_cpu->enabled) {
            pr_info("APIC id %u is %s; not starting it", acpi_cpu->apic_id,
                    acpi_cpu->online_capable ? "online-capable but disabled"
                                             : "disabled by firmware");
            continue;
        }

        if (cpu_count >= ARRAY_SIZE(cpus))
            break;

        u32 index = cpu_count++;
        struct cpu *cpu = &cpus[index];

        cpu->present = true;
        cpu->online = false;
        cpu->index = index;
        cpu->apic_id = acpi_cpu->apic_id;
    }

    if (cpu_count == 1) {
        pr_info("one processor: the boot processor is the only one the "
                "firmware reports as usable");
        return;
    }

    paddr_t bootstrap_pd = vmm_create_bootstrap_pd();

    if (!bootstrap_pd) {
        pr_err("no bootstrap page directory; cannot start any processor");
        cpu_count = 1;
        return;
    }

    install_trampoline(bootstrap_pd);

    u8 *tramp = phys_to_virt(TRAMPOLINE_PHYS);
    volatile u32 *flag = (volatile u32 *)(tramp + (u32)ap_param_flag_off);
    volatile u32 *stack = (volatile u32 *)(tramp + (u32)ap_param_stack_off);
    u32 online = 1;

    for (u32 index = 1; index < cpu_count; index++) {
        struct cpu *cpu = &cpus[index];

        cpu->stack_top = map_cpu_stack(index);

        if (!cpu->stack_top) {
            stats.start_failures++;
            continue;
        }

        /* One processor at a time: the parameter block has one stack slot.
         * Written before the startup message, read after it. */
        *flag = 0;
        *stack = cpu->stack_top;
        starting_index = index;

        pr_debug("starting cpu %u (APIC id %u), stack top %p", index,
                 cpu->apic_id, (void *)cpu->stack_top);

        apic_start_ap(cpu->apic_id, TRAMPOLINE_PAGE);

        if (wait_for_online(cpu, flag)) {
            online++;
            pr_info("cpu %u online (APIC id %u)", index, cpu->apic_id);
        } else {
            stats.start_failures++;
            cpu->present = false;
        }
    }

    /* Only the processors that answered are counted. A `struct cpu` left
     * marked absent is one whose index must never be handed out again, which
     * is why the gaps are not compacted away: the index is baked into a TSS
     * descriptor, and the processor reads it back out of its own task
     * register. */
    cpu_count = online;

    /* Every processor is on the kernel's directory now - each one switched as
     * its first action after loading descriptor tables - so the bootstrap
     * directory has no remaining user. Freeing it is also what guarantees no
     * low mapping of any kind survives into normal operation. */
    vmm_destroy_bootstrap_pd(bootstrap_pd);

    pr_info("%u of %u processor(s) online%s", cpu_count, cpus_present,
            stats.start_failures ? " (some failed to start)" : "");
}
