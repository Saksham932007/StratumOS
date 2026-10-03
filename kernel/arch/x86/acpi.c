/* StratumOS - ACPI table discovery.
 *
 * See arch/acpi.h for the scope: four facts about the processors and the
 * interrupt controllers, and nothing else.
 *
 * The walk is RSDP -> RSDT (or XSDT) -> MADT. Each step is a pointer out of
 * firmware memory into more firmware memory, and each one is checked, because
 * the lengths in these tables are what the loops below iterate over.
 */
#define LOG_TAG "acpi"

#include <arch/acpi.h>

#include <kernel/kernel.h>
#include <kernel/layout.h>
#include <kernel/log.h>
#include <kernel/string.h>

#include <mm/vmm.h>

/* The RSDP lives in one of two places, and the firmware chooses. */
#define EBDA_POINTER_ADDR 0x40E /* a segment, so << 4 for the address   */
#define BIOS_AREA_START   0xE0000
#define BIOS_AREA_END     0x100000

#define RSDP_SIGNATURE    "RSD PTR "

struct acpi_rsdp {
    char signature[8];
    u8 checksum; /* over the first 20 bytes                  */
    char oem_id[6];
    u8 revision;
    u32 rsdt_address;
    /* ACPI 2.0 and later only: */
    u32 length;
    u64 xsdt_address;
    u8 extended_checksum; /* over `length` bytes                    */
    u8 reserved[3];
} PACKED;

_Static_assert(sizeof(struct acpi_rsdp) == 36, "the ACPI 2.0 RSDP is 36 bytes");
_Static_assert(sizeof(struct acpi_sdt_header) == 36,
               "an ACPI table header is 36 bytes");

struct madt_entry_header {
    u8 type;
    u8 length;
} PACKED;

struct madt_local_apic {
    struct madt_entry_header hdr;
    u8 acpi_id;
    u8 apic_id;
    u32 flags;
} PACKED;

struct madt_io_apic {
    struct madt_entry_header hdr;
    u8 id;
    u8 reserved;
    u32 address;
    u32 gsi_base;
} PACKED;

struct madt_override {
    struct madt_entry_header hdr;
    u8 bus;
    u8 source;
    u32 gsi;
    u16 flags;
} PACKED;

struct madt_lapic_override {
    struct madt_entry_header hdr;
    u16 reserved;
    u64 address;
} PACKED;

static struct acpi_info info;

/* The root table, mapped. Kept so that acpi_find_table() can be called after
 * init without re-finding the RSDP. */
static const struct acpi_sdt_header *root_table;
static bool root_is_xsdt;

const struct acpi_info *acpi_get_info(void)
{
    return &info;
}

/* ------------------------------------------------------------------------- */

/* Every ACPI table carries a checksum that is simply the sum of its bytes,
 * truncated to 8 bits, and that sum must be zero. It is the only thing
 * standing between a kernel and firmware that handed it a table from the
 * wrong machine. */
static bool checksum_ok(const void *data, size_t length)
{
    const u8 *p = data;
    u8 sum = 0;

    for (size_t i = 0; i < length; i++)
        sum = (u8)(sum + p[i]);

    return sum == 0;
}

/* Search a physical range, which must be inside the linear map, for the RSDP.
 * The signature is 16-byte aligned by specification. */
static const struct acpi_rsdp *scan_for_rsdp(paddr_t start, paddr_t end)
{
    for (paddr_t at = start; at + sizeof(struct acpi_rsdp) <= end; at += 16) {
        const struct acpi_rsdp *candidate = phys_to_virt(at);

        if (memcmp(candidate->signature, RSDP_SIGNATURE, 8) != 0)
            continue;

        /* The signature can occur by accident; the checksum is what makes it
         * a finding. Only the first 20 bytes are covered, because that is all
         * ACPI 1.0 defined and the field was never redefined. */
        if (!checksum_ok(candidate, 20)) {
            pr_warn("an RSDP signature at %p has a bad checksum; ignoring it",
                    (void *)at);
            continue;
        }

        /* The extended checksum covers `length` bytes - and `length` is a
         * number the firmware wrote, used here as a read length. An
         * implausible value would walk off the end of the linear map and
         * panic the kernel from firmware data before a single field had been
         * looked at.
         *
         * A fuzz target found this immediately: AddressSanitizer reported a
         * read past the end of its physical-memory buffer, from a checksum
         * over a table claiming to be megabytes long. The specification fixes
         * the ACPI 2.0 RSDP at 36 bytes, so anything outside a narrow range
         * is not an RSDP. */
        if (candidate->revision >= 2 &&
            (candidate->length < sizeof(*candidate) ||
             candidate->length > 64)) {
            pr_warn("the RSDP at %p claims a length of %u; ignoring its "
                    "64-bit fields",
                    (void *)at, candidate->length);
        } else if (candidate->revision >= 2 &&
                   !checksum_ok(candidate, candidate->length)) {
            pr_warn("the RSDP at %p has a bad extended checksum; using its "
                    "32-bit fields only",
                    (void *)at);
            /* Not fatal: the revision-0 half of the structure checksummed
             * correctly, so the RSDT pointer is still trustworthy. */
        }

        pr_debug("RSDP at phys %p, revision %u", (void *)at,
                 candidate->revision);
        return candidate;
    }

    return NULL;
}

