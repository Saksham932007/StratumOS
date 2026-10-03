/* StratumOS - Intel 82540EM (e1000) gigabit Ethernet.
 *
 * The interesting half of this file is the descriptor ring protocol, so it is
 * worth stating before any register names appear.
 *
 * THE RING, AND WHO OWNS WHAT
 * ---------------------------
 * Receive and transmit are each a circular array of 16-byte descriptors in
 * memory the device reads and writes by DMA. Each descriptor holds a
 * *physical* address of a buffer, a length, and a status byte. There are two
 * indices per ring, and the whole protocol is which side moves which:
 *
 *   head (RDH/TDH)  the device moves. Where it will work next.
 *   tail (RDT/TDT)  the driver moves. Where the device must stop.
 *
 * The device consumes descriptors from head towards tail and will not pass
 * tail. So:
 *
 *   receive   the driver hands the device empty buffers by *advancing tail*.
 *             Tail points at the last descriptor the device may fill, so the
 *             ring is "full of empties" when tail == head - 1. A received
 *             frame arrives with the DD (descriptor done) bit set.
 *
 *   transmit  the driver writes a descriptor at tail, then advances tail,
 *             which is what tells the device there is work. The device sets
 *             DD when the frame is on the wire.
 *
 * One slot is always left unused. With `count` descriptors the ring can hold
 * `count - 1`, because tail == head has to mean empty rather than full -
 * there is no third state to distinguish them with. Getting this wrong gives
 * a ring the device thinks is empty the moment it is full, which presents as
 * transmits that silently never happen.
 *
 * WHY NO CACHE FLUSHING
 * ---------------------
 * x86 DMA is cache-coherent: the device's reads snoop the processor's caches.
 * So descriptors live in ordinary cached memory and no flush is needed. What
 * *is* needed is `volatile` and barriers, for the same reason the page-table
 * code needs them: the compiler cannot see that another agent reads these
 * words, so it is entitled to keep a descriptor's status byte in a register
 * forever, and entitled to move the store that advances tail above the stores
 * that filled the descriptor it publishes.
 *
 * On an architecture without coherent DMA this file would need explicit cache
 * maintenance, and that is one of the things a port would have to deal with.
 */
#define LOG_TAG "e1000"

#include <arch/io.h>
#include <arch/irq.h>

#include <drivers/e1000.h>
#include <drivers/pci.h>
#include <drivers/timer.h>

#include <kernel/layout.h>
#include <kernel/log.h>
#include <kernel/string.h>

#include <mm/pmm.h>
#include <mm/vmm.h>

#include <net/net.h>

/* ---- registers ---------------------------------------------------------- */

#define E1000_CTRL       0x0000
#define E1000_STATUS     0x0008
#define E1000_EECD       0x0010
#define E1000_EERD       0x0014
#define E1000_ICR        0x00C0
#define E1000_ITR        0x00C4
#define E1000_ICS        0x00C8
#define E1000_IMS        0x00D0
#define E1000_IMC        0x00D8
#define E1000_RCTL       0x0100
#define E1000_TCTL       0x0400
#define E1000_TIPG       0x0410
#define E1000_RDBAL      0x2800
#define E1000_RDBAH      0x2804
#define E1000_RDLEN      0x2808
#define E1000_RDH        0x2810
#define E1000_RDT        0x2818
#define E1000_TDBAL      0x3800
#define E1000_TDBAH      0x3804
#define E1000_TDLEN      0x3808
#define E1000_TDH        0x3810
#define E1000_TDT        0x3818
#define E1000_MTA        0x5200 /* 128 dwords of multicast table */
#define E1000_RAL0       0x5400
#define E1000_RAH0       0x5404

/* Statistics the device keeps itself. Reading most of them clears them,
 * which is why the driver accumulates rather than reporting the raw read. */
#define E1000_CRCERRS    0x4000
#define E1000_RNBC       0x40A0 /* receive no buffers count */
#define E1000_GPRC       0x4074 /* good packets received    */
#define E1000_GPTC       0x4080 /* good packets transmitted */

