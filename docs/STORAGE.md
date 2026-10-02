# Storage: the disk, the partition table and the filesystem

`exec` used to resolve a program name against a table compiled into the
kernel. It now resolves it against a FAT16 filesystem on the disk the kernel
booted from, and `/bin/INIT` is read off that disk before ring 3 is entered.

Three layers, each with one job:

```
  fat16.c      a path -> a cluster chain -> bytes
     |
  blockdev.c   a partition -> an offset on a disk, bounds-checked
     |
  ata.c        a sector on a disk -> 256 words through a port
```

```
stratum> disk
ATA drives
  DEV  MODEL                           SECTORS    ADDR
  hd0  QEMU HARDDISK                     36864   LBA48
  2 read(s), 2 sector(s), 2 command(s), 0 error(s), 0 timeout(s)
Block devices
  NAME     TYPE        FIRST LBA      SECTORS
  hd0      disk                0        36864
  hd0p1    0e               2048        32768

stratum> mount
FAT16 "STRATUM" on hd0p1, mounted at /
  geometry  : 512-byte sectors, 4 per cluster (2 KiB clusters)
  layout    : 1 reserved, FAT at +1 (2 x 32 sectors), root at +65 (512 entries), data at +97
  size      : 32768 sectors, 8167 clusters
  activity  : 0 sector read(s), 0 cache hit(s), 0 lookup(s), 0 file(s) read whole

stratum> ls /bin
  NAME               SIZE  ATTR
  .                     0  d---
  ..                    0  d---
  HELLO              8752  ----
  INIT              12524  ----
  2 file(s), 21276 bytes; 2 director(y|ies)

stratum> cat /etc/MOTD.TXT
console=vga,serial
init=/bin/INIT
```

---

## One disk, not two

The kernel and the filesystem share a single disk image, with a real MBR
partition table in sector 0 describing where the filesystem starts:

```
  LBA 0          stage 1 - 315 bytes of code, then the partition table at 446
  LBA 1-24       stage 2
  LBA 25+        the kernel ELF
  LBA 2048+      partition 1: FAT16, 16 MiB
```

A second drive would have been less work and would have tested less. This way
the partition table has to be parsed correctly, and every filesystem read has
to be offset by the partition's start — which is the bug a separate disk
hides, because a filesystem at LBA 0 works whether or not the offset is being
added.

The partition table fits because stage 1 uses 315 of the 446 bytes available
before it. `mkimage` checks that rather than assuming it, and so does
`check-image.py`: the assembler cannot catch an overlap, because as far as it
is concerned the whole 510 bytes are free.

Only the LBA fields are written and only the LBA fields are read. The CHS
fields in a partition entry exist for BIOSes older than INT 13h extensions,
cannot describe anything past 8 GiB, and are routinely wrong on real disks. A
table where CHS and LBA disagree is normal; LBA is the one to believe.

---

## ATA, by programmed I/O

`kernel/drivers/ata.c`. PIO rather than DMA, polling rather than IRQ 14. Both
are the slow option and both are deliberate:

**PIO** moves every sector through the data register, 256 16-bit reads at a
time, with the CPU doing the carrying. Bus-mastering DMA needs a physical
region descriptor table, a scatter list of physical addresses and the
controller's own BAR — none of which can be debugged until the simple path
works.

**Polling** spins on the status register instead of sleeping on the interrupt.
On a real disk that burns the rest of a timeslice, which is why no real kernel
does it. What it buys is a read path that is a straight line: no state
machine, nothing to get wrong about a request that completes while its handler
is still being installed, and no question about what happens if two channels
share a line.

The driver sets `nIEN` on each channel so the drive does not assert its
interrupt at all. An unexpected IRQ 14 with no handler is a warning in the log
for no reason.

### The parts that are easy to get wrong

**The 400 ns settling delay.** After selecting a drive, the status register
cannot be trusted for 400 nanoseconds. The conventional way to spell that is
to read the *alternate* status register four times — four port reads take at
least that long on any bus that can run ATA, and reading that register has no
side effects, unlike the status register, which acknowledges the interrupt.

**A floating bus reads 0xFF.** Three of the four drive slots on a typical
machine are empty, and nothing is driving the lines. Checking for 0xFF before
issuing IDENTIFY avoids waiting out the full timeout on each one.

