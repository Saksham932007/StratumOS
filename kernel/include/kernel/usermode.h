/* StratumOS - dropping to ring 3. */
#ifndef _KERNEL_USERMODE_H
#define _KERNEL_USERMODE_H

#include <arch/idt.h>

#include <kernel/types.h>

/* Map the embedded user payload, then iret into it at ring 3. */
bool usermode_spawn_demo(void);
bool usermode_ran(void);

/* Implemented in arch/x86/usermode.asm: forge an inter-privilege iret frame
 * and return into it. Never comes back. */
NORETURN void usermode_enter(vaddr_t entry, vaddr_t user_stack_top);

/* Map USER_STACK_PAGES writable user pages below USER_STACK_TOP in the
 * current address space. Used when a process is first built and again after
 * exec() has cleared the old image out. */
bool usermode_map_stack(void);

/* Replace the calling process's image with the named embedded program,
 * rewriting `r` so the syscall's own interrupt return lands in the new
 * image's entry point. Returns false only when the name is unknown or the
 * caller has no address space of its own; past that point a failure kills
 * the process, because its old image is already gone. */
bool usermode_exec(const char *name, struct regs *r);

/* The i'th embedded program's name, or NULL once past the end. exec()'s
 * namespace, which the shell lists. */
const char *usermode_program_name(u32 index);
u32 usermode_exec_count(void);

/* Start a named embedded or on-disk program as a new ring-3 process, and
 * return its pid - or 0 if it could not be started. The boot path uses this
 * for `init`; the shell uses it for the syscall fuzzer, and needs the pid so
 * it can wait for that process specifically. */
u32 usermode_spawn_named(const char *name);

/* How many program images came off the disk, and how many from the copies
 * embedded in the kernel image. The second number being non-zero on a kernel
 * booted from a disk would mean a file is missing from the filesystem. */
void usermode_get_load_counts(u32 *disk, u32 *embedded);

#endif /* _KERNEL_USERMODE_H */
