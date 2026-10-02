/* StratumOS - kernel link script.
 *
 * The kernel is loaded at the 1 MiB mark, the conventional home for a 32-bit
 * x86 kernel: it is above the BIOS/VGA holes in low memory and reachable only
 * once the A20 gate is open, which both boot paths guarantee.
 *
 * .multiboot comes first and is deliberately not aligned away from the start
 * of the image, because the Multiboot2 specification requires the header to
 * appear within the first 32 KiB of the file. If the linker ever moved it, GRUB
 * would refuse the kernel with "no multiboot header found" and nothing else
 * would explain why.
 *
 * Section boundary symbols are exported because real code needs them:
 *   __bss_start/__bss_end   - _start zeroes the BSS itself
 *   __text_start/__text_end - the backtrace walker validates return addresses
 *   __user_start/__user_end - the pages re-mapped as ring-3 accessible
 *   __kernel_start/__kernel_end - the PMM reserves the image
 */

ENTRY(_start)

KERNEL_PHYS_BASE = 1M;

SECTIONS
{
    . = KERNEL_PHYS_BASE;

    __kernel_start = .;

    /* Must stay first: the Multiboot2 header has to be in the first 32 KiB. */
    .multiboot : ALIGN(8)
    {
        KEEP(*(.multiboot))
    }

    .text : ALIGN(4K)
    {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    }

    .rodata : ALIGN(4K)
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    .data : ALIGN(4K)
    {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    /* The ring-3 payload gets its own page-aligned region so that exactly
     * these pages - and nothing else - can be marked user-accessible. Mixing
     * it into .text would mean exposing the whole kernel to userspace. */
    .user : ALIGN(4K)
    {
        __user_start = .;
        *(.user.text)
        *(.user.rodata)
        . = ALIGN(4K);
        __user_end = .;
    }

    .bss : ALIGN(4K)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(4);
        __bss_end = .;
    }

    __kernel_end = ALIGN(4K);

    /* Debug info is useful in the ELF on disk; these are not. Dropping them
     * keeps `objdump -h` readable and the loadable image minimal. */
    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.gnu.hash)
        *(.hash)
    }
}
