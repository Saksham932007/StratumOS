/* StratumOS - driving the boot processor into 64-bit long mode and back.
 *
 * The mechanics are in longmode.asm and the boundary of the claim is in
 * arch/longmode.h. This file is the part that has to be got right around
 * them: building a four-level page table from a kernel that has only ever
 * had two, and arranging for the trampoline to be reachable under all four
 * paging regimes the transition passes through.
 */
#define LOG_TAG "lm"

#include <arch/cpu.h>
#include <arch/io.h>
#include <arch/longmode.h>

#include <kernel/layout.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/string.h>

#include <mm/pmm.h>
#include <mm/vmm.h>

/* Where the trampoline runs. Must match LM_TRAMPOLINE_PHYS in longmode.asm,
 * and must be below 1 MiB - which the PMM reserves in its entirety, so
 * nothing else will ever be handed this page. 0x8000 is the application
 * processor trampoline from the SMP work; this is the next page up. */
#define LM_TRAMPOLINE_PHYS  0x9000u

/* The trampoline's own stack grows down from the top of that page, so the
 * code and the block have to leave room. 608 bytes at the time of writing;
 * the check below is what makes that a fact rather than a hope. */
#define LM_TRAMPOLINE_MAX   2048u

/* --- the four-level page table ---------------------------------------------
 *
 * Long mode's walk is PML4 -> PDPT -> PD -> PT, with any level above the
 * last able to stop early and map a large page. Using 2 MiB pages at the PD
 * means three tables rather than four and 512 entries rather than 262144:
 *
 *   PML4[0]   -> PDPT
 *   PDPT[0]   -> PD
 *   PD[0..511] -> 2 MiB each, identity, = the first gigabyte
 *
 * A gigabyte is far more than the trampoline needs. It costs one page and it
 * means any physical address this machine can have is readable from 64-bit
 * mode, which is what makes the pointer probe below worth doing.
 *
 * The entries are 64 bits wide - that is the whole difference from the
 * kernel's 32-bit tables, and the reason CR4.PAE has to be set first: PAE
 * introduced this entry format, and long mode is PAE with a fourth level.
 */
#define PTE64_PRESENT       0x001ull
#define PTE64_WRITE         0x002ull
#define PTE64_PS            0x080ull /* at the PD, "this is a 2 MiB page"        */

#define LM_IDENTITY_ENTRIES 512u /* 512 x 2 MiB = 1 GiB                   */
#define LM_IDENTITY_MIB     (LM_IDENTITY_ENTRIES * 2u)

/* Built once and reused: a transition is cheap but allocating four frames
 * and an MMIO window for every run would not be, and `longmode` is a command
 * someone will run twice. */
static struct {
    bool built;
    paddr_t pml4, pdpt, pd, probe;
    volatile u64 *pml4_map, *pdpt_map, *pd_map, *probe_map;
} tables;

static struct longmode_result last;

/* The trampoline, assembled into the kernel image but executed from a copy at
 * LM_TRAMPOLINE_PHYS. See longmode.asm for why it cannot run where it is
 * linked. */
extern const u8 lm_trampoline_start[];
extern const u8 lm_trampoline_end[];
extern const u8 lm_enter_long_mode_off[];
extern const u8 lm_off_flags[];
extern const u8 lm_off_cs[];
extern const u8 lm_off_wide[];
extern const u8 lm_off_crossed[];
extern const u8 lm_off_r15[];
extern const u8 lm_off_rip[];
extern const u8 lm_off_efer[];
extern const u8 lm_off_cr4[];
extern const u8 lm_off_probe_value[];
extern const u8 lm_off_probe_phys[];
extern const u8 lm_off_pml4[];
extern const u8 lm_off_return_cr3[];

/* Offsets arrive from the assembler as absolute symbols, which is how
 * kernel/core/smp.c reads the AP trampoline's parameter block too: one
 * definition of the layout, in the file that depends on it most. */
#define LM_OFF(sym) ((u32)(unsigned long)(sym))

bool longmode_supported(void)
{
    u32 a, b, c, d;

    /* Leaf 0x80000001 only exists if leaf 0x80000000 says so, and on a
     * processor without extended leaves at all CPUID returns the highest
     * *basic* leaf instead - which would be read as a yes. */
    cpuid_raw(0x80000000u, 0, &a, &b, &c, &d);
    if (a < 0x80000001u)
        return false;

    cpuid_raw(0x80000001u, 0, &a, &b, &c, &d);
    return (d & (1u << 29)) != 0; /* EDX.LM */
}

