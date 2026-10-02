/* StratumOS - PCI configuration space access and bus enumeration.
 *
 * Uses the legacy 0xCF8/0xCFC configuration mechanism and a brute-force scan
 * of bus/device/function. That is the right starting point: it needs no ACPI
 * tables and it is exactly how the firmware found the devices in the first
 * place.
 */
#ifndef _DRIVERS_PCI_H
#define _DRIVERS_PCI_H

#include <kernel/types.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_MAX_DEVICES    32

struct pci_device {
    u8 bus, slot, func;
    u16 vendor_id, device_id;
    u8 class_code, subclass, prog_if, revision;
    u8 header_type;
    u8 irq_line;
    u32 bar[6];
};

void pci_init(void);
u32 pci_device_count(void);
const struct pci_device *pci_device_at(u32 index);
const char *pci_class_name(u8 class_code, u8 subclass);
const char *pci_vendor_name(u16 vendor_id);

u32 pci_config_read32(u8 bus, u8 slot, u8 func, u8 offset);
u16 pci_config_read16(u8 bus, u8 slot, u8 func, u8 offset);
void pci_config_write32(u8 bus, u8 slot, u8 func, u8 offset, u32 value);

#endif /* _DRIVERS_PCI_H */
