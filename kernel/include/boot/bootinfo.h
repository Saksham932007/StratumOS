/* StratumOS - boot protocol abstraction.
 *
 * The kernel supports two ways of being loaded and refuses to care which one
 * was used past the first few instructions of C:
 *
 *   * Multiboot2 (GRUB, qemu -kernel)  -- EAX = 0x36d76289, EBX -> tag list
 *   * StratumOS stage 2                -- EAX = 0x53545241, EBX -> struct below
 *
 * boot_parse() normalises either one into a single struct boot_params, which
 * is all the rest of the kernel ever sees. Adding a third protocol means
 * adding one parser here and nothing else.
 */
#ifndef _BOOT_BOOTINFO_H
#define _BOOT_BOOTINFO_H

#include <kernel/types.h>

#define MULTIBOOT2_BOOTLOADER_MAGIC 0x36d76289u
#define STRATUM_BOOT_MAGIC          0x53545241u /* 'STRA' */

/* ---- what stage 2 hands us (mirrors the offsets in boot/stage2.asm) ---- */

#define BI_FLAG_E820 0x01
#define BI_FLAG_E801 0x02
#define BI_FLAG_LBA  0x04

struct stratum_boot_info {
    u32 magic;
    u32 version;
    u32 flags;
    u32 e820_count;
    u32 e820_addr;
    u32 boot_drive;
    u32 kernel_stage;
    u32 kernel_sectors;
    u32 loader_name;
    u32 cmdline;
    u32 mem_lower;  /* KiB between 1 MiB and 16 MiB  */
    u32 mem_upper;  /* 64 KiB blocks above 16 MiB    */
} PACKED;

struct e820_entry {
    u64 base;
    u64 length;
    u32 type;
    u32 acpi_attrs;
} PACKED;

/* ---- normalised view ---------------------------------------------------- */

enum mem_type {
    MEM_USABLE        = 1,
    MEM_RESERVED      = 2,
    MEM_ACPI_RECLAIM  = 3,
    MEM_ACPI_NVS      = 4,
    MEM_BAD           = 5,
};

struct mem_region {
    u64 base;
    u64 length;
    u32 type;
};

#define BOOT_MAX_REGIONS 48

enum boot_protocol {
    BOOT_PROTO_UNKNOWN = 0,
    BOOT_PROTO_STRATUM,
    BOOT_PROTO_MULTIBOOT2,
};

struct boot_params {
    enum boot_protocol protocol;
    const char *protocol_name;
    const char *loader_name;
    const char *cmdline;

    struct mem_region regions[BOOT_MAX_REGIONS];
    u32 region_count;

    u64 mem_usable;  /* total bytes of usable RAM reported     */
    u64 mem_highest; /* highest usable address seen            */
    u32 boot_device;

    /* Where the loader itself lives, so the PMM can avoid clobbering any
     * structure we are still reading from. */
    paddr_t reserved_lo;
    paddr_t reserved_hi;
};

const struct boot_params *boot_parse(u32 magic, u32 info_addr);
const char *mem_type_name(u32 type);

#endif /* _BOOT_BOOTINFO_H */