bool longmode_available(const char **why)
{
#define NO(msg)           \
    do {                  \
        if (why)          \
            *why = (msg); \
        return false;     \
    } while (0)

    const struct cpu_info *cpu = cpu_get_info();

    if (!cpu->has_cpuid)
        NO("no CPUID, so no way to ask");
    if (!longmode_supported())
        NO("this processor has no long mode (CPUID.80000001H:EDX.LM clear)");

    /* EFER is a model-specific register, and rdmsr/wrmsr are what set
     * LME. A processor with long mode but no MSRs does not exist, but the
     * check costs nothing and documents the dependency. */
    if (!cpu->has_msr)
        NO("no rdmsr/wrmsr, so EFER.LME cannot be set");
    if (!cpu->has_pae)
        NO("no PAE, and long mode is PAE with a fourth level");
    if (!vmm_is_enabled())
        NO("paging is not up yet");

    return true;
#undef NO
}

/* Allocate and fill the three tables and the probe frame. Once. */
static bool build_tables(const char **why)
{
    if (tables.built)
        return true;

    paddr_t frames[4] = {0, 0, 0, 0};
    volatile u64 *maps[4] = {NULL, NULL, NULL, NULL};

    for (u32 i = 0; i < 4; i++) {
        /* The probe frame - the last one - has to sit *outside* the 4 MiB
         * identity map the transition runs under, or reading it from 64-bit
         * mode proves nothing: the bootstrap directory would have mapped it
         * too. The first version of this allocated it like the others and
         * the warning below fired on the first run, which is the only reason
         * it is not still wrong. */
        frames[i] =
            (i == 3) ? pmm_alloc_frame_above(4 * MIB) : pmm_alloc_frame();
        if (!frames[i]) {
            *why = "out of physical memory for the four-level tables";
            goto fail;
        }

        /* Mapped through the MMIO window rather than the linear map, because
         * the linear map covers only the first 16 MiB and the PMM is free to
         * hand out a frame from anywhere. The window is uncached=false: this
         * is ordinary memory. */
        maps[i] = vmm_map_mmio(frames[i], PAGE_SIZE, false);
        if (!maps[i]) {
            *why = "no MMIO window left to build the four-level tables in";
            goto fail;
        }
    }

    tables.pml4 = frames[0];
    tables.pdpt = frames[1];
    tables.pd = frames[2];
    tables.probe = frames[3];
    tables.pml4_map = maps[0];
    tables.pdpt_map = maps[1];
    tables.pd_map = maps[2];
    tables.probe_map = maps[3];

    /* The processor walks page tables by physical address, so these three
     * need no mapping of their own for the walk to work - the windows above
     * exist only so this code can write them. That is worth stating because
     * it is the opposite of the constraint on the trampoline. */
    for (u32 i = 0; i < PAGE_SIZE / sizeof(u64); i++) {
        tables.pml4_map[i] = 0;
        tables.pdpt_map[i] = 0;
        tables.pd_map[i] = 0;
    }

    tables.pml4_map[0] = (u64)tables.pdpt | PTE64_PRESENT | PTE64_WRITE;
    tables.pdpt_map[0] = (u64)tables.pd | PTE64_PRESENT | PTE64_WRITE;

    for (u32 i = 0; i < LM_IDENTITY_ENTRIES; i++)
        tables.pd_map[i] =
            ((u64)i << 21) | PTE64_PRESENT | PTE64_WRITE | PTE64_PS;

    /* The probe. A magic number at a physical address that only the
     * four-level walk can reach: the kernel's 32-bit directory does not map
     * it, and the identity map the caller switches to covers only the first
     * 4 MiB. Reading it back from 64-bit mode is therefore a test of the
     * walk and not of memory. */
    tables.probe_map[0] = LM_PROBE_MAGIC;

    /* Both halves of that claim, checked rather than assumed. Outside the
     * identity map, or the test is vacuous; inside the gigabyte the tables
     * cover, or the read faults with no handler to take it. */
    if (tables.probe < 4 * MIB) {
        *why = "the probe frame landed inside the identity map, which would "
               "make the 64-bit pointer test pass for the wrong reason";
        goto fail;
    }
    if (tables.probe >= (paddr_t)LM_IDENTITY_MIB * MIB) {
        *why = "the probe frame is past the gigabyte the tables map";
        goto fail;
    }

    tables.built = true;

    pr_debug("four-level tables: pml4 %p -> pdpt %p -> pd %p, %u MiB "
             "identity in 2 MiB pages; probe at %p",
             (void *)tables.pml4, (void *)tables.pdpt, (void *)tables.pd,
             LM_IDENTITY_MIB, (void *)tables.probe);
    return true;

fail:
    /* The MMIO window is a bump allocator with no free, so a failure here
     * leaks address space - not frames. Said out loud rather than hidden:
     * the window is 8 MiB and this path runs at most once. */
    for (u32 i = 0; i < 4; i++)
        if (frames[i])
            pmm_free_frame(frames[i]);

    /* The cached addresses go with them. `built` stays false so nothing
     * reads these, but leaving a freed frame's address in a static is how a
     * later edit turns a clean failure into a use-after-free. */
    tables.pml4 = tables.pdpt = tables.pd = tables.probe = 0;
    tables.pml4_map = tables.pdpt_map = tables.pd_map = tables.probe_map = NULL;
    return false;
}