**ATAPI aborts IDENTIFY.** A CD-ROM is not a drive this driver can read, and
it says so by failing IDENTIFY and putting a signature (`0x14 0xEB`) in the
LBA mid and high registers. That is exactly what happens on the GRUB ISO boot
path, and distinguishing it from a real error is what lets the log say
"skipping an ATAPI device" rather than inventing a fault.

**One DRQ handshake per sector, not per command.** A multi-sector read waits
for DRQ before *each* sector's 256 words. Transferring all of them after a
single wait works under emulation and fails on hardware, which makes it one
of the worst bugs to have: the test suite therefore reads two sectors in one
command and requires the result to equal two single-sector reads — and
requires those two sectors to differ, so the comparison cannot pass on a
driver that returned the same sector twice.

**LBA48 writes each register twice.** Six bytes of address through three
8-bit ports: the high half first, then the low. The drive latches the previous
value when a register is written again. The driver only uses LBA48 when the
address needs it, because the LBA28 path is what every small disk takes.

**Every wait is bounded.** A drive that never clears BSY would otherwise hang
the kernel at boot with no message, which is the least debuggable failure a
driver can have. The bound is 3 million status reads — seconds under
emulation, and far short of the 30 seconds the specification allows for a
spin-up, which no test wants to wait for.

---

## The block layer

`kernel/fs/blockdev.c` is thin on purpose. It exists for two reasons.

**A filesystem lives in a partition, and a partition is an offset.** Without
this layer, every read in the FAT driver would have to remember to add the
partition's starting LBA. Forgetting once gives you a filesystem that reads
the wrong sectors and then blames its own metadata.

**A device knows its own length.** A filesystem that follows a corrupt
pointer is stopped here, with a message naming the request:

```
blk: hd0p1: read of 1 blocks at 32768 runs past the device's 32768
```

rather than at the drive, which can only say "sector not found", or not at
all. The test suite reads past the end of the partition and requires the
refusal — and requires it to be *logged*, because a silent refusal leaves
whoever hits it with nothing to read.

The partition scan registers the whole disk as well as each partition, so
`hd0` and `hd0p1` are both addressable and the test suite can prove that a
read of `hd0p1` block 0 returns the same bytes as a read of `hd0` at the
partition's first LBA. That is the check that the offset is being applied.

---

## FAT16

`kernel/fs/fat16.c`, read-only.

FAT16 rather than FAT12 or FAT32 because it is the smallest version with all
of the ideas and none of the busywork. FAT12's entries are twelve bits, so
every other one straddles a byte boundary — a fiddly decoder that teaches
nothing. FAT32 moves the root directory into the data area, adds an FSInfo
sector and reserves the top four bits of every entry: more bookkeeping, same
concepts.

Read-only because writing is a different problem. A correct write allocates
from the FAT, updates both copies of it, extends a directory entry's size and
cluster chain, and survives being interrupted between any two of those —
which is a journalling discussion, not a filesystem-format one. Reading is
what `exec` needs.

### The shape of it

The boot sector (the BPB) gives the geometry, from which four offsets follow:

```
  reserved          the boot sector
  FAT 1, FAT 2      sectors_per_fat each
  root directory    root_entries * 32 bytes
  data              clusters 2..N
```

A file is a starting cluster and a length. The FAT turns the starting cluster
into a chain by holding, at index N, the number of the cluster that follows N
— a linked list stored as an array of indices. 0xFFF8 and above mean the end.

Everything else is arithmetic on those four offsets.

### Validation is not optional here

Every number that comes off the disk is checked before it is used as an index
or a length, and the reason is specific: a FAT image is data somebody else may
have written, the cluster chain's pointers live inside it, and a chain that
loops back on itself is the easiest way to hang a driver that trusts it.

So:

- **The BPB is validated before anything is computed from it.** Every field
  below is a divisor or a multiplier: a zero is a crash and a large value is
  an out-of-range read.
- **FAT16 is decided by cluster count, not by the `fs_type` string.** That
  string is a comment and is routinely wrong. Below 4085 clusters the entries
  are 12 bits wide and this driver would misread every one, so it refuses
  rather than misreading.
- **The FAT has to be long enough for its own cluster count.** A table
  shorter than that means following a chain reads off the end of it.
