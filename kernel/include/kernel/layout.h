/* StratumOS - address-space constants.
 *
 * Separated from kernel.h so that low-level headers (the VGA driver, the VMM)
 * can use them without pulling in the whole kernel interface.
 *
 *   0x00000000              the null page, never mapped
 *   0x00400000              a user process's image
 *   ...                     user heap and stacks
 *   0xC0000000              KERNEL_VIRT_BASE: the kernel, and its linear map
 *                           of the first VMM_LINEAR_SIZE of physical memory
 *   0xD0000000              the kernel heap window
 *   0xFFC00000              the recursive page-table window
 */
#ifndef _KERNEL_LAYOUT_H
#define _KERNEL_LAYOUT_H

#include <kernel/types.h>

/* The kernel occupies the top gigabyte of every address space; the bottom
 * three belong to user processes. It is loaded at 1 MiB physical and linked
 * KERNEL_VIRT_BASE higher - see linker/kernel.ld for how AT() expresses that.
 *
 * The same offset doubles as a linear map of low physical memory: physical
 * address P is reachable at P + KERNEL_VIRT_BASE for the first
 * VMM_LINEAR_SIZE bytes. That is how the kernel reaches the VGA framebuffer,
 * the loader's tables and its own page frames once the identity mapping used
 * during boot has been removed. */
#define KERNEL_VIRT_BASE 0xC0000000u
#define KERNEL_PHYS_BASE 0x00100000u

/* Translate between a physical address and its address in the linear map.
 *
 * Valid only for physical addresses below VMM_LINEAR_SIZE, and for virtual
 * addresses inside that window - which covers the kernel image, the frame
 * bitmap, the VGA framebuffer and anything a loader left in low memory. A heap
 * pointer, a user pointer or a page-table frame above the window is NOT in the
 * linear map; use vmm_translate() for those. */
#ifdef STRATUM_FUZZING

/* On a host there is no linear map, and a physical address is an offset into
 * the buffer the fuzzer supplied. Without this, the ACPI table walk - whose
 * whole job is following physical pointers - dereferences
 * `p + 0xC0000000` and segfaults before it reaches a single check. */
void *phys_to_virt_shim(paddr_t p);

static inline void *phys_to_virt(paddr_t p)
{
    return phys_to_virt_shim(p);
}

#else

static inline void *phys_to_virt(paddr_t p)
{
    return (void *)(p + KERNEL_VIRT_BASE);
}

#endif /* STRATUM_FUZZING */

static inline paddr_t virt_to_phys(const void *v)
{
    /* Through `uptr`, not `u32`. Casting a pointer to a 32-bit integer is
     * correct on i386 and silently truncating on anything wider, and it was
     * one of the places the RISC-V port's first build caught. */
    return (paddr_t)((uptr)v - KERNEL_VIRT_BASE);
}

static inline bool is_kernel_address(vaddr_t addr)
{
    return addr >= KERNEL_VIRT_BASE;
}

#endif /* _KERNEL_LAYOUT_H */