static const struct acpi_rsdp *find_rsdp(void)
{
    /* The firmware may put it in the first kilobyte of the Extended BIOS Data
     * Area, whose segment is at 0x40E. */
    const u16 *ebda_segment = phys_to_virt(EBDA_POINTER_ADDR);
    paddr_t ebda = (paddr_t)*ebda_segment << 4;

    if (ebda >= 0x80000 && ebda < 0xA0000) {
        const struct acpi_rsdp *found = scan_for_rsdp(ebda, ebda + KIB);

        if (found)
            return found;
    } else if (ebda) {
        pr_debug("the EBDA pointer says %p, which is not a plausible EBDA",
                 (void *)ebda);
    }

    /* Otherwise it is in the BIOS read-only area. */
    return scan_for_rsdp(BIOS_AREA_START, BIOS_AREA_END);
}

/* ------------------------------------------------------------------------- */

/* Map a table: read its header to learn the length, then map the whole thing.
 *
 * Two mappings rather than one because the length is inside the data being
 * mapped - and mapping a fixed guess instead would either waste the window or
 * truncate a large table. The first mapping is a page, which always covers a
 * 36-byte header. */
static const struct acpi_sdt_header *map_table(u64 phys)
{
    if (phys == 0 || phys > 0xFFFFFFFFull) {
        pr_err("a table pointer of %llu is not a 32-bit physical address",
               phys);
        return NULL;
    }

    const struct acpi_sdt_header *peek =
        vmm_map_mmio((paddr_t)phys, sizeof(*peek), false);

    if (!peek)
        return NULL;

    u32 length = peek->length;

    if (length < sizeof(*peek) || length > 1 * MIB) {
        pr_err("a table at %p claims a length of %u, which is implausible",
               (void *)(u32)phys, length);
        return NULL;
    }

    if (length <= PAGE_SIZE - ((u32)phys & (PAGE_SIZE - 1))) {
        /* Already entirely inside the page just mapped. */
        if (!checksum_ok(peek, length)) {
            pr_err("the table at %p has a bad checksum", (void *)(u32)phys);
            return NULL;
        }
        return peek;
    }

    const struct acpi_sdt_header *full =
        vmm_map_mmio((paddr_t)phys, length, false);

    if (!full)
        return NULL;

    if (!checksum_ok(full, length)) {
        pr_err("the table at %p has a bad checksum", (void *)(u32)phys);
        return NULL;
    }

    return full;
}

static u32 root_entry_count(void)
{
    if (!root_table)
        return 0;

    u32 payload = root_table->length - sizeof(*root_table);

    return payload / (root_is_xsdt ? 8 : 4);
}

static u64 root_entry(u32 index)
{
    const u8 *payload = (const u8 *)root_table + sizeof(*root_table);

    if (root_is_xsdt) {
        u64 value;

        /* Unaligned by construction: the XSDT's 64-bit entries start 36 bytes
         * into the table. memcpy is the portable way to say so. */
        memcpy(&value, payload + (size_t)index * 8, sizeof(value));
        return value;
    }

    u32 value;

    memcpy(&value, payload + (size_t)index * 4, sizeof(value));
    return value;
}

const struct acpi_sdt_header *acpi_find_table(const char *signature)
{
    if (!root_table || !signature)
        return NULL;

    u32 count = root_entry_count();

    for (u32 i = 0; i < count; i++) {
        const struct acpi_sdt_header *table = map_table(root_entry(i));

        if (table && memcmp(table->signature, signature, 4) == 0)
            return table;
    }

    return NULL;
}

/* ------------------------------------------------------------------------- */

