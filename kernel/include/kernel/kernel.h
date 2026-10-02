/* StratumOS - top-level kernel declarations. */
#ifndef _KERNEL_KERNEL_H
#define _KERNEL_KERNEL_H

#include <boot/bootinfo.h>
#include <kernel/types.h>

#define STRATUM_NAME    "StratumOS"
#define STRATUM_VERSION "0.3.0"

/* Provided by the linker script. */
extern u8 __kernel_start[];
extern u8 __kernel_end[];
extern u8 __text_start[], __text_end[];
extern u8 __rodata_start[], __rodata_end[];
extern u8 __data_start[], __data_end[];
extern u8 __bss_start[], __bss_end[];

/* The C entry point, called from arch/x86/boot.asm. */
void kmain(u32 magic, u32 info_addr);

const struct boot_params *kernel_boot_params(void);

/* Parsed from the boot command line. */
struct kernel_cmdline {
    bool autotest;      /* run self-tests and exit instead of starting a shell */
    bool quiet;         /* suppress VGA output                                 */
    bool no_usermode;   /* skip the ring-3 demo                                */
    bool no_sched_demo; /* skip the demo worker threads                        */
    const char *loglevel;
};

const struct kernel_cmdline *kernel_cmdline(void);

#endif /* _KERNEL_KERNEL_H */
