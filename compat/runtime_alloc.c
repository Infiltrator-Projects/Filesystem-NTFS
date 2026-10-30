/* SPDX-License-Identifier: GPL-3.0-or-later
 * Generic infallible allocator policy retained for original HFS+ worker.
 * No filesystem structures, layout or writer logic reside in this file.
 */
#include "ld_runtime.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *ld_xmalloc(size_t size) {
    void *ptr = malloc(size == 0u ? 1u : size);
    if (ptr == NULL) {
        fprintf(stderr, "HFSPLUS.so: malloc: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
    return ptr;
}
