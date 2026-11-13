/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_NTFS_FORMAT_DEVICE_H
#define IFS_NTFS_FORMAT_DEVICE_H
#include <infiltratr/filesystem_module_v2.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const IfsFsHostRuntimeV2 *host;
    uint64_t serial;
    uint64_t position;
    bool started;
    bool failed;
    bool have_primary;
    bool have_backup;
    uint8_t primary[512];
    uint8_t backup[512];
} NtfsFormatDevice;

struct ntfs_device *ntfs_format_device(NtfsFormatDevice *target);
int ntfs_format_finish(NtfsFormatDevice *target);
int ifs_ntfs_format(const IfsFsHostRuntimeV2 *host, uint32_t variant,
                    const char *label, uint32_t flags);
#endif
