/* StratumOS - riscv64 link map for QEMU's `virt` machine.
 *
 * Loaded at 0x80000000, the base of RAM, because that is where the hart's
 * reset vector goes when there is no firmware: QEMU's `virt` board with
 * `-bios none` jumps straight to the bottom of RAM and the ELF entry point
 * is not consulted.
 *
 * This was the first thing to go wrong. The kernel was linked at 0x80200000
 * - which is where OpenSBI hands control to a payload, and therefore where
 * a kernel booted the normal way belongs - and QEMU started executing empty
 * memory at 0x80000000 instead. A perfectly well-formed ELF, the right entry
 * address in its header, and no output at all.
 *
 * Booting under firmware instead would mean arriving already in supervisor
 * mode, which would remove the machine-mode half of this port: the timer,
 * the delegation, and the mret transition. Owning the machine from reset is
 * the more interesting arrangement and the one that matches what the x86
 * side does, so the link address moved rather than the design.
 *
 * No higher-half split here, unlike the x86 link map. That is deliberate and
 * is explained in docs/PORTING.md: the kernel runs at its physical address,
 * because an Sv39 kernel reaches its own page tables through an identity map
 * rather than through x86's recursive page-directory window, and so it needs
 * that identity map to be where it is executing.
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY {
    RAM (rwxa) : ORIGIN = 0x80000000, LENGTH = 32M
}

SECTIONS
{
    . = 0x80000000;

    /* .text.boot first, so that the ELF entry point is the first thing in
     * the image and a loader that ignores the entry address still works. */
    .text : ALIGN(4096) {
        __text_start = .;
        *(.text.boot)
        *(.text .text.*)
        __text_end = .;
    } > RAM

    .rodata : ALIGN(4096) {
        __rodata_start = .;
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        __rodata_end = .;
    } > RAM

    .data : ALIGN(4096) {
        __data_start = .;
        *(.data .data.*)
        /* .sdata is the small-data section the RISC-V ABI puts globals in
         * when they are reachable from the gp register. It has no x86
         * counterpart, and leaving it out of the link map produces a kernel
         * whose globals are in a section nothing loads. */
        *(.sdata .sdata.*)
        __data_end = .;
    } > RAM

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        *(COMMON)
        /* The boot stack, with a page of nothing below it. There is no MMU
         * at the point the stack is first used, so this guard cannot fault -
         * it is a cushion, not a guard page, and calling it the latter would
         * be claiming a protection that is not there. */
        . = ALIGN(4096);
        *(.bss.stack)
        __bss_end = .;
    } > RAM

    __kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.riscv.attributes) }
}
