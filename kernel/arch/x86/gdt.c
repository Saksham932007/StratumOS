/* StratumOS - Global Descriptor Table and Task State Segment.
 *
 * Flat segmentation. Every descriptor spans the whole 4 GiB address space, so
 * a linear address equals a logical offset and segmentation effectively
 * disappears. Protection is paging's job; keeping segmentation trivial means
 * there is one less mechanism to reason about when something faults.
 *
 * Ring-3 descriptors exist because the CPU reads the current privilege level
 * out of CS's low two bits - there is no other way to *be* in user mode.
 *
 * The TSS is needed for one field. When an interrupt fires while the CPU is in
 * ring 3, the CPU switches to a ring-0 stack, and the only place it will look
 * for that stack's address is ss0/esp0 in the active TSS. Software task
 * switching does not otherwise use it.
 */
#define LOG_TAG "gdt"

#include <arch/gdt.h>

#include <kernel/log.h>
#include <kernel/smp.h>
#include <kernel/string.h>

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr gdt_pointer;

/* One TSS per processor. The CPU reads ss0/esp0 out of whichever TSS its own
 * task register names, so a shared one would mean a ring-3 interrupt taken on
 * one processor switching to another processor's kernel stack - which is the
 * sort of corruption that only appears under load, on a machine with more
 * cores than the one it was developed on. */
static struct tss_entry tss[SMP_MAX_CPUS];

/* Implemented in arch/x86/gdt_flush.asm */
extern void gdt_flush(const struct gdt_ptr *ptr);
extern void tss_flush(u16 selector);

void gdt_set_entry(int idx, u32 base, u32 limit, u8 access, u8 gran)
{
    gdt[idx].base_low = (u16)(base & 0xFFFF);
    gdt[idx].base_mid = (u8)((base >> 16) & 0xFF);
    gdt[idx].base_high = (u8)((base >> 24) & 0xFF);
    gdt[idx].limit_low = (u16)(limit & 0xFFFF);
    /* The granularity byte is a mongrel: low nibble is limit bits 19:16,
     * high nibble is the flags. */
    gdt[idx].granularity = (u8)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[idx].access = access;
}

void tss_set_kernel_stack(u32 esp0)
{
    tss[smp_cpu_index()].esp0 = esp0;
}

const struct tss_entry *tss_of(u32 cpu)
{
    return cpu < SMP_MAX_CPUS ? &tss[cpu] : NULL;
}

void gdt_init(void)
{
    const u8 code_access = GDT_PRESENT | GDT_SEGMENT | GDT_EXEC | GDT_RW;
    const u8 data_access = GDT_PRESENT | GDT_SEGMENT | GDT_RW;
    const u8 gran = GDT_GRAN_4K | GDT_GRAN_32BIT;

    memset(gdt, 0, sizeof(gdt));
    memset(tss, 0, sizeof(tss));

    /* A limit of 0xFFFFF with 4 KiB granularity covers 0xFFFFFFFF bytes. */
    gdt_set_entry(GDT_NULL, 0, 0, 0, 0);
    gdt_set_entry(GDT_KERNEL_CODE, 0, 0xFFFFF, code_access | GDT_RING0, gran);
    gdt_set_entry(GDT_KERNEL_DATA, 0, 0xFFFFF, data_access | GDT_RING0, gran);
    gdt_set_entry(GDT_USER_CODE, 0, 0xFFFFF, code_access | GDT_RING3, gran);
    gdt_set_entry(GDT_USER_DATA, 0, 0xFFFFF, data_access | GDT_RING3, gran);

    /* Every processor's TSS descriptor, built once. A TSS descriptor is a
     * system descriptor: S=0, type 9 (available 32-bit TSS), byte
     * granularity, and a limit that is the real size of the structure rather
     * than 4 GiB.
     *
     * All of them are built here even though only one processor exists yet,
     * because the GDT is shared and sized once - and because an application
     * processor coming up later should only have to execute `ltr`, not edit a
     * descriptor table that other processors are concurrently using. */
    for (u32 cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        tss[cpu].ss0 = SEL_KERNEL_DATA;
        tss[cpu].esp0 = 0; /* filled in per task by tss_set_kernel_stack() */
        /* Setting iomap_base past the end of the TSS means "no I/O permission
         * bitmap", which makes every port access from ring 3 fault. That is
         * the behaviour we want: userspace has no business talking to
         * hardware. */
        tss[cpu].iomap_base = sizeof(struct tss_entry);

        gdt_set_entry(GDT_TSS_FIRST + (int)cpu, (u32)&tss[cpu],
                      sizeof(struct tss_entry) - 1, GDT_PRESENT | 0x09, 0x00);
    }

    gdt_pointer.limit = (u16)(sizeof(gdt) - 1);
    gdt_pointer.base = (u32)&gdt;

    gdt_flush(&gdt_pointer);
    tss_flush(SEL_TSS(smp_cpu_index()));

    pr_debug("GDT at %p, %u entries; %u TSS(es) from %p", (void *)&gdt,
             (unsigned)GDT_ENTRIES, (unsigned)SMP_MAX_CPUS, (void *)&tss[0]);
}

void gdt_init_ap(u32 cpu)
{
    /* The table is already built and must not be touched: other processors
     * are using it. All that is missing is this processor's descriptor
     * registers, which came out of reset empty. */
    gdt_flush(&gdt_pointer);
    tss_flush(SEL_TSS(cpu));
}
