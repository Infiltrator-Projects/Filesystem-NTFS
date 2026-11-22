/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ntfs_disk.h"
#include <infiltratr/endian.h>
#include <string.h>

int ntfs_disk_geometry(const uint8_t boot[512], uint64_t host_bytes, NtfsDiskGeometry *out)
{
    if (memcmp(boot + 3, "NTFS    ", 8))
        return IFS_FS_NOT_FOUND;
    if (boot[510] != 0x55 || boot[511] != 0xaa)
        return IFS_FS_CORRUPT;
    const uint32_t sector = infiltratr_load_le16(boot + 11);
    const uint32_t spc = boot[13];
    if (sector < 256 || sector > 4096 || (sector & (sector - 1)) ||
        !spc || (spc & (spc - 1)))
        return IFS_FS_CORRUPT;
    const uint32_t cluster = sector * spc;
    if (cluster > 65536)
        return IFS_FS_UNSUPPORTED;
    const uint64_t sectors = infiltratr_load_le64(boot + 40);
    const uint64_t clusters = sectors / spc;
    const uint64_t mft = infiltratr_load_le64(boot + 48);
    const uint64_t mirror = infiltratr_load_le64(boot + 56);
    if (clusters < 2 || sectors > host_bytes / sector || !mft || !mirror ||
        mft >= clusters || mirror >= clusters || mft == mirror)
        return IFS_FS_CORRUPT;
    const int encoded = (int8_t)boot[64];
    if (encoded < -20)
        return IFS_FS_UNSUPPORTED;
    const uint32_t record = encoded < 0 ? UINT32_C(1) << -encoded : (uint32_t)encoded * cluster;
    if (record < sector || (record & (record - 1)) ||
        record > UINT32_C(1048576) || record % sector ||
        record > (clusters - mft) * cluster || record > (clusters - mirror) * cluster)
        return IFS_FS_CORRUPT;
    *out = (NtfsDiskGeometry){ sector, cluster, spc, record, clusters,
                               sectors * sector, mft, mirror };
    return IFS_FS_OK;
}

int ntfs_disk_probe(const IfsFsHostRuntimeV2 *host, uint32_t *variant)
{
    if (!variant)
        return IFS_FS_INVALID;
    *variant = 0;
    if (!ifs_fs_host_v2_valid(host))
        return IFS_FS_INVALID;
    uint8_t boot[512];
    if (host->volume_bytes < sizeof(boot) || host->volume_bytes < host->logical_sector_bytes)
        return IFS_FS_NOT_FOUND;
    int rc = ifs_fs_host_read_bytes_v2(host, 0, boot, sizeof(boot));
    if (rc != IFS_FS_OK)
        return rc;
    NtfsDiskGeometry geometry;
    rc = ntfs_disk_geometry(boot, host->volume_bytes, &geometry);
    if (rc == IFS_FS_OK)
        *variant = 1;
    return rc;
}

int ntfs_disk_usa(const uint8_t *record, size_t length, uint32_t sector_bytes,
                  uint16_t *offset, uint16_t *count)
{
    if (!record || !offset || !count ||
        (sector_bytes != 512u && sector_bytes != 4096u) ||
        length < sector_bytes || length % sector_bytes)
        return IFS_FS_CORRUPT;
    uint16_t at = infiltratr_load_le16(record + 4);
    uint16_t words = infiltratr_load_le16(record + 6);
    if (words != length / sector_bytes + 1u || at < 8 || (at & 1) ||
        at + 2u * words > sector_bytes - 2u)
        return IFS_FS_CORRUPT;
    *offset = at;
    *count = words;
    return IFS_FS_OK;
}
