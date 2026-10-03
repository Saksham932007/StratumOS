/* StratumOS - running kernel code on the host, for fuzzing.
 *
 * Every target in this directory compiles the *real* kernel source - the same
 * kernel/core/elf.c and kernel/fs/fat16.c that boot - and links it against
 * this shim, which supplies the handful of things those files expect a kernel
 * to provide. Nothing in the parsers is reimplemented or simplified, because a
 * fuzzer that found bugs in a rewritten copy would be finding bugs in the
 * rewrite.
 *
 * What the shim provides, and the reasoning for each:
 *
 *   kmalloc/kfree       the host allocator, so AddressSanitizer sees every
 *                       allocation, every free, and every byte either side of
 *                       them. This is most of the value: the kernel's own
 *                       heap has guard magics, but ASan has redzones, and a
 *                       one-byte overrun is invisible to the first and
 *                       immediate to the second.
 *
 *   panic()             abort(). A kernel panic is a crash the fuzzer should
 *                       report, not a graceful exit. That makes every
 *                       `panic()` in the kernel an assertion the fuzzer is
 *                       trying to violate, which is exactly the right
 *                       reading of them.
 *
 *   a page table        vmm_alloc_at() mmaps the page the kernel asked for,
 *                       at that address, with MAP_FIXED_NOREPLACE. So
 *                       elf_load_user() is fuzzed completely unmodified: its
 *                       `memcpy((void *)ph->vaddr, ...)` writes to genuinely
 *                       mapped memory, and a write past the end of what it
 *                       mapped hits an unmapped page and takes SIGSEGV -
 *                       which is exactly what the kernel would do. A stub
 *                       that returned true and discarded the writes would
 *                       make elf.c untestable.
 *
 *   logging             discarded by default, printed with STRATUM_FUZZ_VERBOSE
 *                       set. A fuzzer running a million inputs a minute does
 *                       not want the kernel's opinion on each one, but the
 *                       reproducer does.
 */
#ifndef _FUZZ_SHIM_H
#define _FUZZ_SHIM_H

#include <stddef.h>
#include <stdint.h>

/* Reset every piece of shim state between inputs. A fuzz target that leaks
 * state across inputs is a fuzz target whose crashes do not reproduce. */
void shim_reset(void);

/* How many pages the fake VMM currently has mapped, and how many bytes the
 * fake heap is holding. Both must come back to zero after a target that
 * cleans up - which is itself a thing worth fuzzing for, because a parser
 * that leaks on a rejection path leaks once per hostile input. */
size_t shim_mapped_pages(void);
size_t shim_heap_outstanding(void);

/* The buffer a block device reads from, for the filesystem target. */
void shim_set_disk(const uint8_t *data, size_t len);

/* The buffer vmm_map_mmio() hands out, for the ACPI target. Physical
 * addresses are translated into offsets within it, so a parser following a
 * pointer out of the table reads the fuzzer's bytes rather than the host's
 * memory - and reading past the end hits an ASan redzone. */
void shim_set_physmem(const uint8_t *data, size_t len);

#endif /* _FUZZ_SHIM_H */