#define CTRL_FD          (1u << 0)
#define CTRL_ASDE        (1u << 5)
#define CTRL_SLU         (1u << 6) /* set link up */
#define CTRL_RST         (1u << 26)
#define CTRL_PHY_RST     (1u << 31)

#define STATUS_FD        (1u << 0)
#define STATUS_LU        (1u << 1) /* link up */
#define STATUS_SPEED_SHF 6
#define STATUS_SPEED_MSK 0x3u

#define RCTL_EN          (1u << 1)
#define RCTL_SBP         (1u << 2)
#define RCTL_UPE         (1u << 3)  /* unicast promiscuous */
#define RCTL_MPE         (1u << 4)  /* multicast promiscuous */
#define RCTL_LPE         (1u << 5)  /* long packet enable */
#define RCTL_BAM         (1u << 15) /* accept broadcast */
#define RCTL_BSIZE_2048  (0u << 16)
#define RCTL_SECRC       (1u << 26) /* strip the Ethernet CRC */

#define TCTL_EN          (1u << 1)
#define TCTL_PSP         (1u << 3) /* pad short packets */
#define TCTL_CT_SHF      4
#define TCTL_COLD_SHF    12

#define ICR_TXDW         (1u << 0)
#define ICR_LSC          (1u << 2) /* link status change */
#define ICR_RXDMT        (1u << 4) /* receive descriptor minimum threshold */
#define ICR_RXO          (1u << 6) /* receiver overrun */
#define ICR_RXT0         (1u << 7) /* receive timer, i.e. a packet arrived */

#define EERD_START       (1u << 0)
#define EERD_DONE        (1u << 4)

#define TXD_CMD_EOP      (1u << 0)
#define TXD_CMD_IFCS     (1u << 1) /* insert the frame checksum */
#define TXD_CMD_RS       (1u << 3) /* report status, i.e. set DD when done */
#define TXD_STAT_DD      (1u << 0)

#define RXD_STAT_DD      (1u << 0)
#define RXD_STAT_EOP     (1u << 1)

/* ---- rings -------------------------------------------------------------- */

/* 32 of each. The ring has to be a multiple of 8 descriptors because RDLEN
 * and TDLEN are in bytes and the device requires a 128-byte multiple; 32 x 16
 * is 512. Larger rings buy burst tolerance this stack cannot use, because the
 * layers above it process a frame to completion before taking the next. */
#define RX_DESCS         32
#define TX_DESCS         32
#define RX_BUF           2048 /* must match RCTL_BSIZE_2048 */
#define TX_BUF           2048

struct rx_desc {
    u64 addr;
    u16 length;
    u16 checksum;
    u8 status;
    u8 errors;
    u16 special;
} PACKED;

struct tx_desc {
    u64 addr;
    u16 length;
    u8 cso;
    u8 cmd;
    u8 status;
    u8 css;
    u16 special;
} PACKED;

static struct {
    bool present;
    volatile u8 *mmio;
    paddr_t mmio_phys;
    struct pci_device pci;
    u8 irq;

    struct mac_addr mac;
    bool mac_from_eeprom;

    volatile struct rx_desc *rx;
    volatile struct tx_desc *tx;
    paddr_t rx_phys, tx_phys;

    u8 *rx_buf[RX_DESCS];
    u8 *tx_buf[TX_DESCS];
    paddr_t rx_buf_phys[RX_DESCS];
    paddr_t tx_buf_phys[TX_DESCS];

    u32 tx_next; /* the driver's own tail, mirrored so a read of TDT is not
                  * needed on the fast path */

    u32 dev_rx_packets, dev_tx_packets, dev_crc_errors, dev_rx_no_buffers;
    u32 interrupts, tx_ring_full, rx_overruns;
} nic;

static inline u32 reg_read(u32 off)
{
    return *(volatile u32 *)(nic.mmio + off);
}

