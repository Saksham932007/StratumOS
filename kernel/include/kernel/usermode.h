/* StratumOS - dropping to ring 3. */
#ifndef _KERNEL_USERMODE_H
#define _KERNEL_USERMODE_H

#include <kernel/types.h>

/* Map the embedded user payload, then iret into it at ring 3. */
bool usermode_spawn_demo(void);
bool usermode_ran(void);

/* Implemented in arch/x86/usermode.asm: forge an inter-privilege iret frame
 * and return into it. Never comes back. */
NORETURN void usermode_enter(vaddr_t entry, vaddr_t user_stack_top);

/* The ring-3 payload's entry point, linked into the .user section by
 * core/user_demo.c. Only ever called by the CPU after an IRET to ring 3. */
void user_demo_entry(void);

#endif /* _KERNEL_USERMODE_H */
