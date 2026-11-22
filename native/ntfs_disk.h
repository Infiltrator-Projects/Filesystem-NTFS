/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_NTFS_DISK_H
#define IFS_NTFS_DISK_H
#include <infiltratr/filesystem_module_v2.h>

typedef struct {
    uint32_t sector_bytes, cluster_bytes, sectors_per_cluster, record_bytes;
    uint64_t cluster_count, volume_bytes, mft_lcn, mirror_lcn;
} NtfsDiskGeometry;

int ntfs_disk_geometry(const uint8_t boot[512], uint64_t host_bytes, NtfsDiskGeometry *out);
int ntfs_disk_probe(const IfsFsHostRuntimeV2 *host, uint32_t *variant);

/* The NTFS update-sequence array protects each on-disk logical sector.
 * A 4Kn FILE or INDEX record has one 4096-byte protected interval rather
 * than eight 512-byte intervals; the USA stays before the first tail. */
#define NTFS_MST_BYTES 512u
int ntfs_disk_usa(const uint8_t *record, size_t length, uint32_t sector_bytes,
                  uint16_t *offset, uint16_t *count);
#endif