/* Copy the trampoline to its runnable address and fill in its inputs. */
static u8 *install_trampoline(paddr_t return_cr3)
{
    size_t size = (size_t)(lm_trampoline_end - lm_trampoline_start);
    u8 *dst = phys_to_virt(LM_TRAMPOLINE_PHYS);

    if (size > LM_TRAMPOLINE_MAX)
        panic("the long-mode trampoline is %u bytes; it has to leave room "
              "for its own stack in one page",
              (unsigned)size);

    memcpy(dst, lm_trampoline_start, size);

    *(volatile u32 *)(dst + LM_OFF(lm_off_flags)) = 0;
    *(volatile u32 *)(dst + LM_OFF(lm_off_pml4)) = (u32)tables.pml4;
    *(volatile u32 *)(dst + LM_OFF(lm_off_return_cr3)) = (u32)return_cr3;
    *(volatile u64 *)(dst + LM_OFF(lm_off_probe_phys)) = (u64)tables.probe;

    return dst;
}

static void collect(const u8 *tramp, struct longmode_result *r)
{
    r->flags = *(const volatile u32 *)(tramp + LM_OFF(lm_off_flags));
    r->cs = *(const volatile u32 *)(tramp + LM_OFF(lm_off_cs));
    r->wide = *(const volatile u64 *)(tramp + LM_OFF(lm_off_wide));
    r->crossed = *(const volatile u64 *)(tramp + LM_OFF(lm_off_crossed));
    r->r15 = *(const volatile u64 *)(tramp + LM_OFF(lm_off_r15));
    r->rip_lea = *(const volatile u64 *)(tramp + LM_OFF(lm_off_rip));
    r->efer = *(const volatile u64 *)(tramp + LM_OFF(lm_off_efer));
    r->cr4 = *(const volatile u64 *)(tramp + LM_OFF(lm_off_cr4));
    r->probe_value =
        *(const volatile u64 *)(tramp + LM_OFF(lm_off_probe_value));
}

/* Check everything the payload reported. Separate from running it, so that a
 * failure names the property that did not hold rather than "it did not
 * work". */
static const char *validate(const struct longmode_result *r)
{
    if ((r->flags & LM_F_ENTERED) == 0)
        return "the trampoline never reached its own GDT";
    if ((r->flags & LM_F_COMPAT) == 0)
        return "paging never came back on with the PML4 loaded";
    if ((r->flags & LM_F_LONG) == 0)
        return "the far jump to the 64-bit code segment did not arrive";
    if ((r->flags & LM_F_VERIFIED) == 0)
        return "the 64-bit payload did not finish";
    if ((r->flags & LM_F_BACK32) == 0)
        return "the way back to 32-bit code did not arrive";
    if ((r->flags & LM_F_RETURNED) == 0)
        return "the kernel's GDT and stack were never restored";

    if (r->wide != LM_WIDE_VALUE)
        return "a 64-bit immediate did not survive in a register";
    if (r->crossed != LM_CROSSED_VALUE)
        return "0xFFFFFFFF + 1 did not carry past bit 31";
    if (r->r15 != LM_R15_VALUE)
        return "r15 did not hold what was put in it";
    if (r->rip_lea != (u64)r->rip_expected)
        return "RIP-relative addressing did not resolve to the right address";
    if ((r->efer & EFER_LMA_BIT) == 0)
        return "EFER.LMA was clear, so the processor was not in long mode";
    if ((r->efer & EFER_LME_BIT) == 0)
        return "EFER.LME was clear inside long mode, which cannot happen";
    if ((r->cr4 & CR4_PAE_BIT) == 0)
        return "CR4.PAE was clear, which long mode does not permit";
    if (r->probe_value != LM_PROBE_MAGIC)
        return "the read through a 64-bit pointer did not return the magic";
    if (r->cs != LM_SEL_CODE64)
        return "the 64-bit payload was not running on the 64-bit selector";

    /* And the kernel's own state, which is the part that matters for
     * everything after this function returns. */
    if (read_cr4() & CR4_PAE_BIT)
        return "CR4.PAE was left set, so 32-bit paging is now wrong";
    if (!(read_cr0() & 0x80000000u))
        return "paging was left off";

    return NULL;
}

