/* StratumOS - top-level kernel declarations. */
#ifndef _KERNEL_KERNEL_H
#define _KERNEL_KERNEL_H

#include <boot/bootinfo.h>

#include <kernel/layout.h>
#include <kernel/types.h>

#define STRATUM_NAME    "StratumOS"
#define STRATUM_VERSION "0.7.0"

/* Address-space constants and phys/virt translation live in
 * kernel/layout.h, which this header re-exports. */

/* Provided by the linker script. */
extern u8 __kernel_start[];
extern u8 __kernel_end[];
/* The image's *physical* extent. The frame allocator needs this, and it
 * cannot be derived from __kernel_start by subtraction because .boot is not
 * relocated. */
extern u8 __kernel_phys_start[];
extern u8 __kernel_phys_end[];
/* .boot: the low window that runs with paging off. VMA == LMA. */
extern u8 __boot_start[];
extern u8 __boot_end[];
extern u8 __text_start[], __text_end[];
extern u8 __rodata_start[], __rodata_end[];
extern u8 __data_start[], __data_end[];
extern u8 __bss_start[], __bss_end[];

/* The C entry point, called from arch/x86/boot.asm. */
void kmain(u32 magic, u32 info_addr);

const struct boot_params *kernel_boot_params(void);

/* Parsed from the boot command line. */
struct kernel_cmdline {
    /* Run the in-kernel test suites, then shut the machine down. */
    bool autotest;
    /* Run the microbenchmarks and a profile, then shut down. */
    bool autobench;
    /* Suppress VGA output; serial only. */
    bool quiet;
    /* Skip the ring-3 demonstration task. */
    bool no_usermode;
    /* Skip the background demo worker threads. */
    bool no_sched_demo;
    /* Initial log level, by name. */
    const char *loglevel;
};

const struct kernel_cmdline *kernel_cmdline(void);

#endif /* _KERNEL_KERNEL_H */