static void parse_madt(const struct acpi_sdt_header *madt)
{
    /* The MADT's own fields sit between the header and the entry list. */
    const u8 *base = (const u8 *)madt;
    u32 lapic_addr;
    u32 flags;

    memcpy(&lapic_addr, base + 36, sizeof(lapic_addr));
    memcpy(&flags, base + 40, sizeof(flags));

    info.local_apic_phys = lapic_addr;
    info.pcat_compat = (flags & 1) != 0;

    u32 offset = 44;

    while (offset + sizeof(struct madt_entry_header) <= madt->length) {
        struct madt_entry_header hdr;

        memcpy(&hdr, base + offset, sizeof(hdr));

        /* A zero-length entry would loop forever, and a length that runs past
         * the table would read whatever follows it. Both are firmware bugs,
         * and both have to be survivable. */
        if (hdr.length < sizeof(hdr) || offset + hdr.length > madt->length) {
            pr_err("MADT entry at +%u has length %u; stopping the walk", offset,
                   hdr.length);
            break;
        }

        switch (hdr.type) {
        case MADT_LOCAL_APIC: {
            struct madt_local_apic e;

            if (hdr.length < sizeof(e))
                break;

            memcpy(&e, base + offset, sizeof(e));
            info.cpus_reported++;

            if (info.cpu_count < ARRAY_SIZE(info.cpus)) {
                struct acpi_cpu *cpu = &info.cpus[info.cpu_count++];

                cpu->acpi_id = e.acpi_id;
                cpu->apic_id = e.apic_id;
                cpu->enabled = (e.flags & MADT_CPU_ENABLED) != 0;
                cpu->online_capable = (e.flags & MADT_CPU_ONLINE_CAPABLE) != 0;
            }
            break;
        }

        case MADT_IO_APIC: {
            struct madt_io_apic e;

            if (hdr.length < sizeof(e))
                break;

            memcpy(&e, base + offset, sizeof(e));

            if (info.ioapic_count < ARRAY_SIZE(info.ioapics)) {
                struct acpi_ioapic *io = &info.ioapics[info.ioapic_count++];

                io->id = e.id;
                io->address = e.address;
                io->gsi_base = e.gsi_base;
            }
            break;
        }

        case MADT_INTERRUPT_OVERRIDE: {
            struct madt_override e;

            if (hdr.length < sizeof(e))
                break;

            memcpy(&e, base + offset, sizeof(e));

            if (info.iso_count < ARRAY_SIZE(info.isos)) {
                struct acpi_iso *iso = &info.isos[info.iso_count++];

                iso->bus = e.bus;
                iso->source = e.source;
                iso->gsi = e.gsi;
                iso->flags = e.flags;
            }
            break;
        }

        case MADT_LOCAL_APIC_OVERRIDE: {
            struct madt_lapic_override e;

            if (hdr.length < sizeof(e))
                break;

            memcpy(&e, base + offset, sizeof(e));

            /* A 64-bit address for the local APIC supersedes the 32-bit one
             * in the MADT header. On a 32-bit kernel an address above 4 GiB
             * is unusable, and saying so beats truncating it. */
            if (e.address > 0xFFFFFFFFull)
                pr_err("the MADT puts the local APIC at %llu, above 4 GiB",
                       e.address);
            else
                info.local_apic_phys = e.address;
            break;
        }

        default:
            break;
        }

        offset += hdr.length;
    }

    info.madt_found = true;

    pr_info("MADT: %u processor(s), %u I/O APIC(s), %u interrupt override(s), "
            "local APIC at %p%s",
            info.cpus_reported, info.ioapic_count, info.iso_count,
            (void *)(u32)info.local_apic_phys,
            info.pcat_compat ? ", 8259 PICs present" : "");

    for (u32 i = 0; i < info.cpu_count; i++)
        pr_debug("  cpu: ACPI id %u, APIC id %u, %s", info.cpus[i].acpi_id,
                 info.cpus[i].apic_id,
                 info.cpus[i].enabled          ? "enabled"
                 : info.cpus[i].online_capable ? "online-capable"
                                               : "disabled");

    for (u32 i = 0; i < info.iso_count; i++)
        pr_debug("  override: ISA IRQ %u arrives as GSI %u",
                 info.isos[i].source, info.isos[i].gsi);
}

void acpi_init(void)
{
    memset(&info, 0, sizeof(info));
    root_table = NULL;

    const struct acpi_rsdp *rsdp = find_rsdp();

    if (!rsdp) {
        pr_info("no RSDP found; this machine publishes no ACPI tables");
        return;
    }

    memcpy(info.oem_id, rsdp->oem_id, 6);
    info.oem_id[6] = '\0';
    info.revision = rsdp->revision;

    /* Prefer the XSDT when the firmware offers one. Its entries are 64-bit,
     * which a 32-bit kernel cannot always use - but the tables themselves
     * are below 4 GiB on every machine that also publishes an RSDT, and
     * map_table() refuses anything higher rather than truncating. */
    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        root_table = map_table(rsdp->xsdt_address);
        root_is_xsdt = root_table != NULL;
    }

    if (!root_table) {
        root_table = map_table(rsdp->rsdt_address);
        root_is_xsdt = false;
    }

    if (!root_table) {
        pr_err("the RSDP points at a root table that will not parse");
        return;
    }

    info.used_xsdt = root_is_xsdt;
    info.tables_seen = root_entry_count();
    info.available = true;

    pr_info("ACPI %u.0 from \"%s\": %s with %u table(s)",
            rsdp->revision >= 2 ? 2 : 1, info.oem_id,
            root_is_xsdt ? "XSDT" : "RSDT", info.tables_seen);

    const struct acpi_sdt_header *madt = acpi_find_table("APIC");

    if (madt)
        parse_madt(madt);
    else
        pr_warn("no MADT; there is no way to find the other processors");
}