- **Every chain walk is bounded** by the filesystem's cluster count. A chain
  cannot legitimately be longer than that, so exceeding it is proof of
  corruption rather than a guess.
- **`fat16_read_whole` takes a maximum.** The size comes from a directory
  entry, so without a bound an entry claiming 4 GiB is a `kmalloc` of 4 GiB.

### Names

The on-disk form is eleven bytes with no dot: eight of stem, three of
extension, each space-padded. The dot exists only in the presentation, which
is why `README.TXT` and `README  TXT` are the same name, and why comparing
the undecoded bytes is the wrong way to look a file up.

Comparison is case-insensitive, because FAT is — and because `exec("/bin/init")`
has to find `INIT`.

Long filenames are not supported. They are a chain of pseudo-entries carrying
UTF-16 fragments plus a checksum over the short name, tagged with an attribute
byte (`0x0F`) that a driver which does not understand them skips. This driver
skips them, which is the correct behaviour for a driver that does not
implement them.

### The cache, and what measuring it changed

The access pattern needs a cache. Walking a directory reads 16 entries out of
every sector; following a cluster chain reads one 16-bit FAT entry out of a
sector holding 256. Without one, resolving `/bin/INIT` issues a disk read per
directory entry examined and per cluster followed.

The first version had **one** sector of cache, with a comment confidently
explaining that a second entry would buy nothing. The test suite disagreed.
It reads a file in 64-byte chunks, which touches each 512-byte sector eight
times, and asserted that seven of every eight reads should be cache hits.
They were not: the hit rate was under half.

The reason, once measured, was obvious. Reading a file alternates between FAT
sectors (to follow the chain) and data sectors (to copy bytes out), so each
one evicted the other on every step.

The fix is two slots divided by *purpose* rather than by recency — one for the
allocation table, one for everything else:

```c
enum cache_slot {
    CACHE_FAT = 0,  /* sectors of the allocation table */
    CACHE_DATA = 1, /* directory entries and file contents */
};
```

The two streams are independent and both sequential, so splitting by purpose
needs no replacement policy and makes it impossible for one to starve the
other. The hit rate went from 47% to 89% on the same workload:

```
  before:  360 sector reads, 321 cache hits
  after:   150 sector reads, 1224 cache hits
```

A third slot would buy nothing — there is no third stream. The point is that
the first comment was wrong in a way only a measurement could show, and the
test that showed it was written to check the mechanism rather than the
outcome.

---

## Where a program comes from

`load_program()` in `kernel/core/usermode.c` is the only place that knows:

| `name` | Where it looks |
|---|---|
| `/bin/HELLO` | that path, and nowhere else |
| `hello` | `/bin/hello` on the filesystem, then the embedded table |

An absolute path that does not resolve is an error, not an invitation to run
something with a similar name. A bare name falls back, and that fallback is
not vestigial: **the GRUB ISO boot path has no FAT partition at all**, because
its only drive is an ATAPI CD-ROM. Without the embedded copies, that path
could not run a user program, and the same tests could not cover both loaders.

```
# from the raw disk image
user: pid 4 entering ring 3 at 0x004000f0 in its own address space
      (7 user pages mapped, image from the filesystem)
user: pid 7 exec("hello") from the filesystem: replacing 7 user pages

# from the GRUB ISO
ata: no ATA drives found
blk: no block devices
boot: filesystem         [ok] none present; using the embedded programs
user: pid 4 entering ring 3 ... (image from the kernel image)
```

CI asserts on both. `programs` reports which was used:

```
stratum> programs
Programs embedded in the kernel image (exec's namespace):
  init   (started at boot)
  hello
Programs on the filesystem (what exec() prefers):
  /bin/HELLO            8752 bytes
  /bin/INIT            12524 bytes
  1 exec() call(s) this boot; 2 image(s) loaded from disk, 0 from the kernel image
```

A non-zero "from the kernel image" on a kernel booted from a disk would mean a
file is missing from the filesystem — which is the kind of thing that
otherwise shows up as a program being mysteriously out of date.

---

## Building the filesystem, and checking it

`tools/mkfat.py` writes the image rather than driving `mformat`/`mcopy`. Two
reasons, and the second is the real one.

Dependencies: mtools is one more thing that has to be installed, and the
kernel's test suite asserts on specific bytes in specific clusters — a
formatter whose layout can drift between versions is a poor foundation for
that.

