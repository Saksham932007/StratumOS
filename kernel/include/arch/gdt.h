/* StratumOS - Global Descriptor Table and Task State Segment.
 *
 * Flat segmentation: every segment covers the full 4 GiB, so segmentation
 * stays out of the way and paging does the real protection work. The only
 * reason we keep separate ring-3 descriptors is that the CPU determines the
 * current privilege level from CS's RPL.
 *
 * The TSS exists for exactly one reason on a 32-bit kernel that does software
 * task switching: when an interrupt arrives while the CPU is in ring 3, it
 * needs somewhere to find the ring-0 stack to switch to. That is ss0/esp0.
 */
#ifndef _ARCH_GDT_H
#define _ARCH_GDT_H

#include <kernel/types.h>

/* The upper bound on processors this kernel will use. It is here rather than
 * in the SMP code because the GDT has to be sized for it: `ltr` names a
 * descriptor, so each CPU needs a TSS descriptor of its own. */
#define SMP_MAX_CPUS    8

#define GDT_NULL        0
#define GDT_KERNEL_CODE 1
#define GDT_KERNEL_DATA 2
#define GDT_USER_CODE   3
#define GDT_USER_DATA   4
/* One TSS descriptor per CPU, from here on. A shared GDT with per-CPU TSS
 * descriptors rather than a GDT per CPU: the only per-CPU thing in a GDT is
 * the TSS, and duplicating four identical flat descriptors eight times to
 * avoid eight extra entries would be the wrong trade. */
#define GDT_TSS_FIRST   5
#define GDT_ENTRIES     (GDT_TSS_FIRST + SMP_MAX_CPUS)

#define SEL_KERNEL_CODE (GDT_KERNEL_CODE * 8)         /* 0x08 */
#define SEL_KERNEL_DATA (GDT_KERNEL_DATA * 8)         /* 0x10 */
#define SEL_USER_CODE   ((GDT_USER_CODE * 8) | 3)     /* 0x1B, RPL 3 */
#define SEL_USER_DATA   ((GDT_USER_DATA * 8) | 3)     /* 0x23, RPL 3 */
#define SEL_TSS(cpu)    ((GDT_TSS_FIRST + (cpu)) * 8) /* 0x28 for cpu 0 */

struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8 base_mid;
    u8 access;
    u8 granularity;
    u8 base_high;
} PACKED;

struct gdt_ptr {
    u16 limit;
    u32 base;
} PACKED;

/* 80386 hardware task state segment. We only ever populate ss0/esp0 and
 * iomap_base; the rest exists because the CPU requires the layout. */
struct tss_entry {
    u32 prev_tss;
    u32 esp0;
    u32 ss0;
    u32 esp1, ss1, esp2, ss2;
    u32 cr3, eip, eflags;
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 es, cs, ss, ds, fs, gs;
    u32 ldt;
    u16 trap;
    u16 iomap_base;
} PACKED;

/* Access byte bits */
#define GDT_PRESENT    0x80
#define GDT_RING0      0x00
#define GDT_RING3      0x60
#define GDT_SEGMENT    0x10
#define GDT_EXEC       0x08
#define GDT_RW         0x02

/* Granularity byte bits */
#define GDT_GRAN_4K    0x80
#define GDT_GRAN_32BIT 0x40

/* Build the GDT and install the boot processor's TSS. Also builds every
 * other CPU's TSS descriptor, because the table is shared and sized once. */
void gdt_init(void);
void gdt_set_entry(int idx, u32 base, u32 limit, u8 access, u8 gran);

/* Load the GDT and this CPU's own TSS. Called by each application processor
 * once it reaches C: the table is already built, but the descriptor registers
 * are per-CPU state that a processor coming out of reset does not have. */
void gdt_init_ap(u32 cpu);

/* Record the ring-0 stack the CPU should switch to on a ring 3 -> 0 trap.
 * Writes *this* CPU's TSS: the CPU reads the TSS its own task register names,
 * so a kernel with one shared TSS would have a ring-3 interrupt on one
 * processor land on another processor's stack. */
void tss_set_kernel_stack(u32 esp0);

/* This CPU's TSS, for inspection. */
const struct tss_entry *tss_of(u32 cpu);

#endif /* _ARCH_GDT_H */
