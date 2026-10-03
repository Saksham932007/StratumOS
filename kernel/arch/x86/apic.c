/* StratumOS - the local APIC.
 *
 * See arch/apic.h for why this replaces the 8259 pair for anything involving
 * more than one processor.
 */
#define LOG_TAG "apic"

#include <arch/acpi.h>
#include <arch/apic.h>
#include <arch/cpu.h>
#include <arch/idt.h>
#include <arch/io.h>
#include <arch/irq.h>

#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/panic.h>

#include <mm/vmm.h>

/* CPUID leaf 1, EDX bit 9. */
#define CPUID_APIC_BIT   (1u << 9)

/* The APIC base MSR. Bit 11 is the hardware enable, which firmware normally
 * leaves set; bits 12-31 are the physical page. */
#define MSR_APIC_BASE    0x1B
#define APIC_BASE_ENABLE (1u << 11)
#define APIC_BASE_BSP    (1u << 8)

static volatile u8 *apic_base;
static struct apic_stats stats;

void apic_get_stats(struct apic_stats *out)
{
    if (out)
        *out = stats;
}

bool apic_available(void)
{
    return apic_base != NULL;
}

void apic_write(u32 reg, u32 value)
{
    /* A plain assignment through a volatile pointer, which on x86 is a single
     * 32-bit store - the only access width the APIC accepts. */
    *(volatile u32 *)(apic_base + reg) = value;
}

u32 apic_read(u32 reg)
{
    return *(volatile u32 *)(apic_base + reg);
}

u32 apic_id(void)
{
    if (!apic_base)
        return 0;

    /* The id is in the top byte. On an xAPIC it is 8 bits; x2APIC widens it
     * to 32, which this kernel does not enable. */
    return apic_read(APIC_ID) >> 24;
}

u32 apic_version(void)
{
    return apic_base ? apic_read(APIC_VERSION) : 0;
}

void apic_eoi(void)
{
    if (!apic_base)
        return;

    stats.eois++;
    apic_write(APIC_EOI, 0);
}

/* ------------------------------------------------------------------------- */

static void spurious_handler(struct regs *r)
{
    UNUSED(r);

    /* A spurious APIC interrupt needs no EOI - the APIC did not set an
     * in-service bit for it. Sending one would acknowledge whatever genuinely
     * is in service and lose that interrupt instead. */
    stats.spurious++;
}

static void error_handler(struct regs *r)
{
    UNUSED(r);

    /* The error status register latches, and needs a write before the read to
     * push the current errors into it. */
    apic_write(APIC_ESR, 0);
    u32 esr = apic_read(APIC_ESR);

    stats.errors++;
    pr_err("local APIC error, ESR %08x", esr);
    apic_eoi();
}

/* Bring this CPU's local APIC into a known state. Runs on every processor,
 * because each one has its own.
 *
 * `is_bsp` decides what happens to LINT0, and it is the one parameter here
 * that is not cosmetic - see the comment on it below. */
static void configure_local(bool is_bsp)
{
    /* Accept interrupts of every priority. Firmware sometimes leaves the task
     * priority high enough to block them all, and the symptom is a CPU that
     * comes up and then never takes an interrupt. */
    apic_write(APIC_TPR, 0);

    /* Flat logical destination mode, with this CPU as its own logical id.
     * Physical addressing is what the IPI paths below use, so this exists
     * only so that the registers hold something defined. */
    apic_write(APIC_DFR, 0xFFFFFFFF);
    apic_write(APIC_LDR, (apic_read(APIC_LDR) & 0x00FFFFFF) | (1u << 24));

    /* Mask everything in the local vector table that this kernel does not
     * handle. An unmasked LVT entry left over from firmware delivers an
     * interrupt on a vector nothing is installed for. */
    apic_write(APIC_LVT_TIMER, APIC_LVT_MASKED);
    apic_write(APIC_LVT_THERMAL, APIC_LVT_MASKED);
    apic_write(APIC_LVT_PERF, APIC_LVT_MASKED);
    apic_write(APIC_LVT_ERROR, APIC_VECTOR_ERROR);

    /* LINT0 and LINT1 are not like the rest, and masking them the way the
     * first version of this function did stopped the kernel's timer dead.
     *
     * Before the local APIC is enabled, the 8259 pair drives the processor's
     * INTR pin directly. Enabling the local APIC puts that pin behind LINT0,
     * in the arrangement the firmware set up and the specification calls
     * virtual wire mode - so a masked LINT0 means no 8259 interrupt reaches
     * the CPU at all. This kernel's timer, keyboard and serial input all
     * arrive through the 8259s, so the symptom was a kernel that booted
     * perfectly and then never advanced its clock: every log line timestamped
     * 0.040, and the autotest hanging before its first suite.
     *
     * So the boot processor takes ExtINT on LINT0 - "the vector comes from
     * the external controller, not from this register" - and NMI on LINT1,
     * which is what it is wired to on every PC.
     *
     * Application processors mask both. The 8259 has one output and it is
     * already going to the boot processor; a second CPU accepting ExtINT
     * would race it for the same interrupt and acknowledge a controller it
     * was not talking to. */
    if (is_bsp) {
        apic_write(APIC_LVT_LINT0, APIC_LVT_EXTINT);
        apic_write(APIC_LVT_LINT1, APIC_LVT_NMI);
    } else {
        apic_write(APIC_LVT_LINT0, APIC_LVT_MASKED);
        apic_write(APIC_LVT_LINT1, APIC_LVT_MASKED);
    }

    /* Clear any error the firmware left latched before enabling delivery. */
    apic_write(APIC_ESR, 0);
    (void)apic_read(APIC_ESR);

    /* The software enable, and the vector a spurious interrupt arrives on.
     * This write is what makes the APIC start delivering. */
    apic_write(APIC_SPURIOUS, APIC_VECTOR_SPURIOUS | APIC_SW_ENABLE);
}

