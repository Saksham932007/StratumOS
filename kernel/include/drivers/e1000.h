/* StratumOS - Intel 82540EM (e1000) gigabit Ethernet.
 *
 * The device QEMU presents as `-device e1000`, and a real card people still
 * have. Chosen over virtio-net deliberately: virtio is simpler, but it is a
 * paravirtual interface, and the point of writing a driver is the part where
 * the hardware does not cooperate - descriptor rings the device owns half
 * of, a reset sequence with required delays, a MAC address that lives in an
 * EEPROM behind a polled register.
 *
 * See docs/NETWORK.md for the ring ownership protocol, which is the part of
 * this driver worth reading.
 */
#ifndef _DRIVERS_E1000_H
#define _DRIVERS_E1000_H

#include <kernel/types.h>

#include <net/net.h>

/* Probe PCI for a supported controller and bring it up. Returns false if
 * there is none, which is not an error: the kernel runs fine without a
 * network. */
bool e1000_init(void);
bool e1000_present(void);

/* What the hardware itself says, for the shell. Read live rather than
 * cached - the point of these is to answer "is it the driver or the device". */
struct e1000_info {
    bool present;
    u16 vendor_id, device_id;
    u8 bus, slot, func;
    u8 irq;
    paddr_t mmio_phys;
    struct mac_addr mac;
    bool mac_from_eeprom;
    bool link_up;
    u32 link_speed_mbps;
    bool full_duplex;

    u32 rx_ring_entries, tx_ring_entries;
    paddr_t rx_ring_phys, tx_ring_phys;
    u32 rx_head, rx_tail, tx_head, tx_tail;

    /* The device's own counters, not the driver's. A difference between
     * these and the stack's is the most useful single diagnostic there is:
     * it says whether frames were lost before or after the driver. */
    u32 dev_rx_packets, dev_tx_packets, dev_crc_errors, dev_rx_no_buffers;
    u32 interrupts, tx_ring_full, rx_overruns;
};

void e1000_get_info(struct e1000_info *out);

#endif /* _DRIVERS_E1000_H */
