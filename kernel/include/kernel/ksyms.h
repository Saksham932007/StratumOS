/* StratumOS - in-kernel symbol table.
 *
 * The table is generated from the linked kernel by tools/gen-ksyms.py and
 * embedded on a second link pass. Two things depend on it:
 *
 *   * panic backtraces, which print `function+0x1c` instead of a bare address
 *     and so are readable without addr2line and without the debug ELF;
 *   * the sampling profiler, which attributes timer ticks to functions.
 *
 * Only function symbols are present. That is a deliberate constraint: it keeps
 * the table's size independent of the addresses it contains, which is what
 * makes the multi-pass link converge. See the generator's docstring.
 */
#ifndef _KERNEL_KSYMS_H
#define _KERNEL_KSYMS_H

#include <kernel/types.h>

struct ksym {
    u32 addr;
    const char *name;
};

/* Generated; sorted by address. */
extern const struct ksym ksym_table[];
extern const u32 ksym_count;

bool ksyms_available(void);

/* Name of the function containing `addr`, with the offset into it, or NULL if
 * the address is outside every known function. */
const char *ksym_lookup(u32 addr, u32 *offset_out);

/* Index into ksym_table of the function containing `addr`, or -1. Used by the
 * profiler to aggregate samples without a string compare per tick. */
int ksym_index(u32 addr);

/* Print "name+0xoff" for `addr`, or the raw address when it is unknown. */
void ksym_print(u32 addr);

#endif /* _KERNEL_KSYMS_H */