bool longmode_round_trip(struct longmode_result *out)
{
    struct longmode_result r;
    const char *why = NULL;

    memset(&r, 0, sizeof(r));
    r.trampoline_bytes = (u32)(lm_trampoline_end - lm_trampoline_start);
    r.identity_mib = LM_IDENTITY_MIB;

    r.supported = longmode_supported();
    if (!longmode_available(&why)) {
        r.failure = why;
        goto done;
    }

    if (!build_tables(&why)) {
        r.failure = why;
        goto done;
    }

    r.pml4_phys = tables.pml4;
    r.pdpt_phys = tables.pdpt;
    r.pd_phys = tables.pd;
    r.probe_phys = tables.probe;

    /* Where the 64-bit `lea rax, [rel lm_marker]` should land. Computed from
     * the trampoline's own bytes rather than hardcoded, so the check tests
     * the addressing mode instead of a constant that would have to be
     * updated whenever the code above the marker changes. */
    {
        const u8 *marker = NULL;

        for (const u8 *p = lm_trampoline_start; p + 8 <= lm_trampoline_end;
             p++) {
            u64 v;

            memcpy(&v, p, sizeof(v));
            if (v == LM_MARKER_VALUE) {
                marker = p;
                break;
            }
        }

        if (!marker) {
            r.failure = "the marker the RIP-relative test looks for is not "
                        "in the trampoline";
            goto done;
        }

        r.rip_expected =
            LM_TRAMPOLINE_PHYS + (u32)(marker - lm_trampoline_start);
    }

    /* A directory with both the identity map and the kernel's half. The
     * kernel's own has no low mapping - dropping it is what frees user space
     * - so the trampoline could not be reached through it, and after the
     * transition there would be nothing to come back to. The SMP bring-up
     * needs the same thing for the same reason. */
    paddr_t bootstrap = vmm_create_bootstrap_pd();

    if (!bootstrap) {
        r.failure = "could not build a page directory with an identity map";
        goto done;
    }

    u8 *tramp = install_trampoline(bootstrap);
    void (*enter)(void) = (void (*)(void))(
        unsigned long)(LM_TRAMPOLINE_PHYS + LM_OFF(lm_enter_long_mode_off));

    /* Interrupts off for the window, and they have to be: between the
     * trampoline's lgdt and the one that puts the kernel's back there is no
     * descriptor table a handler could be dispatched through, and long mode
     * needs a different IDT format anyway. An NMI here would be fatal. */
    bool irqs = irq_save();
    paddr_t saved_cr3 = read_cr3();
    u64 before, after;

    r.attempted = true;

    write_cr3(bootstrap);
    before = rdtsc();
    enter();
    after = rdtsc();

    /* The trampoline came back on the bootstrap directory, because that is
     * the only one that had a mapping for the instruction doing the
     * restoring. Whatever was current before - the kernel's directory, or a
     * user process's - goes back now. */
    write_cr3(saved_cr3);
    irq_restore(irqs);

    r.cycles = after - before;
    collect(tramp, &r);
    vmm_destroy_bootstrap_pd(bootstrap);

    r.failure = validate(&r);
    r.round_trip = r.failure == NULL;

done:
    last = r;
    if (out)
        *out = r;

    if (r.round_trip)
        pr_info("64-bit long mode entered and left: CS 0x%x, EFER.LMA set, "
                "%u MiB identity-mapped by a 4-level table, round trip %llu "
                "cycles",
                r.cs, r.identity_mib, r.cycles);
    else if (r.attempted)
        pr_err("long mode round trip failed: %s (flags 0x%02x)", r.failure,
               r.flags);
    else
        pr_info("long mode not attempted: %s", r.failure ? r.failure : "?");

    return r.round_trip;
}

const struct longmode_result *longmode_last(void)
{
    return &last;
}
