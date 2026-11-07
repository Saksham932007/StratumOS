/* StratumOS - SMEP, SMAP and the rest of the hardening switches.
 *
 * See arch/harden.h for what each one does and why SMAP is the interesting
 * one. This file is only the detection and the two instructions.
 */
#define LOG_TAG "harden"

#include <arch/cpu.h>
#include <arch/harden.h>
#include <arch/io.h>

#include <kernel/kernel.h>
#include <kernel/log.h>

/* CPUID leaf 7, subleaf 0, EBX */
#define CPUID7_SMEP (1u << 7)
#define CPUID7_SMAP (1u << 20)

static struct harden_state state;

const struct harden_state *harden_get_state(void)
{
    return &state;
}

void harden_note_text_ro(void)
{
    state.kernel_text_ro = true;
}

void user_access_begin(void)
{
    if (!state.smap_enabled)
        return;

    /* stac */
    __asm__ volatile(".byte 0x0f, 0x01, 0xcb" ::: "cc");
    state.user_access_windows++;
}

void user_access_end(void)
{
    if (!state.smap_enabled)
        return;

    /* clac */
    __asm__ volatile(".byte 0x0f, 0x01, 0xca" ::: "cc");
}

/* Set the control-register bits that make this processor enforce what the
 * page tables say. Returns the CR4 value it ended up with.
 *
 * CR0 and CR4 are *per processor*. The boot processor setting SMEP and SMAP
 * protects the boot processor; an application processor that never sets them
 * will happily execute a user page from ring 0, on a kernel that reports the
 * feature as enabled. That was true in this kernel for as long as the
 * application processors only idled - and stopped being merely untidy the
 * moment they started running tasks, which is how the test suite found it:
 * the hardening suite reads CR4 on whichever processor it happens to be
 * running on, and started failing on three out of four. */
static u32 harden_apply(bool smep, bool smap)
{
    u32 cr4 = read_cr4();

    if (smep)
        cr4 |= CR4_SMEP;
    if (smap)
        cr4 |= CR4_SMAP;

    if (smep || smap)
        write_cr4(cr4);

    /* Read CR4 back rather than assuming the write took. A hypervisor may
     * advertise a feature in CPUID and refuse the CR4 bit, and a kernel that
     * then believed SMAP was on would skip the stac/clac pairs and fault on
     * its first legitimate user access. */
    return read_cr4();
}

void harden_init(void)
{
    const struct cpu_info *cpu = cpu_get_info();

    /* These two live in the structural hardening the kernel already does;
     * reported here so that one command answers "what is switched on". */
    state.wp_enabled = (read_cr0() & CR0_WP) != 0;
    state.stack_guard_pages = true;
    state.stack_canaries = true;

    if (!cpu->has_cpuid || cpu->max_leaf < 7) {
        pr_info("SMEP/SMAP unavailable: CPUID leaf 7 is not implemented");
        return;
    }

    u32 a, b, c, d;

    cpuid_raw(7, 0, &a, &b, &c, &d);

    state.smep_available = (b & CPUID7_SMEP) != 0;
    state.smap_available = (b & CPUID7_SMAP) != 0;

    u32 cr4 = harden_apply(state.smep_available, state.smap_available);

    state.smep_enabled = (cr4 & CR4_SMEP) != 0;
    state.smap_enabled = (cr4 & CR4_SMAP) != 0;

    pr_info("SMEP %s, SMAP %s",
            state.smep_enabled     ? "enabled"
            : state.smep_available ? "available but refused by CR4"
                                   : "not supported by this CPU",
            state.smap_enabled     ? "enabled"
            : state.smap_available ? "available but refused by CR4"
                                   : "not supported by this CPU");
}

bool harden_init_ap(void)
{
    /* No CPUID here: the boot processor already asked, and a processor whose
     * answer differed would be a machine this kernel cannot support anyway.
     * What matters is that the *bits* get set on this processor. */
    u32 cr4 = harden_apply(state.smep_available, state.smap_available);

    bool ok = (!state.smep_enabled || (cr4 & CR4_SMEP)) &&
              (!state.smap_enabled || (cr4 & CR4_SMAP));

    /* CR0.WP too. boot.asm sets it for the boot processor and ap_boot.asm
     * for an application processor, so this is a check rather than a change -
     * but a check worth making, because write protection that is on for one
     * processor and off for another is worse than it being off everywhere. */
    if (!(read_cr0() & CR0_WP))
        ok = false;

    return ok;
}
