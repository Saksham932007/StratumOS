/* StratumOS - 64-bit long mode: getting into it, proving it, and getting out.
 *
 * This is the third step of the arc the project is named for: 16-bit real
 * mode to 32-bit protected mode to 64-bit long mode. The transition itself
 * lives in kernel/arch/x86/longmode.asm, which explains the mechanics; this
 * header is about the boundary of the claim.
 *
 * WHAT IT DOES
 * ------------
 * Builds a four-level page table, puts the boot processor into 64-bit mode,
 * runs a payload there that could not run anywhere else, and brings it back
 * to 32-bit protected mode with the kernel still running. Everything the
 * payload observed comes back in a struct the shell prints and a test suite
 * asserts on.
 *
 * WHAT IT IS NOT
 * --------------
 * It is not a 64-bit kernel, and saying so clearly matters more than the
 * feature does. StratumOS's own code is 32-bit: `vaddr_t` is a `u32`, the
 * IDT holds 8-byte gates, the syscall path is `int 0x80`, and the scheduler
 * saves 32-bit register frames. A port means all of:
 *
 *   - a 64-bit IDT (16-byte gate descriptors, and an interrupt stack table),
 *   - a new calling convention in every assembly stub,
 *   - SYSCALL/SYSRET instead of a software interrupt, which needs STAR,
 *     LSTAR and SFMASK set up,
 *   - a four-level VMM, with the recursive-mapping trick replaced (the
 *     kernel's current PD_VADDR scheme is specific to two levels),
 *   - auditing every u32 that is really an address.
 *
 * Each of those is tractable; together they are a port, not a phase. See
 * docs/LONGMODE.md for the design and docs/ROADMAP.md for the order.
 *
 * So what is here is the part that genuinely had to come first: until the
 * processor can be driven into the mode and back out again, under test, none
 * of the rest can be developed at all.
 */
#ifndef _ARCH_LONGMODE_H
#define _ARCH_LONGMODE_H

#include <kernel/types.h>

/* Progress flags, set by the trampoline as it passes each point. A run that
 * dies partway through still reports where it got to - during development
 * that was the difference between a diagnosis and a reboot. Kept in sync
 * with the LM_F_* equates in longmode.asm, which the build checks by
 * asserting the two agree. */
#define LM_F_ENTERED  0x01u /* running on the trampoline's own GDT, 32-bit */
#define LM_F_COMPAT   0x02u /* CR0.PG set with the PML4: compatibility mode */
#define LM_F_LONG     0x04u /* executing 64-bit instructions               */
#define LM_F_VERIFIED 0x08u /* every probe written                         */
#define LM_F_BACK32   0x10u /* back to 32-bit code, still 64-bit paging    */
#define LM_F_RETURNED 0x20u /* kernel CR3, kernel GDT, kernel stack        */

#define LM_F_ALL                                                            \
    (LM_F_ENTERED | LM_F_COMPAT | LM_F_LONG | LM_F_VERIFIED | LM_F_BACK32 | \
     LM_F_RETURNED)

/* The values the 64-bit payload is built to produce. They are here rather
 * than only in the assembly so that a test can assert on them, which makes
 * the payload's arithmetic part of the test rather than part of the log. */
#define LM_WIDE_VALUE    0x0123456789ABCDEFull
#define LM_CROSSED_VALUE 0x0000000100000000ull /* 0xFFFFFFFF + 1           */
#define LM_R15_VALUE     0xFEEDFACECAFEBEEFull
#define LM_MARKER_VALUE  0x4C4F4E474D4F4445ull /* "LONGMODE", little-endian */
#define LM_PROBE_MAGIC   0x5452415455004F53ull /* "STRATUM" with a hole     */

/* The 64-bit code selector in the trampoline's own GDT. Written down once
 * here rather than as a literal in the validator and again in the test
 * suite; it must match LM_SEL_CODE64 in longmode_tramp.asm, and the
 * validator is what checks that it does. */
#define LM_SEL_CODE64    0x18u

#define EFER_LME_BIT     0x00000100u
#define EFER_LMA_BIT     0x00000400u
#define CR4_PAE_BIT      0x00000020u

struct longmode_result {
    bool supported;  /* CPUID reports long mode                    */
    bool attempted;  /* a transition was actually tried            */
    bool round_trip; /* ... and every stage of it was reached      */

    u32 flags; /* LM_F_*                                           */
    u32 cs;    /* the CS selector the 64-bit payload ran on        */

    /* What the payload observed. Each is checked rather than printed. */
    u64 wide;        /* a 64-bit immediate                          */
    u64 crossed;     /* a carry across bit 31 in one instruction    */
    u64 r15;         /* a register 32-bit mode does not have        */
    u64 rip_lea;     /* lea rax, [rel marker]                       */
    u64 efer;        /* for LMA - the processor's own statement      */
    u64 cr4;         /* for PAE                                      */
    u64 probe_value; /* read through a 64-bit pointer                */

    /* What the 32-bit side set up, so a report can show the geometry. */
    paddr_t pml4_phys;
    paddr_t pdpt_phys;
    paddr_t pd_phys;
    paddr_t probe_phys;
    vaddr_t rip_expected; /* where the marker really is              */
    u32 identity_mib;     /* how much the four-level tables cover    */
    u32 trampoline_bytes;
    u64 cycles; /* the whole round trip, by TSC                      */

    const char *failure; /* NULL on success                         */
};

/* Does this processor have long mode at all? CPUID leaf 0x80000001, EDX bit
 * 29. Cheap, and safe to call before anything is set up. */
bool longmode_supported(void);

/* Everything needed to attempt a transition - the feature, the MSR
 * instructions it needs, and the memory it needs - or false with a reason. */
bool longmode_available(const char **why);

/* Build the tables, go to 64-bit mode, run the payload, come back. Returns
 * true only if every stage was reached and every probe held. Safe to call
 * repeatedly: the tables are built once and reused.
 *
 * Takes interrupts down for the duration - there is no interrupt descriptor
 * table valid in both modes - and restores them. Must be called from ring 0
 * on the boot processor, with a kernel stack. */
bool longmode_round_trip(struct longmode_result *out);

/* The last run's result, for the shell. Zeroed if there has not been one. */
const struct longmode_result *longmode_last(void);

#endif /* _ARCH_LONGMODE_H */
