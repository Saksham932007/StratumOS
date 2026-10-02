/* StratumOS - kernel link script (higher-half).
 *
 * The kernel is *loaded* at 1 MiB physical but *linked* at 0xC0100000, so the
 * top gigabyte of every address space belongs to the kernel and the bottom
 * three are free for user processes. `AT()` is what expresses that split: a
 * section's VMA is where its code expects to be, its LMA is where the loader
 * must put it.
 *
 * The complication is that `_start` runs before paging exists, so it cannot be
 * at 0xC0100000 - nothing is mapped there yet. It therefore lives in its own
 * `.boot` section whose VMA equals its LMA, at 1 MiB, which is an address that
 * is valid both with paging off and (via the identity map it installs) just
 * after paging is turned on. It enables paging and jumps to the higher half;
 * everything from that point on runs at its linked address.
 *
 * This also means `e_entry` is a low physical address, which matters because
 * both loaders jump to it with paging off: GRUB, and our own stage 2.
 *
 * Layout:
 *                       LMA (loaded)        VMA (linked)
 *   .boot               0x00100000          0x00100000     paging off
 *   .text/.rodata/...   0x001xxxxx          0xC01xxxxx     paging on
 *
 * Section boundary symbols that real code depends on:
 *   __bss_start/__bss_end            _start clears the BSS
 *   __text_start/__text_end          backtrace validation, symbol lookup
 *   __kernel_start/__kernel_end      virtual extent of the image
 *   __kernel_phys_start/_phys_end    physical extent, for the frame allocator
 *   boot_page_directory              the page directory _start builds
 */

ENTRY(_start)

KERNEL_VIRT_BASE = 0xC0000000;
KERNEL_PHYS_BASE = 0x00100000;

SECTIONS
{
    . = KERNEL_PHYS_BASE;

    __kernel_phys_start = .;

    /* ---- the low window: runs with paging off --------------------------
     *
     * VMA == LMA. The Multiboot2 header must be here too, because it has to
     * appear within the first 32 KiB of the *file* and a loader searches the
     * file, not the address space.
     */
    .boot ALIGN(4K) : AT(ADDR(.boot))
    {
        __boot_start = .;
        KEEP(*(.multiboot))
        *(.text.boot)
        . = ALIGN(4K);
        /* The boot page directory and its one page table. They are written
         * before paging is on, so they need an address that is valid then -
         * which rules out the higher half and rules out .bss. Emitted as
         * zeroed bytes in the image rather than NOBITS so that their contents
         * do not depend on the loader having zeroed them. */
        *(.boot.pagetables)
        __boot_end = .;
    }

    /* ---- everything else: linked high, loaded low ---------------------- */
    . += KERNEL_VIRT_BASE;

    __kernel_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VIRT_BASE)
    {
        __text_start = .;
        *(.text .text.*)
        __text_end = .;
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VIRT_BASE)
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        __rodata_end = .;
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VIRT_BASE)
    {
        __data_start = .;
        *(.data .data.*)
        __data_end = .;
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VIRT_BASE)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(4);
        __bss_end = .;
    }

    . = ALIGN(4K);
    __kernel_end = .;
    __kernel_phys_end = . - KERNEL_VIRT_BASE;

    /DISCARD/ :
    {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.gnu.hash)
        *(.hash)
    }
}
