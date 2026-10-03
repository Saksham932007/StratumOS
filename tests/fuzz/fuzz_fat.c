/* StratumOS - fuzzing the FAT16 driver against an arbitrary disk.
 *
 * The filesystem is the largest attacker-controlled data structure this
 * kernel reads: a boot sector whose every field is a divisor or a multiplier,
 * an allocation table that is a linked list whose pointers live in that same
 * data, and directory entries whose sizes and starting clusters are taken on
 * trust unless something checks them.
 *
 * The fuzzer's bytes are the disk. The driver mounts it, resolves paths
 * through it, reads files out of it and walks its directories - the same
 * sequence `exec` performs, which is the sequence that matters.
 *
 * What a crash here means:
 *
 *   ASan report      a read outside the disk buffer, which means a sector
 *                    number was used without being bounds-checked
 *   a hang           a cluster chain that loops, or a directory walk that
 *                    does not terminate; libFuzzer's -timeout catches it
 *   abort()          a panic, i.e. the driver reached a state it asserts
 *                    cannot happen
 *   a leak           fat16_read_whole() allocating and not freeing on an
 *                    error path
 */
#include <stdint.h>

#include "shim.h"
#include <assert.h>
#include <fs/blockdev.h>
#include <fs/fat16.h>
#include <stdlib.h>
#include <string.h>

const struct blockdev *shim_device(void);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Paths chosen to exercise the resolver rather than the filesystem: the root,
 * a file, a directory, a file inside a directory, and four malformed ones
 * that must each be refused rather than mishandled. */
static const char *const probe_paths[] = {
    "/",
    "/README.TXT",
    "/BIN",
    "/BIN/INIT",
    "/bin/init",          /* case-insensitive, as FAT is       */
    "//BIN//INIT",        /* redundant separators              */
    "/BIN/INIT/NONSENSE", /* a file used as a directory        */
    "RELATIVE",           /* no working directory exists       */
    "/AVERYLONGNAMETHATCANNOTPOSSIBLYFIT",
    "/\xff\xfe\x01", /* bytes no 8.3 name can hold        */
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* A filesystem needs a boot sector and somewhere for its metadata; below
     * that every input is rejected for the same uninteresting reason. */
    if (size < 4096 || size > 4 * 1024 * 1024)
        return 0;

    /* Whole sectors only, which is what a block device presents. */
    size -= size % 512;

    shim_reset();
    shim_set_disk(data, size);

    if (!fat16_mount(shim_device()))
        return 0;

    /* It mounted. Then the geometry it derived has to be self-consistent,
     * because every read below is computed from it - and a mount that
     * accepted a contradictory BPB would be the bug, not the reads. */
    const struct fat_info *f = fat16_get_info();

    assert(f->mounted);
    assert(f->bytes_per_sector == 512);
    assert(f->sectors_per_cluster > 0);
    assert(f->fat_start >= f->reserved_sectors);
    assert(f->root_start > f->fat_start);
    assert(f->data_start > f->root_start);
    assert(f->data_start < f->total_sectors);
    assert(f->cluster_count >= 4085 && f->cluster_count <= 65524);
    assert((uint64_t)f->total_sectors * 512 <= size);

    for (size_t i = 0; i < sizeof(probe_paths) / sizeof(probe_paths[0]); i++) {
        struct fat_dirent entry;

        if (fat16_stat(probe_paths[i], &entry)) {
            /* A resolved entry's cluster has to be inside the filesystem, or
             * reading it would walk off the end of the disk. The root is the
             * exception: FAT16's root has no cluster number. */
            if (!(entry.first_cluster == 0 && entry.is_dir))
                assert(entry.first_cluster >= 2 &&
                       entry.first_cluster < f->cluster_count + 2);

            if (!entry.is_dir) {
                /* Read it whole, then in chunks, and require the two to
                 * agree. The chunked path is the one that follows the cluster
                 * chain repeatedly, so a disagreement means a chain was
                 * walked inconsistently. */
                uint32_t got = 0;
                void *whole = fat16_read_whole(probe_paths[i], 1u << 20, &got);

                if (whole) {
                    assert(got == entry.size);

                    unsigned char *chunked = malloc(got ? got : 1);

                    if (chunked) {
                        uint32_t at = 0;
                        int ok = 1;

                        while (at < got) {
                            uint32_t want = got - at < 64 ? got - at : 64;
                            int32_t n =
                                fat16_read(&entry, at, chunked + at, want);

                            if (n != (int32_t)want) {
                                ok = 0;
                                break;
                            }
                            at += (uint32_t)n;
                        }

                        if (ok)
                            assert(memcmp(chunked, whole, got) == 0);

                        free(chunked);
                    }

                    free(whole);
                }

                /* A read at and past the end: short, then empty, never an
                 * error and never more than was asked for. */
                unsigned char tail[16];

                int32_t n = fat16_read(&entry, entry.size, tail, sizeof(tail));

                assert(n == 0);
                n = fat16_read(&entry, entry.size + 4096, tail, sizeof(tail));
                assert(n == 0);
            } else {
                /* Walk it. Bounded, because a directory whose chain loops
                 * must be caught by the driver and not by this loop. */
                for (uint32_t n = 0; n < 512; n++) {
                    struct fat_dirent child;

                    if (!fat16_readdir(probe_paths[i], n, &child))
                        break;

                    /* A name the driver reports has to be a name: 8.3 decodes
                     * to at most twelve characters plus a terminator, and
                     * anything else means the decoder wrote past its
                     * buffer. */
                    assert(strlen(child.name) < FAT_NAME_MAX);
                }
            }
        }
    }

    assert(shim_heap_outstanding() >= 0);

    return 0;
}