static inline void reg_write(u32 off, u32 val)
{
    *(volatile u32 *)(nic.mmio + off) = val;
}

/* The controllers QEMU can present and real cards people have. Deliberately
 * a list rather than a class match: "any Ethernet controller" would claim an
 * rtl8139 and then write e1000 registers into it. */
static const struct {
    u16 vendor, device;
    const char *name;
} supported[] = {
    {0x8086, 0x100E, "82540EM"},
    {0x8086, 0x100F, "82545EM"},
    {0x8086, 0x1004, "82544GC"},
    {0x8086, 0x10D3, "82574L"},
};

/* ---- the MAC address ---------------------------------------------------- */

/* Read one 16-bit word from the EEPROM behind EERD. */
static bool eeprom_read(u8 word, u16 *out)
{
    reg_write(E1000_EERD, ((u32)word << 8) | EERD_START);

    /* Bounded, because a controller whose EEPROM is absent or busy would
     * otherwise hang the boot. The datasheet gives no maximum, so this is a
     * generous guess with a real limit rather than none. */
    for (u32 spin = 0; spin < 100000; spin++) {
        u32 v = reg_read(E1000_EERD);

        if (v & EERD_DONE) {
            *out = (u16)(v >> 16);
            return true;
        }
        io_wait();
    }

    return false;
}

static void read_mac(void)
{
    /* The receive address registers are the authority: on real hardware the
     * controller loads them from the EEPROM during reset, and QEMU sets them
     * from the -netdev option. Reading them is one register access and needs
     * no polling. */
    u32 ral = reg_read(E1000_RAL0);
    u32 rah = reg_read(E1000_RAH0);

    nic.mac.b[0] = (u8)(ral);
    nic.mac.b[1] = (u8)(ral >> 8);
    nic.mac.b[2] = (u8)(ral >> 16);
    nic.mac.b[3] = (u8)(ral >> 24);
    nic.mac.b[4] = (u8)(rah);
    nic.mac.b[5] = (u8)(rah >> 8);

    if (!mac_equal(&nic.mac, &mac_zero))
        return;

    /* RAL/RAH came back empty, so the EEPROM has to be read directly. The
     * fallback exists because a controller that was reset but never
     * configured by firmware is a real state, and a MAC of all zeros is one
     * every switch on the path will drop. */
    for (u8 word = 0; word < 3; word++) {
        u16 v = 0;

        if (!eeprom_read(word, &v)) {
            pr_warn("neither RAL/RAH nor the EEPROM gave a MAC address");
            return;
        }

        nic.mac.b[word * 2] = (u8)v;
        nic.mac.b[word * 2 + 1] = (u8)(v >> 8);
    }

    nic.mac_from_eeprom = true;
}

/* ---- bring-up ----------------------------------------------------------- */

/* Allocate the two rings and their buffers.
 *
 * The device addresses all of this by physical address, so the rings have to
 * be physically contiguous - pmm_alloc_frames() - and the buffers have to
 * have their physical addresses recorded. They are reached from C through the
 * MMIO window, which is a mapping of arbitrary physical memory; the linear
 * map covers only the first 16 MiB and the allocator is free to hand out a
 * frame from anywhere. */
