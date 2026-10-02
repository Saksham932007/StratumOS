/* StratumOS - ATA PIO disk driver.
 *
 * See drivers/ata.h for why this is PIO and polled rather than DMA and
 * interrupt-driven.
 *
 * The register layout is the one the AT disk controller established in 1984
 * and that every PC has carried since. Offsets are from the channel's base
 * I/O port; the control block sits 0x206 further up, which is an artefact of
 * the original decoding and not something to rationalise.
 */
#define LOG_TAG "ata"

#include <arch/io.h>

#include <drivers/ata.h>

#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/string.h>

/* Command block, relative to the channel base. */
#define ATA_REG_DATA       0x00
#define ATA_REG_ERROR      0x01 /* reading                      */
#define ATA_REG_FEATURES   0x01 /* writing                      */
#define ATA_REG_SECCOUNT   0x02
#define ATA_REG_LBA_LOW    0x03
#define ATA_REG_LBA_MID    0x04
#define ATA_REG_LBA_HIGH   0x05
#define ATA_REG_DRIVE      0x06
#define ATA_REG_STATUS     0x07 /* reading; clears the IRQ       */
#define ATA_REG_COMMAND    0x07 /* writing                      */

/* Control block. Reading the alternate status register has the same value as
 * the status register but does *not* acknowledge the interrupt, which is what
 * makes it the right one to poll with. */
#define ATA_REG_ALT_STATUS 0x00
#define ATA_REG_CONTROL    0x00

/* Status bits. */
#define ATA_SR_ERR         0x01 /* an error; details in the error register */
#define ATA_SR_DRQ         0x08 /* ready to transfer a block              */
#define ATA_SR_SRV         0x10
#define ATA_SR_DF          0x20 /* device fault                            */
#define ATA_SR_RDY         0x40
#define ATA_SR_BSY         0x80 /* busy; every other bit is meaningless    */

/* Error bits. */
#define ATA_ER_AMNF        0x01
#define ATA_ER_TK0NF       0x02
#define ATA_ER_ABRT        0x04
#define ATA_ER_MCR         0x08
#define ATA_ER_IDNF        0x10
#define ATA_ER_MC          0x20
#define ATA_ER_UNC         0x40
#define ATA_ER_BBK         0x80

#define ATA_CMD_READ_PIO   0x20
#define ATA_CMD_READ_PIO48 0x24
#define ATA_CMD_IDENTIFY   0xEC

#define ATA_CTRL_NIEN      0x02 /* stop the drive asserting its IRQ */

/* Bounds. A drive that misses these is broken, and saying so beats hanging.
 * 30 million inb()s is on the order of seconds under emulation; the real
 * ATA specification allows 30 seconds for a spin-up, which no test wants to
 * wait for. */
#define ATA_SPIN_LIMIT     3000000u

static const u16 channel_base[2] = {0x1F0, 0x170};
static const u16 channel_ctrl[2] = {0x3F6, 0x376};

static struct ata_drive drives[ATA_MAX_DRIVES];
static u32 drive_count;
static struct ata_stats stats;

void ata_get_stats(struct ata_stats *out)
{
    if (out)
        *out = stats;
}

u32 ata_drive_count(void)
{
    return drive_count;
}

const struct ata_drive *ata_get_drive(u32 index)
{
    if (index >= ARRAY_SIZE(drives) || !drives[index].present)
        return NULL;
    return &drives[index];
}

/* ------------------------------------------------------------------------- */

static u8 status_of(u8 channel)
{
    return inb(channel_ctrl[channel] + ATA_REG_ALT_STATUS);
}

/* The specification requires 400 ns between selecting a drive and trusting
 * the status register. Four reads of the alternate status register take at
 * least that long on any bus that can run ATA at all, and reading it has no
 * side effects - which is the trick, and why it is the conventional way to
 * spell "wait a moment" here. */
static void select_delay(u8 channel)
{
    for (int i = 0; i < 4; i++)
        (void)status_of(channel);
}

/* Wait for BSY to clear. Returns the final status, or 0xFF on timeout - which
 * is also what a floating bus reads as, and both mean "no usable drive". */
static u8 wait_not_busy(u8 channel)
{
    for (u32 spin = 0; spin < ATA_SPIN_LIMIT; spin++) {
        u8 status = status_of(channel);

        if (!(status & ATA_SR_BSY))
            return status;
    }

    stats.timeouts++;
    pr_err("channel %u: BSY never cleared", channel);
    return 0xFF;
}

