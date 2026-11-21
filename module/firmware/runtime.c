/* SPDX-License-Identifier: GPL-3.0-or-later
 * Freestanding byte primitives; no libc, VFS or filesystem-specific driver.
 */
#include <stddef.h>
void *memcpy(void *restrict destination, const void *restrict source, size_t n)
{
    unsigned char *d = destination;
    const unsigned char *s = source;
    for (size_t i = 0; i < n; ++i)
        d[i] = s[i];
    return destination;
}
void *memset(void *destination, int value, size_t n)
{
    unsigned char *d = destination;
    for (size_t i = 0; i < n; ++i)
        d[i] = (unsigned char)value;
    return destination;
}
void *memchr(const void *source, int value, size_t n)
{
    const unsigned char *p = source;
    for (size_t i = 0; i < n; ++i)
        if (p[i] == (unsigned char)value)
            return (void *)(p + i);
    return NULL;
}
int memcmp(const void *left, const void *right, size_t n)
{
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i])
            return (int)a[i] - (int)b[i];
    return 0;
}
