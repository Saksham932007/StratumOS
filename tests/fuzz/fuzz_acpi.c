/* StratumOS - fuzzing the ACPI table walk.
 *
 * Firmware tables are the input a kernel cannot refuse: they arrive before
 * anything else and there is no fallback that does not involve giving up on
 * the machine. They are also pointer-rich - an RSDP points at an RSDT, which
 * is an array of pointers to tables, each of which has a length that the
 * parser indexes by - and every one of those pointers and lengths is a number
 * somebody else wrote.
 *
 * The fuzzer's bytes become physical memory. A physical address is an offset
 * into that buffer, so a parser following a pointer out of a table reads the
 * fuzzer's bytes; one reading past the end hits an ASan redzone.
 *
 * What a crash here means:
 *
 *   ASan report      a table length or entry count used without being
 *                   checked against what is actually there
 *   a hang           a MADT entry of length zero, which would loop forever,
 *                   or a self-referential table list
 *   abort()          a panic reached from firmware data
 */
#include <stdint.h>

#include <arch/acpi.h>

#include "shim.h"
#include <assert.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* The RSDP is found by scanning low physical memory for a signature, and the
 * scan needs the EBDA pointer at 0x40E to be something. Rather than make the
 * fuzzer guess all of that, the first bytes of the input are placed where the
 * BIOS area is and the signature is planted - so the fuzzer spends its
 * budget on the *tables*, which is where the parsing is, instead of on
 * rediscovering where to put eight bytes.
 *
 * The checksum is likewise computed rather than guessed. A fuzzer that has to
 * find a valid checksum before it can reach the parser never reaches the
 * parser; the checksum is arithmetic, not logic, and there is nothing to find
 * behind it. */
#define PHYS_SIZE 0x100000
#define RSDP_AT   0xE0000

static uint8_t physmem[PHYS_SIZE];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 64 || size > 64 * 1024)
        return 0;

    shim_reset();
    memset(physmem, 0, sizeof(physmem));

    /* The fuzzer's bytes, as the tables. Placed above the RSDP so that a
     * pointer anywhere into them is a pointer into fuzzer-controlled data. */
    size_t at = 0x10000;

    if (at + size > PHYS_SIZE)
        size = PHYS_SIZE - at;

    memcpy(physmem + at, data, size);

    /* A plausible RSDP whose RSDT pointer is taken from the input, so the
     * fuzzer chooses where the table list is - including choosing somewhere
     * useless, which is a case worth covering. */
    uint8_t *rsdp = physmem + RSDP_AT;

    memcpy(rsdp, "RSD PTR ", 8);
    memcpy(rsdp + 9, "FUZZ  ", 6);
    rsdp[15] = data[0] & 3;         /* revision: 0 picks the RSDT, 2 the XSDT */
    memcpy(rsdp + 16, data + 1, 4); /* rsdt_address */
    memcpy(rsdp + 20, data + 5, 4); /* length, for the extended checksum */
    memcpy(rsdp + 24, data + 9, 8); /* xsdt_address */

    /* The checksum over the first 20 bytes, which is what the parser
     * validates. */
    uint8_t sum = 0;

    for (int i = 0; i < 20; i++)
        if (i != 8)
            sum = (uint8_t)(sum + rsdp[i]);
    rsdp[8] = (uint8_t)(0u - sum);

    shim_set_physmem(physmem, sizeof(physmem));

    acpi_init();

    const struct acpi_info *info = acpi_get_info();

    if (info->available) {
        /* Whatever it believed, it has to be internally consistent -
         * everything downstream indexes these arrays. */
        assert(info->cpu_count <= ACPI_MAX_CPUS);
        assert(info->ioapic_count <= ACPI_MAX_IOAPICS);
        assert(info->iso_count <= ACPI_MAX_ISO);
        assert(info->cpu_count <= info->cpus_reported);

        /* And a local APIC address it reports has to be one a 32-bit kernel
         * could actually map. */
        if (info->madt_found && info->local_apic_phys)
            assert(info->local_apic_phys <= 0xFFFFFFFFull);
    }

    /* Looking up a table that is not there must not depend on the input. */
    (void)acpi_find_table("ZZZZ");
    (void)acpi_find_table("APIC");
    (void)acpi_find_table("FACP");

    return 0;
}
