/* StratumOS - ELF32 loader for user programs.
 *
 * See kernel/include/kernel/elf.h for why this is written defensively: unlike
 * the bootloader's loader, this one may be handed an image the kernel did not
 * produce, and every field in an ELF header is an offset or a length that
 * something will be indexed by.
 */
#define LOG_TAG "elf"

#include <arch/harden.h>

#include <kernel/elf.h>
#include <kernel/layout.h>
#include <kernel/log.h>
#include <kernel/string.h>

#include <mm/pmm.h>
#include <mm/vmm.h>

/* A program image larger than this is refused outright, which bounds every
 * subsequent arithmetic check. */
#define ELF_MAX_IMAGE (8 * MIB)

bool elf_validate(const void *image, size_t size, const char **why)
{
    const struct elf32_header *eh = image;

#define REJECT(msg)       \
    do {                  \
        if (why)          \
            *why = (msg); \
        return false;     \
    } while (0)

    if (!image)
        REJECT("no image");
    if (size < sizeof(struct elf32_header))
        REJECT("shorter than an ELF header");
    if (size > ELF_MAX_IMAGE)
        REJECT("larger than the maximum program image");

    if (eh->magic != ELF_MAGIC)
        REJECT("not an ELF file");
    if (eh->class != 1)
        REJECT("not ELFCLASS32");
    if (eh->data != 1)
        REJECT("not little-endian");
    if (eh->type != ET_EXEC)
        REJECT("not ET_EXEC - a PIE or shared object needs a dynamic loader");
    if (eh->machine != EM_386)
        REJECT("not EM_386");

    if (eh->phentsize < sizeof(struct elf32_phdr))
        REJECT("program header entries are too small");
    if (eh->phnum == 0)
        REJECT("no program headers");

    /* The program header table must lie entirely inside the image. Checked
     * with division rather than multiplication so the product cannot
     * overflow. */
    if (eh->phoff > size)
        REJECT("program header table starts past the end of the image");
    if ((size - eh->phoff) / eh->phentsize < eh->phnum)
        REJECT("program header table runs past the end of the image");

    return true;
#undef REJECT
}

static const struct elf32_phdr *phdr_at(const void *image, u32 index)
{
    const struct elf32_header *eh = image;

    return (const struct elf32_phdr *)((const u8 *)image + eh->phoff +
                                       (size_t)index * eh->phentsize);
}

/* Is this segment's address range acceptable for a user program? */
static bool segment_range_ok(const struct elf32_phdr *ph, const char **why)
{
    u32 start = ph->vaddr;
    u32 end;

    if (ph->memsz == 0)
        return true;

    /* Wrapping the address space would make every subsequent comparison
     * meaningless. */
    if (ph->vaddr + ph->memsz < ph->vaddr)
        goto wrap;

    end = ph->vaddr + ph->memsz;

    /* The null page stays unmapped so that a NULL dereference faults. */
    if (start < PAGE_SIZE)
        goto too_low;

    /* The decisive check: a user segment may not touch the kernel's half. An
     * image claiming vaddr 0xC0100000 is asking the kernel to overwrite
     * itself on the program's behalf. */
    if (is_kernel_address(start) || is_kernel_address(end - 1))
        goto kernel_space;

    return true;

wrap:
    if (why)
        *why = "a segment's address range wraps the address space";
    return false;
too_low:
    if (why)
        *why = "a segment overlaps the null page";
    return false;
kernel_space:
    if (why)
        *why = "a segment would be mapped into kernel address space";
    return false;
}

