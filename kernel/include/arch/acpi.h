/* StratumOS - ACPI table discovery, for the bits SMP needs.
 *
 * ACPI is enormous. This reads four things out of it and ignores the rest:
 * how many processors there are, their local APIC IDs, where the local APIC's
 * registers live, and where the I/O APIC is. That is what bringing up a
 * second CPU requires; power management, device enumeration and the AML
 * bytecode interpreter are a different project.
 *
 * Why ACPI at all rather than the older MP Floating Pointer Structure: QEMU
 * and every machine built this century publish ACPI, many publish no MP table
 * at all, and the MADT is simpler to parse than the MP configuration table.
 *
 * Everything here is validated. The tables are firmware output: their lengths
 * are what the kernel will index by, their entry counts are what it will loop
 * over, and a checksum exists in each one precisely because the firmware's
 * authors did not trust themselves either.
 */
#ifndef _ARCH_ACPI_H
#define _ARCH_ACPI_H

#include <kernel/types.h>

/* Every table after the RSDP starts with this. */
struct acpi_sdt_header {
    char signature[4];
    u32 length;
    u8 revision;
    u8 checksum;
    char oem_id[6];
    char oem_table_id[8];
    u32 oem_revision;
    u32 creator_id;
    u32 creator_revision;
} PACKED;

/* MADT entry types. */
#define MADT_LOCAL_APIC          0
#define MADT_IO_APIC             1
#define MADT_INTERRUPT_OVERRIDE  2
#define MADT_NMI_SOURCE          3
#define MADT_LOCAL_APIC_NMI      4
#define MADT_LOCAL_APIC_OVERRIDE 5
#define MADT_LOCAL_X2APIC        9

/* MADT_LOCAL_APIC flags */
#define MADT_CPU_ENABLED         0x01
#define MADT_CPU_ONLINE_CAPABLE  0x02

#define ACPI_MAX_CPUS            8
#define ACPI_MAX_IOAPICS         2
#define ACPI_MAX_ISO             16

struct acpi_cpu {
    u8 acpi_id;
    u8 apic_id;
    bool enabled;        /* usable now                       */
    bool online_capable; /* could be hot-plugged in          */
};

struct acpi_ioapic {
    u8 id;
    u32 address;
    u32 gsi_base;
};

/* An interrupt source override: the MADT saying that an ISA IRQ does not
 * arrive on the I/O APIC input you would expect. IRQ 0 showing up as global
 * system interrupt 2 is the usual one, and a kernel that assumed otherwise
 * would lose its timer. */
struct acpi_iso {
    u8 bus;
    u8 source; /* the ISA IRQ number     */
    u32 gsi;   /* where it actually lands */
    u16 flags;
};

struct acpi_info {
    bool available; /* an RSDP was found and its tables parsed    */
    bool madt_found;
    u8 revision; /* RSDP revision: 0 means RSDT, 2+ means XSDT */
    bool used_xsdt;
    u32 tables_seen;
    u64 local_apic_phys;
    bool pcat_compat; /* the MADT says there are 8259 PICs to mask  */

    u32 cpu_count;
    struct acpi_cpu cpus[ACPI_MAX_CPUS];
    u32 cpus_reported; /* including any that did not fit the array   */

    u32 ioapic_count;
    struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];

    u32 iso_count;
    struct acpi_iso isos[ACPI_MAX_ISO];

    char oem_id[7];
};

/* Find the RSDP, walk the root table, and parse the MADT. Needs the VMM,
 * because the tables usually sit outside the linear map. */
void acpi_init(void);

const struct acpi_info *acpi_get_info(void);

/* Map and return a table by its four-character signature, or NULL. The
 * mapping is permanent, like everything in the MMIO window. */
const struct acpi_sdt_header *acpi_find_table(const char *signature);

#endif /* _ARCH_ACPI_H */
