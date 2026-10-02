/* StratumOS - ELF32 loader for user programs.
 *
 * The bootloader already parses ELF program headers to load the kernel; this
 * does the same job on the other side of the privilege boundary, mapping a
 * program's segments into user-accessible pages of the current address space.
 *
 * The difference that matters is trust. Stage 2 loads a kernel the build
 * produced; this loads a program that, in general, the kernel did not write.
 * So every field is bounds-checked against the image, and no segment is
 * allowed to land at or above KERNEL_VIRT_BASE - otherwise a crafted ELF
 * would be a request to overwrite the kernel.
 */
#ifndef _KERNEL_ELF_H
#define _KERNEL_ELF_H

#include <kernel/types.h>

#define ELF_MAGIC 0x464C457Fu /* \x7F E L F, little-endian */

#define ET_EXEC   2
#define EM_386    3
#define PT_LOAD   1

#define PF_X      0x1
#define PF_W      0x2
#define PF_R      0x4

struct elf32_header {
    u32 magic;
    u8 class;
    u8 data;
    u8 version;
    u8 osabi;
    u8 abiversion;
    u8 pad[7];
    u16 type;
    u16 machine;
    u32 version2;
    u32 entry;
    u32 phoff;
    u32 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
} PACKED;

struct elf32_phdr {
    u32 type;
    u32 offset;
    u32 vaddr;
    u32 paddr;
    u32 filesz;
    u32 memsz;
    u32 flags;
    u32 align;
} PACKED;

struct elf_load_info {
    vaddr_t entry;      /* where to begin execution              */
    vaddr_t image_low;  /* lowest mapped page                    */
    vaddr_t image_high; /* one past the highest mapped page      */
    u32 segments;       /* PT_LOAD segments mapped               */
    u32 pages;          /* pages allocated                       */
};

/* Map every PT_LOAD segment of `image` into the current address space with
 * user permissions, zeroing the .bss tail. Returns false, having unmapped
 * anything it already mapped, if the image is malformed or does not fit.
 *
 * The caller is responsible for the user stack. */
bool elf_load_user(const void *image, size_t size, struct elf_load_info *out);

/* Release what elf_load_user() mapped. */
void elf_unload_user(const struct elf_load_info *info);

/* Validate without mapping - used by tests and by the loader itself. */
bool elf_validate(const void *image, size_t size, const char **why);

#endif /* _KERNEL_ELF_H */