bool elf_load_user(const void *image, size_t size, struct elf_load_info *out)
{
    const struct elf32_header *eh = image;
    const char *why = NULL;
    struct elf_load_info info;

    if (!elf_validate(image, size, &why)) {
        pr_err("refusing to load: %s", why ? why : "malformed");
        return false;
    }

    memset(&info, 0, sizeof(info));
    info.entry = eh->entry;
    info.image_low = 0xFFFFFFFFu;

    /* --- validate every segment before mapping any of them --------------
     * A half-loaded program is harder to clean up than a rejected one, and
     * leaves the address space in a state the caller cannot describe. */
    for (u32 i = 0; i < eh->phnum; i++) {
        const struct elf32_phdr *ph = phdr_at(image, i);

        if (ph->type != PT_LOAD)
            continue;

        if (!segment_range_ok(ph, &why)) {
            pr_err("refusing to load: %s", why);
            return false;
        }

        if (ph->filesz > ph->memsz) {
            pr_err("refusing to load: a segment's file size exceeds its "
                   "memory size");
            return false;
        }

        if (ph->offset > size || size - ph->offset < ph->filesz) {
            pr_err("refusing to load: a segment's contents run past the end "
                   "of the image");
            return false;
        }
    }

    if (is_kernel_address(info.entry)) {
        pr_err("refusing to load: the entry point is in kernel space");
        return false;
    }

    /* --- map and copy ---------------------------------------------------- */
    for (u32 i = 0; i < eh->phnum; i++) {
        const struct elf32_phdr *ph = phdr_at(image, i);

        if (ph->type != PT_LOAD || ph->memsz == 0)
            continue;

        vaddr_t first = PAGE_TRUNC(ph->vaddr);
        vaddr_t last = PAGE_ALIGN(ph->vaddr + ph->memsz);

        for (vaddr_t page = first; page < last; page += PAGE_SIZE) {
            /* Segments can share a page when they are not page-aligned, so
             * only allocate one that is not already present. */
            if (vmm_translate(page, NULL))
                continue;

            /* Mapped writable for now regardless of the segment's flags: the
             * contents have to be copied in. Permissions are tightened once
             * every segment has been written. */
            if (!vmm_alloc_at(page, PTE_PRESENT | PTE_WRITE | PTE_USER)) {
                pr_err("out of memory loading segment %u", i);
                elf_unload_user(&info);
                return false;
            }

            /* Writing a user page from ring 0, which is exactly what SMAP
             * forbids by default. The window covers one page and closes
             * immediately. */
            user_access_begin();
            memset((void *)page, 0, PAGE_SIZE);
            user_access_end();

            info.pages++;

            /* Widen the recorded range as each page is mapped, not once per
             * segment.
             *
             * The failure path above calls elf_unload_user(), which unmaps
             * image_low..image_high - and if that range is still its initial
             * empty value, every page mapped so far leaks. A hostile image
             * with many segments can provoke that deliberately and leak on
             * every attempt, which is a denial of service that repeats.
             *
             * Found by a fuzz target, which asserts that nothing is left
             * mapped whether the load succeeded or failed. The accounting had
             * been one statement too late since the loader was written. */
            if (page < info.image_low)
                info.image_low = page;
            if (page + PAGE_SIZE > info.image_high)
                info.image_high = page + PAGE_SIZE;
        }

        if (ph->filesz) {
            user_access_begin();
            memcpy((void *)(uptr)ph->vaddr, (const u8 *)image + ph->offset,
                   ph->filesz);
            user_access_end();
        }

        /* The rest of memsz is the .bss tail; the pages were zeroed above. */

        /* A segment whose pages were all already mapped by an earlier one
         * still has to be inside the recorded range. */
        if (first < info.image_low)
            info.image_low = first;
        if (last > info.image_high)
            info.image_high = last;

        info.segments++;
    }

    if (info.segments == 0) {
        pr_err("refusing to load: no loadable segments");
        return false;
    }

    /* The entry point has to be inside something that was actually mapped.
     *
     * Checking it is below KERNEL_VIRT_BASE - which happens earlier - stops a
     * crafted image asking the kernel to jump into itself, and that is the
     * check that matters for security. This one is about not loading a
     * program that cannot possibly run: an entry point outside every segment
     * means usermode_enter() IRETs to an unmapped address, and the process
     * takes a page fault on its first instruction.
     *
     * A fuzz target found this, and what made it worth fixing was where it
     * led: the page-fault handler panicked on *any* unresolved fault,
     * including one taken in ring 3, so a malformed ELF took the machine
     * down. That is fixed too, and independently - a program should not be
     * able to panic a kernel whatever its entry point says. But a loader
     * that accepts an image it knows cannot start is still a loader doing
     * the wrong thing. */
    if (info.entry < info.image_low || info.entry >= info.image_high ||
        !vmm_translate(info.entry, NULL)) {
        pr_err("refusing to load: the entry point %p is not inside any "
               "segment this image maps (%p-%p)",
               (void *)info.entry, (void *)info.image_low,
               (void *)info.image_high);
        elf_unload_user(&info);
        return false;
    }

    /* --- apply the real permissions -------------------------------------
     * Done as a second pass so that a page shared between a read-only and a
     * writable segment ends up writable rather than depending on which
     * segment was processed last. */
    for (u32 i = 0; i < eh->phnum; i++) {
        const struct elf32_phdr *ph = phdr_at(image, i);

        if (ph->type != PT_LOAD || ph->memsz == 0)
            continue;
        if (ph->flags & PF_W)
            continue; /* leave writable segments writable */

        vaddr_t first = PAGE_TRUNC(ph->vaddr);
        vaddr_t last = PAGE_ALIGN(ph->vaddr + ph->memsz);

        for (vaddr_t page = first; page < last; page += PAGE_SIZE) {
            bool shared_with_writable = false;

            for (u32 j = 0; j < eh->phnum; j++) {
                const struct elf32_phdr *other = phdr_at(image, j);

                if (j == i || other->type != PT_LOAD || other->memsz == 0)
                    continue;
                if (!(other->flags & PF_W))
                    continue;
                if (page < PAGE_ALIGN(other->vaddr + other->memsz) &&
                    page + PAGE_SIZE > PAGE_TRUNC(other->vaddr))
                    shared_with_writable = true;
            }

            if (!shared_with_writable)
                vmm_protect(page, PTE_PRESENT | PTE_USER);
        }
    }

    if (out)
        *out = info;

    pr_info("loaded a %u-segment program: entry %p, %u pages mapped "
            "%p-%p",
            info.segments, (void *)info.entry, info.pages,
            (void *)info.image_low, (void *)info.image_high);
    return true;
}

void elf_unload_user(const struct elf_load_info *info)
{
    if (!info || info->image_high <= info->image_low)
        return;

    for (vaddr_t page = info->image_low; page < info->image_high;
         page += PAGE_SIZE)
        vmm_unmap(page);
}
