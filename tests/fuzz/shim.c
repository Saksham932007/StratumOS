/* StratumOS - the host-side shim the fuzz targets link against.
 *
 * See tests/fuzz/shim.h for what each piece is for and why.
 */
#define _GNU_SOURCE

#include "shim.h"

#include <stdarg.h>

#include <arch/harden.h>

#include <kernel/layout.h>
#include <kernel/log.h>
#include <kernel/panic.h>
#include <kernel/types.h>

#include <mm/heap.h>
#include <mm/pmm.h>
#include <mm/vmm.h>

#include <fs/blockdev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* Interrupt state, modelled rather than ignored - see arch/io.h. A lock that
 * forgets to restore it is still a bug a target can assert on. */
int stratum_fuzz_interrupts_enabled = 1;

/* ---- logging ----------------------------------------------------------- */

static int verbose = -1;

static bool log_wanted(void)
{
    if (verbose < 0)
        verbose = getenv("STRATUM_FUZZ_VERBOSE") != NULL;
    return verbose != 0;
}

void log_emit(enum log_level level, const char *tag, const char *fmt, ...)
{
    if (!log_wanted())
        return;

    va_list ap;

    fprintf(stderr, "[%s] %s: ", log_level_name(level), tag);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* The formatter writes through this. Discarded unless the reproducer asked
 * for output: a target running a million inputs a minute does not want the
 * kernel's opinion on each one. */
void console_putc(char c)
{
    if (log_wanted())
        fputc(c, stderr);
}

void console_write(const char *s, size_t n)
{
    if (log_wanted())
        fwrite(s, 1, n, stderr);
}

void console_puts(const char *s)
{
    if (log_wanted())
        fputs(s, stderr);
}

enum log_level log_get_level(void)
{
    return LOG_DEBUG;
}

const char *log_level_name(enum log_level level)
{
    static const char *const names[] = {"PANIC", "ERROR", "WARN",
                                        "INFO",  "DEBUG", "TRACE"};

    return level <= LOG_TRACE ? names[level] : "?";
}

void log_expect_errors(bool on)
{
    (void)on;
}

u32 log_expected_errors(void)
{
    return 0;
}

/* ---- panic ------------------------------------------------------------- */

/* A panic is a crash the fuzzer should report. Every `panic()` in the kernel
 * therefore becomes an assertion a fuzz target is trying to violate, which is
 * the right reading of them: they are all statements of the form "this cannot
 * happen", and the fuzzer's job is to disagree. */
NORETURN void panic(const char *fmt, ...)
{
    va_list ap;

    fputs("\n*** KERNEL PANIC (reached from a fuzz target) ***\n", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);

    abort();
}

void panic_assert_failed(const char *file, int line, const char *expr)
{
    fprintf(stderr, "\n*** ASSERT FAILED %s:%d: %s ***\n", file, line, expr);
    abort();
}

bool vmm_is_enabled(void)
{
    /* The heap asks, so that it can refuse to initialise before paging. On
     * the host paging is somebody else's problem and the answer is yes. */
    return true;
}

/* ---- the heap ---------------------------------------------------------- */

/* Normally the host allocator, so AddressSanitizer sees every allocation and
 * the redzones either side of it - the kernel's own heap has guard magics,
 * which catch an overrun of four bytes or more, and ASan catches one byte.
 *
 * The exception is the target that fuzzes the heap itself, which obviously
 * has to use the real one. STRATUM_FUZZ_REAL_HEAP picks that, and the
 * consequence is worth stating: in that target an overrun inside the arena is
 * invisible to ASan, because the arena is one big legitimate mapping. That is
 * precisely why fuzz_heap.c tags every block with a pattern and rechecks it -
 * the pattern is what catches two allocations overlapping, and no sanitizer
 * can. */
/* Outside the conditional, because shim_heap_outstanding() is part of the
 * shim's interface either way - it just stays zero when the real allocator
 * is in use. */
static size_t heap_outstanding;
static size_t heap_live_blocks;

#ifndef STRATUM_FUZZ_REAL_HEAP

void *kmalloc(size_t size)
{
    if (size == 0)
        return NULL;

    /* A bound, so that a parser tricked into asking for a gigabyte reports
     * "out of memory" - which is a path worth exercising - rather than making
     * the fuzzer swap. */
    if (size > 16u * 1024 * 1024)
        return NULL;

    void *p = malloc(size);

    if (p) {
        heap_outstanding += size;
        heap_live_blocks++;
        /* Deliberately not zeroed: kmalloc does not promise it, so a caller
         * that depends on it has a bug, and poisoning makes that bug
         * visible. */
        memset(p, 0xA5, size);
    }

    return p;
}

void *kmalloc_aligned(size_t size, size_t align)
{
    void *p = NULL;

    if (align < sizeof(void *))
        align = sizeof(void *);

    if (posix_memalign(&p, align, size ? size : 1) != 0)
        return NULL;

    heap_outstanding += size;
    heap_live_blocks++;
    memset(p, 0xA5, size);

    return p;
}

void kfree(void *p)
{
    if (!p)
        return;

    heap_live_blocks--;
    free(p);
}

void *krealloc(void *p, size_t size)
{
    if (size == 0) {
        kfree(p);
        return NULL;
    }

    return realloc(p, size);
}

void heap_init(void)
{
}

unsigned heap_check(void)
{
    return 0;
}

void heap_get_stats(struct heap_stats *out)
{
    if (out)
        memset(out, 0, sizeof(*out));
}

#endif /* !STRATUM_FUZZ_REAL_HEAP */

/* ---- a page table ------------------------------------------------------ */

/* Mapped pages, each backed by a real host mapping *at the address the kernel
 * asked for*.
 *
 * This is the piece that makes the ELF loader fuzzable without touching it.
 * elf_load_user() does `memcpy((void *)ph->vaddr, ...)` after mapping a page,
 * and on the host that address has to genuinely be writable memory for the
 * copy to behave the way it does in the kernel. So vmm_alloc_at() mmaps the
 * exact page, with MAP_FIXED_NOREPLACE so that a request landing on the
 * fuzzer's own code, heap or sanitizer shadow fails rather than destroying
 * the process - which the loader then reports as out of memory, a path worth
 * exercising anyway.
 *
 * The consequence is better than a sanitizer redzone: a write past the end of
 * the mapped region hits an unmapped page and the process takes SIGSEGV,
 * which is exactly what the kernel would do. The loader's bounds are checked
 * by the same mechanism that would check them on real hardware.
 */
#define SHIM_MAX_PAGES 4096

struct shim_page {
    vaddr_t va;
    u32 flags;
};

static struct shim_page pages[SHIM_MAX_PAGES];
static size_t page_count;

size_t shim_mapped_pages(void)
{
    return page_count;
}

static struct shim_page *find_page(vaddr_t va)
{
    vaddr_t base = va & ~(vaddr_t)0xFFF;

    for (size_t i = 0; i < page_count; i++)
        if (pages[i].va == base)
            return &pages[i];

    return NULL;
}

bool vmm_alloc_at(vaddr_t va, u32 flags)
{
    vaddr_t base = va & ~(vaddr_t)0xFFF;

    if (find_page(base))
        return false; /* already mapped: the kernel's VMM warns and refuses */

    if (page_count >= SHIM_MAX_PAGES)
        return false; /* out of memory, which is a path worth exercising */

    /* The null page is never mapped in the kernel, and mapping it here would
     * turn a NULL dereference in the code under test into a silent success. */
    if (base == 0)
        return false;

    void *got = mmap((void *)(uintptr_t)base, 0x1000, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

    if (got == MAP_FAILED || (vaddr_t)(uintptr_t)got != base) {
        /* Occupied by the host's own memory. Reporting failure is both safe
         * and faithful: the kernel's allocator can fail too. */
        if (got != MAP_FAILED)
            munmap(got, 0x1000);
        return false;
    }

    pages[page_count].va = base;
    pages[page_count].flags = flags;
    page_count++;

    return true;
}

bool vmm_map(vaddr_t va, paddr_t pa, u32 flags)
{
    (void)pa;
    return vmm_alloc_at(va, flags);
}

void vmm_unmap(vaddr_t va)
{
    struct shim_page *p = find_page(va);

    if (!p)
        return;

    munmap((void *)(uintptr_t)p->va, 0x1000);
    *p = pages[--page_count];
}

bool vmm_translate(vaddr_t va, paddr_t *out)
{
    struct shim_page *p = find_page(va);

    if (!p)
        return false;

    if (out)
        *out = (paddr_t)p->va;

    return true;
}

u32 vmm_pte(vaddr_t va)
{
    struct shim_page *p = find_page(va);

    return p ? (p->flags | 1u) : 0;
}

bool vmm_protect(vaddr_t va, u32 flags)
{
    struct shim_page *p = find_page(va);

    if (!p)
        return false;

    p->flags = flags;

    /* Honour it, so that a loader which marks a segment read-only and then
     * writes to it takes a fault here as it would in the kernel. */
    int prot = PROT_READ | ((flags & PTE_WRITE) ? PROT_WRITE : 0);

    mprotect((void *)(uintptr_t)p->va, 0x1000, prot);

    return true;
}

bool vmm_protect_range(vaddr_t va, size_t bytes, u32 flags)
{
    bool all = true;

    for (vaddr_t at = va & ~(vaddr_t)0xFFF; at < va + bytes; at += 0x1000)
        if (!vmm_protect(at, flags))
            all = false;

    return all;
}

/* ---- block device ------------------------------------------------------ */

static const u8 *disk_data;
static size_t disk_len;
static struct blockdev shim_dev;

void shim_set_disk(const uint8_t *data, size_t len)
{
    disk_data = data;
    disk_len = len;

    memset(&shim_dev, 0, sizeof(shim_dev));
    shim_dev.present = true;
    strcpy(shim_dev.name, "fuzz0");
    shim_dev.drive = 0;
    shim_dev.first_lba = 0;
    shim_dev.sectors = len / BLOCK_SIZE;
}

const struct blockdev *shim_device(void);
const struct blockdev *shim_device(void)
{
    return &shim_dev;
}

bool blockdev_read(const struct blockdev *dev, u64 offset, u32 count, void *buf)
{
    (void)dev;

    /* The same bounds check the real block layer does, because a filesystem
     * that reads past the end of its partition has to be refused here rather
     * than discovered by ASan - the refusal is the behaviour being tested. */
    if (offset + count > shim_dev.sectors || offset + count < offset)
        return false;

    memcpy(buf, disk_data + offset * BLOCK_SIZE, (size_t)count * BLOCK_SIZE);
    return true;
}

/* ---- physical memory, for the ACPI target ------------------------------ */

static const u8 *physmem;
static size_t physmem_len;

void shim_set_physmem(const uint8_t *data, size_t len)
{
    physmem = data;
    physmem_len = len;
}

/* A physical address is an offset into the fuzzer's buffer. A parser that
 * follows a pointer out of a table reads the fuzzer's bytes - and one that
 * reads past the end hits an ASan redzone, which is the whole point. */
void *phys_to_virt_shim(paddr_t p);
void *phys_to_virt_shim(paddr_t p)
{
    if (!physmem || p >= physmem_len)
        return NULL;

    return (void *)(uintptr_t)(physmem + p);
}

void *vmm_map_mmio(paddr_t phys, size_t bytes, bool uncached)
{
    (void)uncached;

    if (!physmem || phys >= physmem_len)
        return NULL;

    /* Refuse a mapping that would run off the end of the buffer, the way the
     * real window refuses one that would not fit. Returning a short mapping
     * instead would let a parser read host memory and blame the kernel. */
    if (bytes > physmem_len - phys)
        return NULL;

    return (void *)(uintptr_t)(physmem + phys);
}

u32 vmm_mmio_used(void)
{
    return 0;
}

/* ---- hardening stubs --------------------------------------------------- */

static struct harden_state harden_dummy;

const struct harden_state *harden_get_state(void)
{
    return &harden_dummy;
}

void user_access_begin(void)
{
}

void user_access_end(void)
{
}

/* ---- physical frames --------------------------------------------------- */

paddr_t pmm_alloc_frame(void)
{
    void *p = NULL;

    if (posix_memalign(&p, 0x1000, 0x1000) != 0)
        return PMM_NO_FRAME;

    memset(p, 0, 0x1000);
    return (paddr_t)(uintptr_t)p;
}

void pmm_free_frame(paddr_t frame)
{
    free((void *)(uintptr_t)frame);
}

void pmm_frame_ref(paddr_t frame)
{
    (void)frame;
}

u8 pmm_frame_refs(paddr_t frame)
{
    (void)frame;
    return 1;
}

/* ---- reset ------------------------------------------------------------- */

size_t shim_heap_outstanding(void)
{
    return heap_outstanding;
}

void shim_reset(void)
{
    for (size_t i = 0; i < page_count; i++)
        munmap((void *)(uintptr_t)pages[i].va, 0x1000);

    page_count = 0;
    heap_outstanding = 0;
    heap_live_blocks = 0;
    disk_data = NULL;
    disk_len = 0;
    physmem = NULL;
    physmem_len = 0;
}
