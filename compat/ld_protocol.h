// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LD_PROTOCOL_H
#define LD_PROTOCOL_H

#include <stdio.h>

/*
 * Emit one machine-readable operation result.  The Defragmenter protocol remains
 * local; Common owns JSON string escaping.
 */
int ld_emit_result_event(FILE *stream, const char *operation,
                         const char *status, const char *message);

#endif
