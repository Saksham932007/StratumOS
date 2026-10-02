/* StratumOS - CPU-enforced separation between the kernel and user memory.
 *
 * Paging already says which pages ring 3 may touch. SMEP and SMAP are the
 * other direction: they stop the *kernel* from touching user memory by
 * accident, which is the direction that matters once an attacker controls
 * what is in those pages.
 *
 *   SMEP (CR4 bit 20)  ring 0 may not *execute* from a user page. A kernel
 *                      that jumps through a corrupted function pointer into
 *                      a buffer the attacker filled gets a fault instead of
 *                      a shellcode execution.
 *
 *   SMAP (CR4 bit 21)  ring 0 may not *read or write* a user page either,
 *                      unless EFLAGS.AC is set. Every accidental
 *                      dereference of a user pointer faults; every
 *                      deliberate one has to say so.
 *
 * SMAP is the one with a cost, and the cost is the point. The kernel does
 * legitimately touch user memory - write(2) reads a user buffer, wait(2)
 * writes a user int, the ELF loader fills user pages, the copy-on-write
 * handler copies one. Each of those is now wrapped in
 * user_access_begin()/user_access_end(), so the places where the kernel
 * dereferences a ring-3 pointer are an enumerable list rather than
 * "anywhere".
 *
 * Both are detected rather than assumed: neither exists before Ivy Bridge
 * (SMEP) and Haswell (SMAP), and an emulator may expose neither. When a
 * feature is absent the kernel says so at boot and the stac/clac pair
 * becomes a no-op - executing them without CPUID support is #UD, not a
 * silent nothing.
 */
#ifndef _ARCH_HARDEN_H
#define _ARCH_HARDEN_H

#include <kernel/types.h>

#define CR4_SMEP (1u << 20)
#define CR4_SMAP (1u << 21)

/* Probe CPUID leaf 7 and set whatever the CPU supports in CR4. Must run
 * before any address space other than the kernel's exists, and before the
 * first user program, because turning SMAP on retroactively would fault in
 * code that had not been written for it. */
void harden_init(void);

struct harden_state {
    bool smep_available, smep_enabled;
    bool smap_available, smap_enabled;
    bool wp_enabled;         /* CR0.WP: ring 0 honours read-only pages    */
    bool kernel_text_ro;     /* .text and .rodata mapped without WRITE    */
    bool stack_guard_pages;  /* an unmapped page below every task stack   */
    bool stack_canaries;     /* a magic word checked on every switch      */
    u32 user_access_windows; /* how many times AC has been raised         */
};

const struct harden_state *harden_get_state(void);
void harden_note_text_ro(void);

/* Raise and lower EFLAGS.AC around a deliberate access to user memory.
 *
 * Nested use is fine and intentional: the count is a diagnostic, and AC is
 * part of EFLAGS, so an interrupt arriving inside a window saves and restores
 * it like any other flag. The window should still be as short as the access
 * itself - while it is open, an accidental user dereference is no longer
 * caught.
 *
 * The opcodes are written out because `stac` and `clac` are rejected by
 * older assemblers, and this kernel has to build with whatever is on the
 * machine. They are two bytes either way.
 */
void user_access_begin(void);
void user_access_end(void);

#endif /* _ARCH_HARDEN_H */
