/* StratumOS - boot protocol normalisation.
 *
 * Two loaders, one kernel. This file is the only place that knows the
 * difference between them; everything downstream reads struct boot_params.
 *
 * Both protocols are validated rather than trusted. A loader that hands over
 * a truncated memory map or a bogus pointer should produce a clear complaint
 * at boot, not a page fault in the physical allocator twenty frames later.
 */
#define LOG_TAG "boot"

#include <boot/bootinfo.h>
#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/string.h>
#include <mm/pmm.h>

/* ---- Multiboot2 tag definitions (spec section 3.1) --------------------- */

#define MB_TAG_END            0
#define MB_TAG_CMDLINE        1
#define MB_TAG_LOADER_NAME    2
#define MB_TAG_BASIC_MEMINFO  4
#define MB_TAG_BOOTDEV        5
#define MB_TAG_MMAP           6
#define MB_TAG_FRAMEBUFFER    8

struct mb_tag {
    u32 type;
    u32 size;
} PACKED;

struct mb_tag_string {
    u32 type, size;
    char string[1];
} PACKED;

struct mb_tag_basic_meminfo {
    u32 type, size;
    u32 mem_lower, mem_upper;
} PACKED;

struct mb_tag_bootdev {
    u32 type, size;
    u32 biosdev, slice, part;
} PACKED;

struct mb_mmap_entry {
    u64 addr;
    u64 len;
    u32 type;
    u32 zero;
} PACKED;

struct mb_tag_mmap {
    u32 type, size;
    u32 entry_size;
    u32 entry_version;
    struct mb_mmap_entry entries[1];
} PACKED;

/* ------------------------------------------------------------------------ */

static struct boot_params params;

const char *mem_type_name(u32 type)
{
    switch (type) {
    case MEM_USABLE:       return "usable";
    case MEM_RESERVED:     return "reserved";
    case MEM_ACPI_RECLAIM: return "ACPI reclaimable";
    case MEM_ACPI_NVS:     return "ACPI NVS";
    case MEM_BAD:          return "bad";
    default:               return "unknown";
    }
}

static void region_add(u64 base, u64 length, u32 type)
{
    if (length == 0)
        return;

    if (params.region_count >= BOOT_MAX_REGIONS) {
        pr_warn("memory map truncated at %u entries", BOOT_MAX_REGIONS);
        return;
    }

    struct mem_region *r = &params.regions[params.region_count++];
    r->base = base;
    r->length = length;
    r->type = type;

    if (type == MEM_USABLE) {
        params.mem_usable += length;
        if (base + length > params.mem_highest)
            params.mem_highest = base + length;
    }
}

/* ---- StratumOS stage 2 ------------------------------------------------- */

static bool parse_stratum(u32 info_addr)
{
    const struct stratum_boot_info *bi =
        (const struct stratum_boot_info *)info_addr;

    if (!info_addr || bi->magic != STRATUM_BOOT_MAGIC) {
        pr_err("stage2 boot info at %p has bad magic %08x",
               (void *)info_addr, bi ? bi->magic : 0);
        return false;
    }

    params.protocol = BOOT_PROTO_STRATUM;
    params.protocol_name = "StratumOS native (ELF handoff)";
    params.loader_name = bi->loader_name ? (const char *)bi->loader_name
                                         : "stage2";
    params.cmdline = bi->cmdline ? (const char *)bi->cmdline : "";
    params.boot_device = bi->boot_drive;

    if (bi->flags & BI_FLAG_E820) {
        const struct e820_entry *e = (const struct e820_entry *)bi->e820_addr;

        for (u32 i = 0; i < bi->e820_count && i < BOOT_MAX_REGIONS; i++)
            region_add(e[i].base, e[i].length, e[i].type);

        pr_debug("E820 supplied %u entries", bi->e820_count);
    } else if (bi->flags & BI_FLAG_E801) {
        /* No E820. Synthesise a map from the coarse numbers: 0-640 KiB plus
         * whatever E801 reported above 1 MiB. Crude, but it is what the
         * firmware gave us and it beats refusing to boot. */
        pr_warn("no E820 map; falling back to E801 (%u KiB low, %u blocks high)",
                bi->mem_lower, bi->mem_upper);
        region_add(0, 640 * KIB, MEM_USABLE);
        if (bi->mem_lower)
            region_add(1 * MIB, (u64)bi->mem_lower * KIB, MEM_USABLE);
        if (bi->mem_upper)
            region_add(16 * MIB, (u64)bi->mem_upper * 64 * KIB, MEM_USABLE);
    } else {
        pr_err("stage2 reported no memory information at all");
        return false;
    }

    /* Point at stage 2's own structures - the boot info block at 0x4000 and
     * the E820 array at 0x5000 - which the kernel keeps reading from.
     *
     * This deliberately does NOT claim all of low memory. The PMM reserves
     * the whole first mebibyte unconditionally anyway, and naming a range
     * that starts at 0 would make the VMM think a loader structure lives in
     * the null page and map it, costing us NULL-dereference detection. */
    params.reserved_lo = PAGE_TRUNC(bi->e820_addr ? MIN(info_addr,
                                                        bi->e820_addr)
                                                  : info_addr);
    params.reserved_hi = PAGE_ALIGN(MAX(info_addr + sizeof(*bi),
                                        bi->e820_addr +
                                        bi->e820_count *
                                        sizeof(struct e820_entry)));

    return true;
}

