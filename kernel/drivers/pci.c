/* StratumOS - PCI configuration space and bus enumeration.
 *
 * Uses the legacy 0xCF8/0xCFC mechanism: write a bus/device/function/register
 * address to the address port, then read or write the data port. Enumeration
 * is a brute-force scan of all 256 buses x 32 slots x 8 functions.
 *
 * Brute force is the right choice here and not just the easy one: locating
 * buses properly needs either ACPI's MCFG table or recursive bridge walking,
 * and neither exists yet. A full scan is 65536 configuration reads, which
 * costs a few milliseconds once at boot.
 *
 * A vendor ID of 0xFFFF means "nothing here" - that is how the host bridge
 * reports an empty slot. Multi-function devices are identified by bit 7 of
 * the header-type byte; without checking it, every function of a
 * single-function device aliases to function 0 and the same card is reported
 * eight times.
 */
#define LOG_TAG "pci"

#include <arch/io.h>

#include <drivers/pci.h>

#include <kernel/log.h>
#include <kernel/string.h>

#define PCI_VENDOR_ID   0x00
#define PCI_DEVICE_ID   0x02
#define PCI_REVISION    0x08
#define PCI_PROG_IF     0x09
#define PCI_SUBCLASS    0x0A
#define PCI_CLASS       0x0B
#define PCI_HEADER_TYPE 0x0E
#define PCI_BAR0        0x10
#define PCI_IRQ_LINE    0x3C

#define PCI_NONE        0xFFFF

static struct pci_device devices[PCI_MAX_DEVICES];
static u32 device_count;
static bool scanned;

u32 pci_config_read32(u8 bus, u8 slot, u8 func, u8 offset)
{
    /* Bit 31 enables the configuration cycle; the register number must be
     * dword aligned, hence the 0xFC mask. */
    u32 address = (1u << 31) | ((u32)bus << 16) | ((u32)(slot & 0x1F) << 11) |
                  ((u32)(func & 0x07) << 8) | (offset & 0xFC);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 value)
{
    u32 address = (1u << 31) | ((u32)bus << 16) | ((u32)(slot & 0x1F) << 11) |
                  ((u32)(func & 0x07) << 8) | (offset & 0xFC);

    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
}

u16 pci_config_read16(u8 bus, u8 slot, u8 func, u8 offset)
{
    u32 dword = pci_config_read32(bus, slot, func, offset);

    /* Select the correct half of the dword that was actually fetched. */
    return (u16)((dword >> ((offset & 2) * 8)) & 0xFFFF);
}

static u8 pci_config_read8(u8 bus, u8 slot, u8 func, u8 offset)
{
    u32 dword = pci_config_read32(bus, slot, func, offset);

    return (u8)((dword >> ((offset & 3) * 8)) & 0xFF);
}

const char *pci_vendor_name(u16 vendor_id)
{
    /* Only the handful we are likely to meet in a VM or on a test board.
     * A full database belongs in userspace, not in a kernel image. */
    switch (vendor_id) {
    case 0x8086:
        return "Intel";
    case 0x1022:
        return "AMD";
    case 0x10DE:
        return "NVIDIA";
    case 0x1002:
        return "ATI/AMD";
    case 0x1234:
        return "QEMU (Bochs VGA)";
    case 0x1AF4:
        return "Red Hat (virtio)";
    case 0x1B36:
        return "Red Hat (QEMU device)";
    case 0x15AD:
        return "VMware";
    case 0x80EE:
        return "VirtualBox";
    case 0x1013:
        return "Cirrus Logic";
    case 0x106B:
        return "Apple";
    case 0x5333:
        return "S3";
    default:
        return "unknown vendor";
    }
}

const char *pci_class_name(u8 class_code, u8 subclass)
{
    switch (class_code) {
    case 0x00:
        return "unclassified";
    case 0x01:
        switch (subclass) {
        case 0x00:
            return "SCSI controller";
        case 0x01:
            return "IDE controller";
        case 0x06:
            return "SATA controller";
        case 0x08:
            return "NVMe controller";
        default:
            return "storage controller";
        }
    case 0x02:
        return "network controller";
    case 0x03:
        return "display controller";
    case 0x04:
        return "multimedia controller";
    case 0x05:
        return "memory controller";
    case 0x06:
        switch (subclass) {
        case 0x00:
            return "host bridge";
        case 0x01:
            return "ISA bridge";
        case 0x04:
            return "PCI-to-PCI bridge";
        default:
            return "bridge";
        }
    case 0x07:
        return "communication controller";
    case 0x08:
        return "system peripheral";
    case 0x09:
        return "input device";
    case 0x0C:
        switch (subclass) {
        case 0x03:
            return "USB controller";
        default:
            return "serial bus controller";
        }
    case 0x0D:
        return "wireless controller";
    default:
        return "other device";
    }
}

static void record(u8 bus, u8 slot, u8 func, u16 vendor)
{
    if (device_count >= PCI_MAX_DEVICES) {
        /* Warn once rather than on every further device. */
        if (device_count == PCI_MAX_DEVICES)
            pr_warn("more than %u PCI devices present; the rest are ignored",
                    (unsigned)PCI_MAX_DEVICES);
        return;
    }

    struct pci_device *d = &devices[device_count++];

    d->bus = bus;
    d->slot = slot;
    d->func = func;
    d->vendor_id = vendor;
    d->device_id = pci_config_read16(bus, slot, func, PCI_DEVICE_ID);
    d->revision = pci_config_read8(bus, slot, func, PCI_REVISION);
    d->prog_if = pci_config_read8(bus, slot, func, PCI_PROG_IF);
    d->subclass = pci_config_read8(bus, slot, func, PCI_SUBCLASS);
    d->class_code = pci_config_read8(bus, slot, func, PCI_CLASS);
    d->header_type = pci_config_read8(bus, slot, func, PCI_HEADER_TYPE);
    d->irq_line = pci_config_read8(bus, slot, func, PCI_IRQ_LINE);

    for (int i = 0; i < 6; i++)
        d->bar[i] = pci_config_read32(bus, slot, func, (u8)(PCI_BAR0 + i * 4));
}

void pci_init(void)
{
    memset(devices, 0, sizeof(devices));
    device_count = 0;

    for (u32 bus = 0; bus < 256; bus++) {
        for (u32 slot = 0; slot < 32; slot++) {
            u16 vendor = pci_config_read16((u8)bus, (u8)slot, 0, PCI_VENDOR_ID);
            if (vendor == PCI_NONE)
                continue;

            record((u8)bus, (u8)slot, 0, vendor);

            /* Bit 7 of the header type marks a multi-function device. Only
             * then is it meaningful to probe functions 1-7. */
            u8 header = pci_config_read8((u8)bus, (u8)slot, 0, PCI_HEADER_TYPE);
            if (!(header & 0x80))
                continue;

            for (u32 func = 1; func < 8; func++) {
                u16 fv = pci_config_read16((u8)bus, (u8)slot, (u8)func,
                                           PCI_VENDOR_ID);
                if (fv != PCI_NONE)
                    record((u8)bus, (u8)slot, (u8)func, fv);
            }
        }
    }

    scanned = true;
    pr_info("%u PCI device%s found", device_count,
            device_count == 1 ? "" : "s");
}

u32 pci_device_count(void)
{
    return scanned ? device_count : 0;
}

const struct pci_device *pci_device_at(u32 index)
{
    if (index >= device_count)
        return NULL;
    return &devices[index];
}