static bool alloc_rings(void)
{
    /* 32 descriptors x 16 bytes = 512 bytes each, so one frame holds both
     * rings with room to spare - but they are allocated separately so that
     * RDBAL and TDBAL are independent, which is what the datasheet's
     * 16-byte alignment requirement is about. */
    nic.rx_phys = pmm_alloc_frame();
    nic.tx_phys = pmm_alloc_frame();
    if (!nic.rx_phys || !nic.tx_phys) {
        pr_err("no memory for the descriptor rings");
        return false;
    }

    nic.rx = vmm_map_mmio(nic.rx_phys, PAGE_SIZE, false);
    nic.tx = vmm_map_mmio(nic.tx_phys, PAGE_SIZE, false);
    if (!nic.rx || !nic.tx) {
        pr_err("no address space to map the descriptor rings");
        return false;
    }

    memset((void *)nic.rx, 0, PAGE_SIZE);
    memset((void *)nic.tx, 0, PAGE_SIZE);

    /* Two 2048-byte buffers per 4 KiB frame. 32 of each ring means 16 frames
     * per direction. */
    for (u32 i = 0; i < RX_DESCS; i += 2) {
        paddr_t frame = pmm_alloc_frame();
        u8 *va;

        if (!frame || !(va = vmm_map_mmio(frame, PAGE_SIZE, false))) {
            pr_err("no memory for receive buffers");
            return false;
        }

        nic.rx_buf[i] = va;
        nic.rx_buf_phys[i] = frame;
        nic.rx_buf[i + 1] = va + RX_BUF;
        nic.rx_buf_phys[i + 1] = frame + RX_BUF;
    }

    for (u32 i = 0; i < TX_DESCS; i += 2) {
        paddr_t frame = pmm_alloc_frame();
        u8 *va;

        if (!frame || !(va = vmm_map_mmio(frame, PAGE_SIZE, false))) {
            pr_err("no memory for transmit buffers");
            return false;
        }

        nic.tx_buf[i] = va;
        nic.tx_buf_phys[i] = frame;
        nic.tx_buf[i + 1] = va + TX_BUF;
        nic.tx_buf_phys[i + 1] = frame + TX_BUF;
    }

    return true;
}