bool apic_init_bsp(void)
{
    const struct cpu_info *cpu = cpu_get_info();
    const struct acpi_info *acpi = acpi_get_info();

    if (!cpu->has_apic) {
        pr_info("this CPU reports no local APIC (CPUID leaf 1, EDX bit 9)");
        return false;
    }

    paddr_t base = 0xFEE00000u;

    if (acpi->available && acpi->madt_found && acpi->local_apic_phys)
        base = (paddr_t)acpi->local_apic_phys;
    else
        pr_warn("no MADT; assuming the local APIC is at the architectural "
                "default %p",
                (void *)base);

    /* Uncached, and it has to be: see arch/apic.h. */
    apic_base = vmm_map_mmio(base, PAGE_SIZE, true);

    if (!apic_base) {
        pr_err("cannot map the local APIC at %p", (void *)base);
        return false;
    }

    isr_install_handler(APIC_VECTOR_SPURIOUS, spurious_handler);
    isr_install_handler(APIC_VECTOR_ERROR, error_handler);

    configure_local(true);

    u32 version = apic_read(APIC_VERSION);

    pr_info("local APIC at %p -> %p, id %u, version %02x, %u LVT entries",
            (void *)base, (void *)apic_base, apic_id(), version & 0xFF,
            ((version >> 16) & 0xFF) + 1);

    /* The 8259s are still wired up and still unmasked for the lines this
     * kernel uses. They stay that way: the timer and the keyboard arrive
     * through them, and moving those to the I/O APIC is a separate change.
     * What matters here is that nothing has been broken by enabling the local
     * APIC, because the two controllers deliver on different vectors. */
    if (acpi->pcat_compat)
        pr_debug("the MADT reports 8259 PICs; leaving them as they are");

    return true;
}

void apic_init_ap(void)
{
    /* The registers are already mapped - the mapping is in the kernel half,
     * which every address space shares - so an application processor only has
     * to configure its own APIC. */
    configure_local(false);
}

/* ------------------------------------------------------------------------- */

/* The interrupt command register holds one IPI at a time, and bit 12 stays
 * set while delivery is in progress. Writing a second command before the
 * first is accepted loses it.
 *
 * Bounded, because a CPU that never accepts the IPI would otherwise hang the
 * sender - and the sender is often the only CPU that could report it. */
static bool wait_for_delivery(void)
{
    for (u32 spin = 0; spin < 1000000; spin++)
        if (!(apic_read(APIC_ICR_LOW) & APIC_ICR_PENDING))
            return true;

    pr_err("an IPI was never accepted; the command register is still busy");
    return false;
}

static void send_raw(u32 high, u32 low)
{
    if (!apic_base)
        return;

    /* High first: writing the low half is what sends the command, so the
     * destination has to already be there. */
    apic_write(APIC_ICR_HIGH, high);
    apic_write(APIC_ICR_LOW, low);

    stats.ipis_sent++;
    (void)wait_for_delivery();
}

void apic_send_ipi(u8 target, u32 vector)
{
    send_raw((u32)target << 24, vector | APIC_ICR_FIXED | APIC_ICR_PHYSICAL |
                                    APIC_ICR_EDGE | APIC_ICR_ASSERT);
}

void apic_broadcast_ipi(u32 vector)
{
    send_raw(0, vector | APIC_ICR_FIXED | APIC_ICR_EDGE | APIC_ICR_ASSERT |
                    APIC_ICR_ALL_BUT_ME);
}

/* ------------------------------------------------------------------------- */

/* Spin for roughly `us` microseconds.
 *
 * The AP startup sequence has mandatory delays - 10 ms after INIT, 200 us
 * between the two SIPIs - and they happen before the APs exist, so there is
 * nothing to sleep on and the scheduler is not running yet. Port 0x80 is the
 * unused POST diagnostic register; reading it is the traditional way to
 * consume about a microsecond of bus time without needing a calibrated
 * timer. */
static void stall_us(u32 us)
{
    for (u32 i = 0; i < us; i++)
        (void)inb(0x80);
}

void apic_start_ap(u8 target, u8 vector)
{
    if (!apic_base)
        return;

    /* The universal startup algorithm, from the MultiProcessor Specification
     * and Intel's manual. Every step matters:
     *
     *   1. assert INIT, which resets the target processor
     *   2. deassert it
     *   3. wait 10 ms for the reset to complete
     *   4. send STARTUP with the trampoline's page number
     *   5. wait 200 us
     *   6. send STARTUP again
     *
     * The second STARTUP is not belt-and-braces: the specification requires
     * it, because the first one can be lost if the processor was still
     * finishing its reset. A processor that has already started ignores the
     * duplicate, so sending it twice is always safe and sometimes necessary.
     */
    send_raw((u32)target << 24, APIC_ICR_INIT | APIC_ICR_PHYSICAL |
                                    APIC_ICR_LEVEL | APIC_ICR_ASSERT);

    send_raw((u32)target << 24, APIC_ICR_INIT | APIC_ICR_PHYSICAL |
                                    APIC_ICR_LEVEL | APIC_ICR_DEASSERT);

    stall_us(10000);

    for (int attempt = 0; attempt < 2; attempt++) {
        /* Clear the error register between attempts, so that a failure on the
         * second one is reported rather than attributed to the first. */
        apic_write(APIC_ESR, 0);

        send_raw((u32)target << 24, APIC_ICR_STARTUP | APIC_ICR_PHYSICAL |
                                        APIC_ICR_EDGE | APIC_ICR_ASSERT |
                                        vector);

        stall_us(200);
    }
}
