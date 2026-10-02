/* StratumOS - ATA (IDE) disk access by programmed I/O.
 *
 * PIO rather than DMA, and polling rather than interrupts. Both are
 * deliberate choices for a first block driver, and both are the slow option:
 *
 *   PIO means every sector crosses the bus through the data register, 256
 *   16-bit reads at a time, with the CPU doing the moving. Bus-mastering DMA
 *   needs a PRD table, a physical-address scatter list and the controller's
 *   own BAR, none of which can be tested until the simpler path works.
 *
 *   Polling means the driver spins on the status register instead of sleeping
 *   on IRQ 14. That burns the rest of a timeslice on a real disk, which is
 *   why no real kernel does it - but it keeps the read path a straight line,
 *   with no state machine and nothing to get wrong about a request that
 *   completes while its handler is still being installed.
 *
 * Every wait is bounded. A disk that never clears BSY is a disk that would
 * otherwise hang the kernel at boot with no message, which is the single
 * least debuggable failure a driver can have.
 */
#ifndef _DRIVERS_ATA_H
#define _DRIVERS_ATA_H

#include <kernel/types.h>

#define ATA_SECTOR_SIZE 512

/* Two channels, two drives each: the historical ISA arrangement, which is
 * still what a PIIX3 presents and what QEMU emulates. */
#define ATA_MAX_DRIVES  4

struct ata_drive {
    bool present;
    bool lba48;      /* supports 48-bit addressing          */
    u8 channel;      /* 0 = primary (0x1F0), 1 = secondary  */
    u8 slot;         /* 0 = master, 1 = slave               */
    u64 sectors;     /* capacity, in 512-byte sectors       */
    char model[41];  /* IDENTIFY words 27-46, byte-swapped  */
    char serial[21]; /* IDENTIFY words 10-19                */
};

/* Probe both channels with IDENTIFY. Safe to call once, at boot. */
void ata_init(void);

u32 ata_drive_count(void);
const struct ata_drive *ata_get_drive(u32 index);

/* Read `count` sectors starting at `lba` into `buf`, which must have room
 * for count * ATA_SECTOR_SIZE bytes. Returns false on any error, having
 * logged what the drive said.
 *
 * `count` is bounded to 255 per command by the 8-bit sector-count register;
 * larger requests are split internally, so a caller may ask for any length
 * the buffer can hold. */
bool ata_read(u32 drive, u64 lba, u32 count, void *buf);

/* Counters, for `disk` in the shell and for the test suite. */
struct ata_stats {
    u32 reads; /* ata_read() calls                    */
    u32 sectors_read;
    u32 commands; /* individual ATA commands issued      */
    u32 errors;
    u32 timeouts;
};
void ata_get_stats(struct ata_stats *out);

#endif /* _DRIVERS_ATA_H */
