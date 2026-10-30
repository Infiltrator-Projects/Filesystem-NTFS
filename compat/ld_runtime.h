// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_RUNTIME_H
#define LD_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

void ld_runtime_set_program_name(const char *name);

_Noreturn void ld_die_errno(const char *what);
_Noreturn void ld_die(const char *message);
void ld_warn_errno(const char *what);

void *ld_xmalloc(size_t size);
void *ld_xcalloc(size_t count, size_t size);
char *ld_xstrdup(const char *text);
char *ld_xstrndup(const char *text, size_t length);

#endif
