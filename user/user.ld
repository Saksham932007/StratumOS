/* StratumOS - link script for user programs.
 *
 * Linked at USER_IMAGE_BASE (4 MiB), which is a *user* address: below
 * KERNEL_VIRT_BASE, and far enough above zero that the null page stays
 * unmapped and a NULL dereference still faults.
 *
 * This is the point of building the program separately rather than as a
 * section of the kernel. A program linked at a kernel address and then mapped
 * somewhere else would have every absolute reference - every string literal -
 * pointing into the kernel's half, where ring 3 cannot read.
 */

ENTRY(_start)

USER_IMAGE_BASE = 0x00400000;

SECTIONS
{
    . = USER_IMAGE_BASE;

    .text ALIGN(4K) : { *(.text .text.*) }
    .rodata ALIGN(4K) : { *(.rodata .rodata.*) }
    .data ALIGN(4K) : { *(.data .data.*) }
    .bss ALIGN(4K) : { *(COMMON) *(.bss .bss.*) }

    . = ALIGN(4K);
    __user_image_end = .;

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame*) }
}
