/* StratumOS - CPUID-based processor identification and control registers. */
#ifndef _ARCH_CPU_H
#define _ARCH_CPU_H

#include <kernel/types.h>

struct cpu_info {
    char vendor[13]; /* 12 chars + NUL, e.g. "GenuineIntel" */
    char brand[49];  /* 48 chars + NUL, from leaf 0x80000002.. */
    u32 max_leaf;
    u32 max_ext_leaf;
    u8 family, model, stepping;
    u32 features_edx; /* leaf 1 EDX */
    u32 features_ecx; /* leaf 1 ECX */
    bool has_cpuid;
    bool has_tsc, has_msr, has_pae, has_pge, has_pse, has_apic;
    bool has_fpu, has_mmx, has_sse, has_sse2, has_sse3, has_hypervisor;
    u32 cache_line_size;
};

void cpu_detect(void);
const struct cpu_info *cpu_get_info(void);
void cpu_print_info(void);

/* Reset the machine: ask the 8042 first, then deliberately triple-fault by
 * loading a null IDT. Both are standard; neither is graceful. */
NORETURN void cpu_reset(void);
NORETURN void cpu_halt_forever(void);

/* Ask QEMU to exit with a status code. Only has an effect when the VM was
 * started with `-device isa-debug-exit`; harmless otherwise, which is what
 * makes it safe to leave in a release build. */
void cpu_qemu_exit(u8 code);

static inline void cpuid_raw(u32 leaf, u32 subleaf, u32 *a, u32 *b, u32 *c,
                             u32 *d)
{
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

static inline u64 rdtsc(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

#endif /* _ARCH_CPU_H */
