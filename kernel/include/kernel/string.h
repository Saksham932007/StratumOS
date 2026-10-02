/* StratumOS - the subset of <string.h> the kernel needs, implemented locally.
 * GCC lowers struct copies and array initialisers to memcpy/memset calls even
 * with -ffreestanding, so these must exist with exactly these names. */
#ifndef _KERNEL_STRING_H
#define _KERNEL_STRING_H

#include <kernel/types.h>

void *memset(void *dst, int c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
char *strcpy(char *dst, const char *src);
size_t strlcpy(char *dst, const char *src, size_t size);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);

bool str_to_u32(const char *s, u32 *out);

#endif /* _KERNEL_STRING_H */