And writing it is the point. The driver has to parse a BPB, walk a FAT, follow
a chain and decode 8.3 names; writing the producer means every one of those
fields was chosen on purpose rather than accepted from a tool. When the driver
and the formatter disagree, one of them is wrong and both are readable.

It refuses rather than mangles: a name that does not fit 8.3 is a build error,
because a formatter that silently truncated `initialise` to `INITIAL` would
produce an image whose contents do not match the tree it was built from, and
the resulting bug would look like a driver bug. Timestamps are fixed rather
than taken from the clock, so the same tree always produces the same image —
a build whose output changes when nothing changed makes it impossible to tell
whether a test is asserting on content or on a date.

The image is then verified independently. `tools/mkfat.py`'s output is read
back by `mdir`/`mtype` during development, and `tools/check-image.py` runs on
every build:

```
$ make image-check
    stage 1: 315 of 446 bytes before the partition table
    partition 1: type 0x0e, LBA 2048+32768 (16384 KiB)
    stage 2: kernel at LBA 25, 377 sectors, cmdline "console=vga,serial"
    filesystem: FAT16 "STRATUM", 32768 sectors, 8167 clusters of 2 KiB
      /BIN/HELLO: 8752 bytes, ELF32 i386, entry 0x400000
      /BIN/INIT: 12524 bytes, ELF32 i386, entry 0x4000f0
    root: BIN, ETC, README.TXT
```

It walks the partition table, the stage 2 header, the BPB, the FAT and the
`/BIN` directory, follows each file's cluster chain, and requires every file
in `/BIN` to be an ELF32 i386 executable whose entry point is in user space —
with `INIT` present, because that is the program the kernel starts.

That last check is the whole point of the script. A build that produces an
image whose `/BIN/INIT` is missing or truncated boots perfectly and then fails
in ring 3, which is the hardest place to debug it from.

---

## Testing

Two suites, and they run against whatever the kernel actually booted from.
That matters because the two boot paths are genuinely different: the raw disk
image has an ATA drive with an MBR and a FAT partition, the GRUB ISO has an
ATAPI CD-ROM and neither. One suite has to pass on both, so each group checks
either the real thing or the absence of it — never "skip", which is how a
test suite quietly stops testing anything.

| Suite | What it establishes |
|---|---|
| `storage` | IDENTIFY reports a model and a capacity; sector 0 ends in 0xAA55; a two-sector read equals two one-sector reads *and* the two sectors differ; reads past the end, straddling the end, wrapping the address space, of a nonexistent drive and into a null buffer are each refused *and* logged; zero sectors is a no-op that logs nothing; the partition lies inside the disk; a read of the partition's block 0 equals a read of the disk at the partition's first LBA; the partition's first sector says FAT16; a read past the partition's end is refused even though those sectors exist |
| `fs` | the BPB's four offsets are ordered and inside the device; the cluster count is in FAT16's range; the root has no cluster number; a directory and a file inside it resolve; lookup is case-insensitive and tolerates redundant slashes; a missing name, a relative path, an over-long component, a file used as a directory and a directory read as a file are all refused; a chunked read agrees with a whole-file read byte for byte, including across a cluster boundary; a read at the end is short and past the end is zero; a size bound the file exceeds refuses rather than truncates; the volume label is not reported as a file; and the cache's hit rate on an eight-reads-per-sector workload |

### Errors that are supposed to happen

Several of those checks provoke an error, correctly — and CI treats an
unexpected `ERROR` line as a failure, also correctly. Rather than soften the
messages, a test can open a window in which they are counted instead of
printed:

```c
log_expect_errors(true);
KT_ASSERT(r, !ata_read(0, d->sectors, 1, buf));          /* past the end */
KT_ASSERT(r, !ata_read(0, d->sectors - 1, 2, buf));      /* straddles it */
...
u32 complaints = log_expected_errors();
log_expect_errors(false);
KT_EQ(r, complaints, 5u);
```

Counting is stronger than suppressing: five refusals must produce five
complaints, so a driver that refused *silently* fails this test. A refusal
nobody can see is nearly as bad as no refusal at all.

A lookup that fails to find a file is deliberately *not* in that category —
it logs at debug level, because a shell that probes for a file would be
unusable otherwise, and the suite checks those refusals without a window to
prove it.
