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

/* CR0 */
#define CR0_WP      (1u << 16)

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

    u32 cr4 = read_cr4();

    if (state.smep_available)
        cr4 |= CR4_SMEP;
    if (state.smap_available)
        cr4 |= CR4_SMAP;

    if (state.smep_available || state.smap_available) {
        write_cr4(cr4);

        /* Read CR4 back rather than assuming the write took. A hypervisor may
         * advertise a feature in CPUID and refuse the CR4 bit, and a kernel
         * that then believed SMAP was on would skip the stac/clac pairs and
         * fault on its first legitimate user access. */
        cr4 = read_cr4();
        state.smep_enabled = (cr4 & CR4_SMEP) != 0;
        state.smap_enabled = (cr4 & CR4_SMAP) != 0;
    }

    pr_info("SMEP %s, SMAP %s",
            state.smep_enabled     ? "enabled"
            : state.smep_available ? "available but refused by CR4"
                                   : "not supported by this CPU",
            state.smap_enabled     ? "enabled"
            : state.smap_available ? "available but refused by CR4"
                                   : "not supported by this CPU");
}
