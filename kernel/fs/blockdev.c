/* StratumOS - block devices, and the MBR partition table.
 *
 * The partition table is four 16-byte entries at offset 446 of the first
 * sector - immediately after the 446 bytes of boot code, which is why stage 1
 * has to fit in 446 rather than 512. It does, with room to spare, so this
 * kernel's own boot sector carries a real partition table describing the
 * filesystem that follows the kernel on the same disk.
 *
 * Only the LBA fields are read. The CHS fields in each entry exist for BIOSes
 * older than INT 13h extensions, cannot address more than 8 GiB, and are
 * routinely wrong on modern disks - a partition table where CHS and LBA
 * disagree is normal, and LBA is the one to believe.
 */
#define LOG_TAG "blk"

#include <drivers/ata.h>

#include <kernel/kernel.h>
#include <kernel/log.h>
#include <kernel/printf.h>
#include <kernel/string.h>

#include <fs/blockdev.h>

#define MBR_PARTITION_OFFSET 446
#define MBR_SIGNATURE_OFFSET 510
#define MBR_SIGNATURE        0xAA55

struct mbr_partition {
    u8 status; /* 0x80 = bootable; advisory             */
    u8 chs_first[3];
    u8 type;
    u8 chs_last[3];
    u32 first_lba;
    u32 sectors;
} PACKED;

_Static_assert(sizeof(struct mbr_partition) == 16,
               "an MBR partition entry is 16 bytes");

static struct blockdev devices[BLOCKDEV_MAX];
static u32 device_count;

u32 blockdev_count(void)
{
    return device_count;
}

const struct blockdev *blockdev_get(u32 index)
{
    if (index >= ARRAY_SIZE(devices) || !devices[index].present)
        return NULL;
    return &devices[index];
}

const struct blockdev *blockdev_find(const char *name)
{
    if (!name)
        return NULL;

    for (u32 i = 0; i < ARRAY_SIZE(devices); i++)
        if (devices[i].present && strcmp(devices[i].name, name) == 0)
            return &devices[i];

    return NULL;
}

static bool type_is_fat(u8 type)
{
    return type == MBR_TYPE_FAT16 || type == MBR_TYPE_FAT16_LBA ||
           type == MBR_TYPE_FAT12 || type == MBR_TYPE_FAT16_SML;
}

const struct blockdev *blockdev_first_fs(void)
{
    for (u32 i = 0; i < ARRAY_SIZE(devices); i++)
        if (devices[i].present && type_is_fat(devices[i].partition_type))
            return &devices[i];

    return NULL;
}

bool blockdev_read(const struct blockdev *dev, u64 offset, u32 count, void *buf)
{
    if (!dev || !dev->present) {
        pr_err("read from a device that is not present");
        return false;
    }

    if (offset + count > dev->sectors || offset + count < offset) {
        pr_err("%s: read of %u blocks at %llu runs past the device's %llu",
               dev->name, count, offset, dev->sectors);
        return false;
    }

    return ata_read(dev->drive, dev->first_lba + offset, count, buf);
}

static struct blockdev *alloc_device(void)
{
    for (u32 i = 0; i < ARRAY_SIZE(devices); i++)
        if (!devices[i].present)
            return &devices[i];
    return NULL;
}

static void register_device(const char *name, u32 drive, u64 first_lba,
                            u64 sectors, u8 type)
{
    struct blockdev *dev = alloc_device();

    if (!dev) {
        pr_warn("no room for %s; %u devices is the limit", name,
                (unsigned)ARRAY_SIZE(devices));
        return;
    }

    dev->present = true;
    strlcpy(dev->name, name, sizeof(dev->name));
    dev->drive = drive;
    dev->first_lba = first_lba;
    dev->sectors = sectors;
    dev->partition_type = type;
    device_count++;
}

static void scan_drive(u32 index)
{
    const struct ata_drive *d = ata_get_drive(index);

    if (!d)
        return;

    char name[BLOCKDEV_NAME_MAX];

    ksnprintf(name, sizeof(name), "hd%u", index);
    register_device(name, index, 0, d->sectors, 0);

    u8 sector[BLOCK_SIZE];

    if (!ata_read(index, 0, 1, sector)) {
        pr_warn("hd%u: cannot read sector 0; no partitions registered", index);
        return;
    }

    u16 signature;

    memcpy(&signature, &sector[MBR_SIGNATURE_OFFSET], sizeof(signature));

    if (signature != MBR_SIGNATURE) {
        pr_debug("hd%u: no 0xAA55 signature; not a partitioned disk", index);
        return;
    }

    for (u32 i = 0; i < 4; i++) {
        struct mbr_partition p;

        memcpy(&p, &sector[MBR_PARTITION_OFFSET + i * sizeof(p)], sizeof(p));

        if (p.type == 0 || p.sectors == 0)
            continue;

        /* A partition that starts or ends outside the disk is a corrupt
         * table, or a table from a different disk. Either way, registering it
         * would hand the filesystem layer a device whose every read fails. */
        if (p.first_lba >= d->sectors ||
            (u64)p.first_lba + p.sectors > d->sectors) {
            pr_warn("hd%u partition %u claims LBA %u+%u, past the disk's %llu",
                    index, i + 1, p.first_lba, p.sectors, d->sectors);
            continue;
        }

        ksnprintf(name, sizeof(name), "hd%up%u", index, i + 1);
        register_device(name, index, p.first_lba, p.sectors, p.type);

        pr_info("%s: type %02x%s, LBA %u + %u sectors (%u KiB)%s", name, p.type,
                type_is_fat(p.type) ? " (FAT)" : "", p.first_lba, p.sectors,
                p.sectors / 2, (p.status & 0x80) ? ", bootable" : "");
    }
}

void blockdev_init(void)
{
    memset(devices, 0, sizeof(devices));
    device_count = 0;

    for (u32 i = 0; i < ATA_MAX_DRIVES; i++)
        scan_drive(i);

    if (device_count == 0)
        pr_info("no block devices");
}