static void setup_rx(void)
{
    for (u32 i = 0; i < RX_DESCS; i++) {
        nic.rx[i].addr = (u64)nic.rx_buf_phys[i];
        nic.rx[i].status = 0;
    }

    reg_write(E1000_RDBAL, (u32)nic.rx_phys);
    reg_write(E1000_RDBAH, 0); /* the whole ring is below 4 GiB */
    reg_write(E1000_RDLEN, RX_DESCS * (u32)sizeof(struct rx_desc));
    reg_write(E1000_RDH, 0);

    /* Tail at the last descriptor, so the device may fill 0..RX_DESCS-1. The
     * one-slot rule does not bite on receive the way it does on transmit,
     * because the driver moves tail as it consumes. */
    reg_write(E1000_RDT, RX_DESCS - 1);

    /* SECRC strips the 4-byte Ethernet CRC, so a received length is the
     * frame length the layers above expect. Without it every length is four
     * too big and every payload has four bytes of garbage on the end, which
     * presents as checksums that fail for no visible reason.
     *
     * BAM accepts broadcast, which ARP needs: a who-has for this machine
     * arrives addressed to ff:ff:ff:ff:ff:ff, so a receiver that only
     * accepts its own unicast address can never be found. */
    reg_write(E1000_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
}

static void setup_tx(void)
{
    for (u32 i = 0; i < TX_DESCS; i++) {
        nic.tx[i].addr = (u64)nic.tx_buf_phys[i];
        nic.tx[i].cmd = 0;
        /* DD set on every descriptor to start with, which marks them all as
         * "the device is finished with this one" - i.e. free. The device
         * only ever sets DD; the driver clears it when it takes a slot. */
        nic.tx[i].status = TXD_STAT_DD;
    }

    reg_write(E1000_TDBAL, (u32)nic.tx_phys);
    reg_write(E1000_TDBAH, 0);
    reg_write(E1000_TDLEN, TX_DESCS * (u32)sizeof(struct tx_desc));
    reg_write(E1000_TDH, 0);
    reg_write(E1000_TDT, 0);
    nic.tx_next = 0;

    /* Collision threshold 15 and collision distance 64, which are the
     * datasheet's values for full duplex. PSP pads anything shorter than the
     * 60-byte Ethernet minimum, so the stack above does not have to - an ARP
     * reply is 42 bytes and would otherwise be a runt. */
    reg_write(E1000_TCTL, TCTL_EN | TCTL_PSP | (15u << TCTL_CT_SHF) |
                              (64u << TCTL_COLD_SHF));
    reg_write(E1000_TIPG, 10 | (8 << 10) | (6 << 20));
}

/* ---- the device_t interface -------------------------------------------- */

static bool e1000_link_up(void)
{
    return (reg_read(E1000_STATUS) & STATUS_LU) != 0;
}

static bool e1000_transmit(const void *frame, size_t len)
{
    if (!nic.present || len == 0 || len > TX_BUF)
        return false;

    u32 slot = nic.tx_next;
    volatile struct tx_desc *d = &nic.tx[slot];

    /* The device sets DD when it is done with a descriptor. A slot whose DD
     * is clear is still owned by the device, and writing it would corrupt a
     * frame in flight. */
    if (!(d->status & TXD_STAT_DD)) {
        nic.tx_ring_full++;
        return false;
    }

    memcpy(nic.tx_buf[slot], frame, len);

    d->length = (u16)len;
    d->cso = 0;
    d->css = 0;
    d->special = 0;
    d->status = 0; /* the device will set DD */
    d->cmd = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;

    /* The barrier is the whole correctness of this function. Advancing tail
     * is what tells the device to read the descriptor, so every store above
     * has to be visible first - and the compiler, which cannot see that
     * another agent reads this memory, is otherwise entitled to move the
     * tail store up. x86's store ordering makes the hardware side of this
     * free; the compiler side is not. */
    barrier();

    nic.tx_next = (slot + 1) % TX_DESCS;
    reg_write(E1000_TDT, nic.tx_next);

    struct net_stats *s = net_stats();

    s->tx_frames++;
    s->tx_bytes += (u32)len;
    return true;
}

static u32 e1000_poll(u32 max)
{
    u32 handled = 0;

    if (!nic.present)
        return 0;

    /* Where the driver resumes is derived from tail rather than kept
     * separately: tail is the last descriptor the device may fill, so the
     * next one to inspect is the one after it. One source of truth, and it
     * survives an interrupt arriving in the middle. */
    while (handled < max) {
        u32 tail = reg_read(E1000_RDT);
        u32 slot = (tail + 1) % RX_DESCS;
        volatile struct rx_desc *d = &nic.rx[slot];

        if (!(d->status & RXD_STAT_DD))
            break; /* the device has not filled this one */

        u16 len = d->length;
        u8 status = d->status;
        u8 errors = d->errors;

        barrier();

        if (errors) {
            pr_debug("receive descriptor %u reports errors 0x%02x", slot,
                     errors);
        } else if (!(status & RXD_STAT_EOP)) {
            /* A frame split across descriptors. With 2048-byte buffers and a
             * 1500-byte MTU this cannot happen unless LPE is set, which it
             * is not - so rather than build reassembly that can never be
             * tested, the frame is dropped and counted. */
            nic.rx_overruns++;
        } else {
            struct net_stats *s = net_stats();

            s->rx_frames++;
            s->rx_bytes += len;
            eth_input(nic.rx_buf[slot], len);
        }

        /* Give the descriptor back. Clearing DD first and advancing tail
         * second matters for the same reason as on transmit: tail is the
         * publish. */
        d->status = 0;
        barrier();
        reg_write(E1000_RDT, slot);

        handled++;
    }

    return handled;
}

/* Not const, because .mac is only known once the controller has been read.
 *
 * It was const, with .mac left at zero, and the bug that produced was worth
 * the comment: every frame went out with a source MAC of 00:00:00:00:00:00,
 * the peer replied *to* that address, and the controller's own receive
 * filter dropped the reply because it was not addressed to the card. So
 * transmit worked, the boot log printed the right MAC - it was printing the
 * driver's copy - and nothing was ever received, with no error anywhere.
 *
 * Only a packet capture showed it. docs/NETWORK.md has the frame. */
static struct net_device e1000_netdev = {
    .name = "eth0",
    .transmit = e1000_transmit,
    .link_up = e1000_link_up,
    .poll = e1000_poll,
};

static void e1000_irq(struct regs *r)
{
    UNUSED(r);

    /* Reading ICR clears it, which is also what de-asserts the interrupt
     * line. Reading it once and working from the copy is required rather
     * than tidy: a second read would return zero and lose the cause. */
    u32 cause = reg_read(E1000_ICR);

    if (!cause)
        return; /* shared line, not ours */

    nic.interrupts++;

    if (cause & ICR_RXO)
        nic.rx_overruns++;

    if (cause & ICR_LSC)
        pr_info("link %s", e1000_link_up() ? "up" : "down");

    if (cause & (ICR_RXT0 | ICR_RXDMT | ICR_RXO))
        e1000_poll(RX_DESCS);
}

/* ---- init --------------------------------------------------------------- */

static const char *find_device(struct pci_device *out)
{
    for (u32 i = 0; i < pci_device_count(); i++) {
        const struct pci_device *d = pci_device_at(i);

        for (size_t k = 0; k < ARRAY_SIZE(supported); k++) {
            if (d->vendor_id != supported[k].vendor ||
                d->device_id != supported[k].device)
                continue;

            *out = *d;
            return supported[k].name;
        }
    }

    return NULL;
}

bool e1000_init(void)
{
    const char *model = find_device(&nic.pci);

    if (!model) {
        pr_info("no supported Ethernet controller on the PCI bus");
        return false;
    }

    /* BAR0 is the memory-mapped register window. The low bits are flags, not
     * address: bit 0 says memory or I/O space, bits 1-3 are the type. */
    if (nic.pci.bar[0] & 1) {
        pr_err("BAR0 is an I/O port range; this driver needs the memory-mapped "
               "window");
        return false;
    }

    nic.mmio_phys = nic.pci.bar[0] & ~0xFu;
    nic.irq = nic.pci.irq_line;

    /* Uncached, because these are device registers: a cached read would
     * return a stale STATUS and a cached write might never arrive. This is
     * the one mapping in the driver that must not be ordinary memory. */
    nic.mmio = vmm_map_mmio(nic.mmio_phys, 128 * KIB, true);
    if (!nic.mmio) {
        pr_err("cannot map the register window at %p", (void *)nic.mmio_phys);
        return false;
    }

    /* Bus mastering. The device cannot touch memory without it, so a driver
     * that forgets this gets a ring the device never reads and transmits
     * that never happen - with every register reading back correctly. */
    u32 cmd = pci_config_read32(nic.pci.bus, nic.pci.slot, nic.pci.func, 0x04);

    pci_config_write32(nic.pci.bus, nic.pci.slot, nic.pci.func, 0x04,
                       cmd | 0x04 | 0x02);

    /* Mask every interrupt before the reset, so a device that was already
     * running cannot deliver one into a handler that is not installed. */
    reg_write(E1000_IMC, 0xFFFFFFFFu);
    (void)reg_read(E1000_ICR);

    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_RST);

    /* The datasheet requires a delay after reset before any register is
     * touched; 10 ms is well past it. Spun on the PIT rather than a busy
     * count so it is a real duration on any processor speed. */
    timer_busy_wait_ms(10);

    reg_write(E1000_IMC, 0xFFFFFFFFu);
    (void)reg_read(E1000_ICR);

    /* Set link up and let the PHY auto-negotiate. ASDE plus SLU is the
     * documented combination; without SLU the link never comes up and every
     * transmit is swallowed. */
    reg_write(E1000_CTRL, reg_read(E1000_CTRL) | CTRL_SLU | CTRL_ASDE);

    read_mac();

    /* The multicast table array is not cleared by reset on every stepping,
     * and a stale entry means accepting frames meant for someone else. */
    for (u32 i = 0; i < 128; i++)
        reg_write(E1000_MTA + i * 4, 0);

    if (!alloc_rings())
        return false;

    setup_rx();
    setup_tx();

    irq_install_handler(nic.irq, e1000_irq, "e1000");

    /* Receive-timer, descriptor-minimum, overrun and link-change. Transmit
     * completion is deliberately not enabled: nothing waits on it, and an
     * interrupt per transmitted frame is the most expensive way to learn
     * something the next transmit would have discovered anyway. */
    reg_write(E1000_IMS, ICR_RXT0 | ICR_RXDMT | ICR_RXO | ICR_LSC);

    nic.present = true;

    /* The address the stack puts in every frame it sends. One assignment,
     * and leaving it out cost an afternoon - see the comment on the struct. */
    e1000_netdev.mac = nic.mac;
    net_set_device(&e1000_netdev);

    char mb[MAC_STR_LEN];
    u32 status = reg_read(E1000_STATUS);
    static const u32 speeds[] = {10, 100, 1000, 1000};

    pr_info("%s at %02x:%02x.%u, MMIO %p -> %p, IRQ %u, MAC %s%s", model,
            nic.pci.bus, nic.pci.slot, nic.pci.func, (void *)nic.mmio_phys,
            (void *)nic.mmio, nic.irq, mac_str(&nic.mac, mb, sizeof(mb)),
            nic.mac_from_eeprom ? " (from EEPROM)" : "");
    pr_info("link %s%s, %u rx / %u tx descriptors of %u bytes",
            (status & STATUS_LU) ? "up" : "down",
            (status & STATUS_LU)
                ? ((status & STATUS_FD) ? ", full duplex" : ", half duplex")
                : "",
            RX_DESCS, TX_DESCS, RX_BUF);
    UNUSED(speeds);

    return true;
}

