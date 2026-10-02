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

#define GDT_NULL        0
#define GDT_KERNEL_CODE 1
#define GDT_KERNEL_DATA 2
#define GDT_USER_CODE   3
#define GDT_USER_DATA   4
#define GDT_TSS         5
#define GDT_ENTRIES     6

#define SEL_KERNEL_CODE (GDT_KERNEL_CODE * 8)        /* 0x08 */
#define SEL_KERNEL_DATA (GDT_KERNEL_DATA * 8)        /* 0x10 */
#define SEL_USER_CODE   ((GDT_USER_CODE * 8) | 3)    /* 0x1B, RPL 3 */
#define SEL_USER_DATA   ((GDT_USER_DATA * 8) | 3)    /* 0x23, RPL 3 */
#define SEL_TSS         (GDT_TSS * 8)                /* 0x28 */

struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  granularity;
    u8  base_high;
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
#define GDT_PRESENT  0x80
#define GDT_RING0    0x00
#define GDT_RING3    0x60
#define GDT_SEGMENT  0x10
#define GDT_EXEC     0x08
#define GDT_RW       0x02

/* Granularity byte bits */
#define GDT_GRAN_4K    0x80
#define GDT_GRAN_32BIT 0x40

void gdt_init(void);
void gdt_set_entry(int idx, u32 base, u32 limit, u8 access, u8 gran);

/* Record the ring-0 stack the CPU should switch to on a ring 3 -> 0 trap. */
void tss_set_kernel_stack(u32 esp0);

#endif /* _ARCH_GDT_H */