/* Wait until the drive has a block ready, or has given up. */
static bool wait_for_data(u8 channel)
{
    u8 status = wait_not_busy(channel);

    if (status == 0xFF)
        return false;

    if (status & (ATA_SR_ERR | ATA_SR_DF)) {
        u8 err = inb(channel_base[channel] + ATA_REG_ERROR);

        stats.errors++;
        pr_err("channel %u: status %02x error %02x%s%s%s%s", channel, status,
               err, (err & ATA_ER_IDNF) ? " (sector not found)" : "",
               (err & ATA_ER_UNC) ? " (uncorrectable)" : "",
               (err & ATA_ER_ABRT) ? " (aborted)" : "",
               (status & ATA_SR_DF) ? " (device fault)" : "");
        return false;
    }

    if (!(status & ATA_SR_DRQ)) {
        stats.errors++;
        pr_err("channel %u: no data to transfer (status %02x)", channel,
               status);
        return false;
    }

    return true;
}

/* IDENTIFY returns strings as 16-bit words with the bytes the wrong way
 * round, because the words are big-endian in a little-endian structure. */
static void copy_ata_string(char *dst, const u16 *words, u32 count)
{
    for (u32 i = 0; i < count; i++) {
        dst[i * 2] = (char)(words[i] >> 8);
        dst[i * 2 + 1] = (char)(words[i] & 0xFF);
    }
    dst[count * 2] = '\0';

    /* Drives pad with spaces; nobody wants to print those. */
    for (int i = (int)(count * 2) - 1; i >= 0 && dst[i] == ' '; i--)
        dst[i] = '\0';
}