bool e1000_present(void)
{
    return nic.present;
}

void e1000_get_info(struct e1000_info *out)
{
    memset(out, 0, sizeof(*out));

    out->present = nic.present;
    if (!nic.present)
        return;

    u32 status = reg_read(E1000_STATUS);
    static const u32 speeds[] = {10, 100, 1000, 1000};

    out->vendor_id = nic.pci.vendor_id;
    out->device_id = nic.pci.device_id;
    out->bus = nic.pci.bus;
    out->slot = nic.pci.slot;
    out->func = nic.pci.func;
    out->irq = nic.irq;
    out->mmio_phys = nic.mmio_phys;
    out->mac = nic.mac;
    out->mac_from_eeprom = nic.mac_from_eeprom;
    out->link_up = (status & STATUS_LU) != 0;
    out->full_duplex = (status & STATUS_FD) != 0;
    out->link_speed_mbps =
        speeds[(status >> STATUS_SPEED_SHF) & STATUS_SPEED_MSK];

    out->rx_ring_entries = RX_DESCS;
    out->tx_ring_entries = TX_DESCS;
    out->rx_ring_phys = nic.rx_phys;
    out->tx_ring_phys = nic.tx_phys;
    out->rx_head = reg_read(E1000_RDH);
    out->rx_tail = reg_read(E1000_RDT);
    out->tx_head = reg_read(E1000_TDH);
    out->tx_tail = reg_read(E1000_TDT);

    /* These registers clear on read, so they are accumulated rather than
     * reported raw - otherwise two `net` commands in a row disagree and
     * neither is wrong. */
    nic.dev_rx_packets += reg_read(E1000_GPRC);
    nic.dev_tx_packets += reg_read(E1000_GPTC);
    nic.dev_crc_errors += reg_read(E1000_CRCERRS);
    nic.dev_rx_no_buffers += reg_read(E1000_RNBC);

    out->dev_rx_packets = nic.dev_rx_packets;
    out->dev_tx_packets = nic.dev_tx_packets;
    out->dev_crc_errors = nic.dev_crc_errors;
    out->dev_rx_no_buffers = nic.dev_rx_no_buffers;
    out->interrupts = nic.interrupts;
    out->tx_ring_full = nic.tx_ring_full;
    out->rx_overruns = nic.rx_overruns;
}
