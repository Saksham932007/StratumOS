/* StratumOS - fuzzing the ring-3 ELF loader.
 *
 * kernel/core/elf.c is the one parser in this kernel that is explicitly
 * documented as handling input the kernel did not produce, so it is the
 * obvious first target. The fuzzer's bytes go in as a complete ELF image.
 *
 * Nothing about elf.c is modified or stubbed. The shim's vmm_alloc_at()
 * mmaps each page at the address the loader asked for, so the loader's
 * `memcpy((void *)ph->vaddr, ...)` writes to real memory at the real address,
 * and a copy that runs past what it mapped takes a fault here exactly as it
 * would on hardware.
 *
 * What a crash here means:
 *
 *   SIGSEGV          the loader wrote outside a page it had mapped, or
 *                    dereferenced something it had not validated
 *   ASan report      the loader read past the end of the image buffer, which
 *                    is the classic ELF parsing bug: trusting e_phoff,
 *                    e_phnum or p_filesz against the file's actual length
 *   abort()          the loader reached a panic(), i.e. violated one of its
 *                    own "this cannot happen" assertions
 *   a leak           pages still mapped after a rejection, which would mean a
 *                    hostile image costs memory every time it is refused
 */
#include <stdint.h>

#include <kernel/elf.h>

#include "shim.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* The loader is told the size, so there is no terminator to find and no
     * reason to bound the input here beyond keeping the corpus sensible. */
    if (size < 4 || size > 256 * 1024)
        return 0;

    shim_reset();

    struct elf_load_info info;

    if (elf_load_user(data, size, &info)) {
        /* It accepted the image. Then its own report has to be
         * self-consistent, because everything downstream trusts it: the
         * entry point is where usermode_enter() jumps, and the page range is
         * what elf_unload_user() will release. */
        assert(info.segments > 0);
        assert(info.pages > 0);
        assert(info.image_low < info.image_high);
        assert(info.entry >= info.image_low);
        assert(info.entry < info.image_high);

        /* And the properties the loader exists to guarantee. A hostile image
         * must not be able to get a mapping in kernel space or over the null
         * page, whatever it claims. */
        assert(info.image_low >= 0x1000);
        assert(info.image_high <= 0xC0000000u);
        assert(info.entry < 0xC0000000u);

        elf_unload_user(&info);
    }

    /* Accepted or rejected, nothing may be left mapped. A parser that leaks
     * on its rejection path leaks once per hostile input, which is a denial
     * of service rather than a memory-safety bug - and is invisible to a
     * sanitizer, because the pages were legitimately allocated. */
    assert(shim_mapped_pages() == 0);

    return 0;
}
