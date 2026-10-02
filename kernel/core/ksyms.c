/* StratumOS - symbol table lookup.
 *
 * The table itself is generated (see tools/gen-ksyms.py); this is the
 * hand-written half that searches it. Kept separate so that the generated file
 * contains nothing but data, which is what keeps the multi-pass link stable.
 */
#include <kernel/kernel.h>
#include <kernel/ksyms.h>
#include <kernel/printf.h>

bool ksyms_available(void)
{
    return ksym_count > 0;
}

int ksym_index(u32 addr)
{
    if (ksym_count == 0)
        return -1;

    /* Only addresses inside an executable section can belong to a function.
     * Checking this first means a stack value that happens to look like a
     * small integer does not get attributed to whatever function sits lowest
     * in memory.
     *
     * Both executable sections count. .user holds the ring-3 payload, and the
     * generator emits its symbols too, so a panic that arrives from ring 3
     * resolves to a name rather than a bare address. Omitting .user here is
     * what the ksyms self-test caught: those symbols could not resolve to
     * themselves. */
    bool in_text = addr >= (u32)__text_start && addr < (u32)__text_end;
    bool in_user = addr >= (u32)__user_start && addr < (u32)__user_end;

    if (!in_text && !in_user)
        return -1;

    /* Binary search for the last symbol whose address is <= addr. The table is
     * sorted, so this is the function containing it. */
    u32 lo = 0, hi = ksym_count - 1, best = 0;
    bool found = false;

    while (lo <= hi) {
        u32 mid = lo + (hi - lo) / 2;

        if (ksym_table[mid].addr <= addr) {
            best = mid;
            found = true;
            if (mid == ksym_count - 1)
                break;
            lo = mid + 1;
        } else {
            if (mid == 0)
                break;
            hi = mid - 1;
        }
    }

    return found ? (int)best : -1;
}

const char *ksym_lookup(u32 addr, u32 *offset_out)
{
    int idx = ksym_index(addr);

    if (idx < 0)
        return NULL;

    if (offset_out)
        *offset_out = addr - ksym_table[idx].addr;

    return ksym_table[idx].name;
}

void ksym_print(u32 addr)
{
    u32 offset = 0;
    const char *name = ksym_lookup(addr, &offset);

    if (!name) {
        kprintf("%p", (void *)addr);
        return;
    }

    if (offset)
        kprintf("%s+0x%x", name, offset);
    else
        kprintf("%s", name);
}