/* ---- Multiboot2 -------------------------------------------------------- */

static bool parse_multiboot2(u32 info_addr)
{
    if (!info_addr || (info_addr & 7)) {
        pr_err("multiboot2 info pointer %p is null or misaligned",
               (void *)info_addr);
        return false;
    }

    params.protocol = BOOT_PROTO_MULTIBOOT2;
    params.protocol_name = "Multiboot2";
    params.loader_name = "unknown";
    params.cmdline = "";

    /* The info block starts with total_size and a reserved word, then a
     * sequence of 8-byte-aligned tags terminated by a type-0 tag. */
    u32 total_size = *(const u32 *)info_addr;
    u32 offset = 8;

    if (total_size < 8 || total_size > 64 * KIB) {
        pr_err("multiboot2 total_size %u is implausible", total_size);
        return false;
    }

    bool saw_mmap = false;
    u32 mem_lower = 0, mem_upper = 0;

    while (offset < total_size) {
        const struct mb_tag *tag = (const struct mb_tag *)(info_addr + offset);

        if (tag->size < 8) {
            pr_err("multiboot2 tag at +%u has size %u", offset, tag->size);
            return false;
        }

        switch (tag->type) {
        case MB_TAG_END:
            offset = total_size;
            continue;

        case MB_TAG_CMDLINE:
            params.cmdline = ((const struct mb_tag_string *)tag)->string;
            break;

        case MB_TAG_LOADER_NAME:
            params.loader_name = ((const struct mb_tag_string *)tag)->string;
            break;

        case MB_TAG_BASIC_MEMINFO: {
            const struct mb_tag_basic_meminfo *m =
                (const struct mb_tag_basic_meminfo *)tag;
            mem_lower = m->mem_lower;
            mem_upper = m->mem_upper;
            break;
        }

        case MB_TAG_BOOTDEV:
            params.boot_device = ((const struct mb_tag_bootdev *)tag)->biosdev;
            break;

        case MB_TAG_MMAP: {
            const struct mb_tag_mmap *mm = (const struct mb_tag_mmap *)tag;

            if (mm->entry_size == 0) {
                pr_err("multiboot2 mmap entry_size is zero");
                break;
            }

            u32 count = (mm->size - sizeof(struct mb_tag_mmap) +
                         sizeof(struct mb_mmap_entry)) / mm->entry_size;

            for (u32 i = 0; i < count; i++) {
                const struct mb_mmap_entry *e =
                    (const struct mb_mmap_entry *)((const u8 *)mm->entries +
                                                   i * mm->entry_size);
                region_add(e->addr, e->len, e->type);
            }

            saw_mmap = true;
            pr_debug("multiboot2 mmap supplied %u entries", count);
            break;
        }

        default:
            break;
        }

        /* Tags are padded up to the next 8-byte boundary. */
        offset += ALIGN_UP(tag->size, 8);
    }

    if (!saw_mmap) {
        if (!mem_upper) {
            pr_err("multiboot2 provided neither a memory map nor meminfo");
            return false;
        }
        pr_warn("no multiboot2 mmap tag; using basic meminfo");
        region_add(0, (u64)mem_lower * KIB, MEM_USABLE);
        region_add(1 * MIB, (u64)mem_upper * KIB, MEM_USABLE);
    }

    /* Keep the loader's own info block out of the allocator's reach: we read
     * cmdline and loader_name straight out of it for the lifetime of the
     * kernel. */
    params.reserved_lo = PAGE_TRUNC(info_addr);
    params.reserved_hi = PAGE_ALIGN(info_addr + total_size);

    return true;
}

/* ------------------------------------------------------------------------ */

const struct boot_params *boot_parse(u32 magic, u32 info_addr)
{
    bool ok;

    memset(&params, 0, sizeof(params));
    params.protocol = BOOT_PROTO_UNKNOWN;
    params.protocol_name = "unknown";
    params.loader_name = "unknown";
    params.cmdline = "";

    switch (magic) {
    case STRATUM_BOOT_MAGIC:
        ok = parse_stratum(info_addr);
        break;
    case MULTIBOOT2_BOOTLOADER_MAGIC:
        ok = parse_multiboot2(info_addr);
        break;
    default:
        pr_err("unrecognised boot magic %08x - was the kernel started by a "
               "supported loader?", magic);
        ok = false;
        break;
    }

    if (!ok)
        return NULL;

    pr_info("booted by %s via %s", params.loader_name, params.protocol_name);
    if (params.cmdline && *params.cmdline)
        pr_info("command line: \"%s\"", params.cmdline);

    return &params;
}