static bool identify(u8 channel, u8 slot, struct ata_drive *out)
{
    u16 base = channel_base[channel];

    /* Interrupts off for this drive: the driver polls, and an unexpected
     * IRQ 14 with no handler is a warning in the log for no reason. */
    outb(channel_ctrl[channel] + ATA_REG_CONTROL, ATA_CTRL_NIEN);

    outb(base + ATA_REG_DRIVE, (u8)(0xA0 | (slot << 4)));
    select_delay(channel);

    /* A floating bus - no drive, nothing driving the lines - reads 0xFF.
     * Checking before issuing a command avoids waiting out the full timeout
     * on every empty slot, which is three of the four on a typical machine. */
    if (status_of(channel) == 0xFF)
        return false;

    outb(base + ATA_REG_SECCOUNT, 0);
    outb(base + ATA_REG_LBA_LOW, 0);
    outb(base + ATA_REG_LBA_MID, 0);
    outb(base + ATA_REG_LBA_HIGH, 0);
    outb(base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    select_delay(channel);

    /* Status zero means there is no device here at all. */
    if (status_of(channel) == 0)
        return false;

    u8 status = wait_not_busy(channel);

    if (status == 0xFF || (status & ATA_SR_ERR)) {
        /* An ATAPI device (a CD-ROM, which is how the GRUB ISO boots) aborts
         * IDENTIFY and puts a signature in the LBA registers. It is not a
         * block device this driver can read, so it is skipped - but it is
         * worth saying so rather than reporting a nonexistent error. */
        u8 lba_mid = inb(base + ATA_REG_LBA_MID);
        u8 lba_high = inb(base + ATA_REG_LBA_HIGH);

        if ((lba_mid == 0x14 && lba_high == 0xEB) ||
            (lba_mid == 0x69 && lba_high == 0x96))
            pr_debug("channel %u slot %u: an ATAPI device; skipping", channel,
                     slot);
        return false;
    }

    if (!(status & ATA_SR_DRQ))
        return false;

    u16 id[256];

    for (u32 i = 0; i < 256; i++)
        id[i] = inw(base + ATA_REG_DATA);

    out->present = true;
    out->channel = channel;
    out->slot = slot;

    copy_ata_string(out->serial, &id[10], 10);
    copy_ata_string(out->model, &id[27], 20);

    /* Word 83 bit 10 advertises 48-bit addressing; words 100-103 then hold
     * the capacity. Without it, words 60-61 hold a 28-bit count. */
    out->lba48 = (id[83] & (1u << 10)) != 0;

    if (out->lba48) {
        out->sectors = (u64)id[100] | ((u64)id[101] << 16) |
                       ((u64)id[102] << 32) | ((u64)id[103] << 48);
    } else {
        out->sectors = (u64)id[60] | ((u64)id[61] << 16);
    }

    /* A drive that reports LBA48 support but a zero 48-bit capacity is
     * describing itself with the 28-bit fields; trust those instead of
     * believing in a zero-sector disk. */
    if (out->sectors == 0)
        out->sectors = (u64)id[60] | ((u64)id[61] << 16);

    return true;
}

void ata_init(void)
{
    memset(drives, 0, sizeof(drives));
    memset(&stats, 0, sizeof(stats));
    drive_count = 0;

    for (u8 channel = 0; channel < 2; channel++) {
        for (u8 slot = 0; slot < 2; slot++) {
            u32 index = (u32)channel * 2 + slot;

            if (!identify(channel, slot, &drives[index]))
                continue;

            drive_count++;

            u64 kib = drives[index].sectors / 2;

            pr_info("hd%u: %s, %llu sectors (%llu MiB), LBA%s", index,
                    drives[index].model[0] ? drives[index].model : "(no model)",
                    drives[index].sectors, kib / 1024,
                    drives[index].lba48 ? "48" : "28");
        }
    }

    if (drive_count == 0)
        pr_info("no ATA drives found");
}

/* ------------------------------------------------------------------------- */

/* One command, at most 255 sectors. The caller does the splitting. */
static bool read_chunk(const struct ata_drive *d, u64 lba, u8 count, u8 *buf)
{
    u16 base = channel_base[d->channel];
    bool use48 = d->lba48 && (lba + count) > 0x0FFFFFFFull;

    if (wait_not_busy(d->channel) == 0xFF)
        return false;

    if (use48) {
        /* LBA48 writes each register twice: the high half first, then the
         * low. The drive latches the previous value when the register is
         * written again, which is how six bytes of address fit through three
         * ports. */
        outb(base + ATA_REG_DRIVE, (u8)(0x40 | (d->slot << 4)));
        outb(base + ATA_REG_SECCOUNT, 0);
        outb(base + ATA_REG_LBA_LOW, (u8)(lba >> 24));
        outb(base + ATA_REG_LBA_MID, (u8)(lba >> 32));
        outb(base + ATA_REG_LBA_HIGH, (u8)(lba >> 40));
        outb(base + ATA_REG_SECCOUNT, count);
        outb(base + ATA_REG_LBA_LOW, (u8)lba);
        outb(base + ATA_REG_LBA_MID, (u8)(lba >> 8));
        outb(base + ATA_REG_LBA_HIGH, (u8)(lba >> 16));
        outb(base + ATA_REG_COMMAND, ATA_CMD_READ_PIO48);
    } else {
        /* LBA28 puts the top four bits of the address in the drive-select
         * register alongside the drive number, which is why 0xE0 and not
         * 0xA0: bit 6 selects LBA addressing over CHS. */
        outb(base + ATA_REG_DRIVE,
             (u8)(0xE0 | (d->slot << 4) | ((lba >> 24) & 0x0F)));
        outb(base + ATA_REG_SECCOUNT, count);
        outb(base + ATA_REG_LBA_LOW, (u8)lba);
        outb(base + ATA_REG_LBA_MID, (u8)(lba >> 8));
        outb(base + ATA_REG_LBA_HIGH, (u8)(lba >> 16));
        outb(base + ATA_REG_COMMAND, ATA_CMD_READ_PIO);
    }

    stats.commands++;
    select_delay(d->channel);

    /* One DRQ handshake per sector, not one per command. Transferring all of
     * them after a single wait works on QEMU and fails on hardware. */
    for (u32 sector = 0; sector < count; sector++) {
        if (!wait_for_data(d->channel))
            return false;

        u16 *dst = (u16 *)(buf + sector * ATA_SECTOR_SIZE);

        for (u32 i = 0; i < ATA_SECTOR_SIZE / 2; i++)
            dst[i] = inw(base + ATA_REG_DATA);
    }

    return true;
}

bool ata_read(u32 drive, u64 lba, u32 count, void *buf)
{
    const struct ata_drive *d = ata_get_drive(drive);

    if (!d) {
        pr_err("read from hd%u, which is not present", drive);
        return false;
    }

    if (count == 0)
        return true;

    if (!buf) {
        pr_err("read into a null buffer");
        return false;
    }

    /* Refuse a read that runs off the end rather than letting the drive
     * report it. An out-of-range LBA usually means a filesystem followed a
     * corrupt pointer, and the useful message names the request. */
    if (lba + count > d->sectors || lba + count < lba) {
        pr_err("read of %u sectors at LBA %llu runs past hd%u's %llu sectors",
               count, lba, drive, d->sectors);
        return false;
    }

    stats.reads++;

    u8 *out = buf;

    while (count > 0) {
        u8 chunk = count > 255 ? 255 : (u8)count;

        if (!read_chunk(d, lba, chunk, out))
            return false;

        stats.sectors_read += chunk;
        lba += chunk;
        out += (u32)chunk * ATA_SECTOR_SIZE;
        count -= chunk;
    }

    return true;
}
