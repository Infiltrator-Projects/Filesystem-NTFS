/* SPDX-License-Identifier: GPL-3.0-or-later
 * Common V2 probing, volume accounting and offline formatting.
 * The host owns the partition lease and all raw I/O.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "ntfs_native.h"
#include "../formatter/format_device.h"
/* The real NTFS MFT and $Bitmap readers run against Common V2 host I/O,
 * not a copied device path. Only a completely qualified, clean volume may
 * provide its actual total/free cluster accounting. */
typedef struct {
    IfsFsHostRuntimeV2 host;
    NtfsVolume media;
    NtfsLayout layout;
    uint64_t free_clusters;
} MountedNtfs;

int ifs_ntfs_map_readonly(const IfsFsHostRuntimeV2 *host, const char *path,
                         uint64_t first, IfsFsExtentV2 *out,
                         uint32_t capacity, uint32_t *written);

static int native_probe(const IfsFsHostRuntimeV2 *host, uint32_t *variant)
{
    if (!variant || !ifs_fs_host_v2_valid(host))
        return IFS_FS_INVALID;
    const uint32_t sector = host->logical_sector_bytes;
    uint8_t *raw = host->allocate(host->context, sector);
    if (!raw) return IFS_FS_NO_MEMORY;
    const int got = host->read_at(host->context, 0u, raw, sector);
    const int signature = got == 0 && sector >= 512u &&
        memcmp(raw + 3u, "NTFS    ", 8u) == 0 &&
        raw[510u] == 0x55u && raw[511u] == 0xaau;
    host->release(host->context, raw);
    if (got != 0) return IFS_FS_IO_ERROR;
    if (!signature) return IFS_FS_NOT_FOUND;
    NtfsVolume media;
    char *error = NULL;
    if (ntfs_open_host(host, &media, &error) != 0) {
        free(error);
        return IFS_FS_CORRUPT;
    }
    ntfs_close_volume(&media);
    free(error);
    *variant = 1u;
    return IFS_FS_OK;
}

static int fill_volume(const MountedNtfs *volume, IfsFsVolumeInfoV2 *info)
{
    if (!volume || !info || info->structure_bytes < sizeof(*info) ||
        !volume->media.cluster_size || !volume->layout.bitmap)
        return IFS_FS_INVALID;
    info->variant = 1u;
    info->block_bytes = volume->media.cluster_size;
    info->max_name_bytes = 255u;
    info->total_bytes = volume->media.volume_bytes;
    info->free_bytes = volume->free_clusters * volume->media.cluster_size;
    return IFS_FS_OK;
}
static int ntfs_volume_query(void *handle, IfsFsVolumeInfoV2 *info)
{
    return fill_volume(handle, info);
}
static unsigned popcount8(uint8_t bits)
{
    unsigned count = 0u;
    while (bits) {
        bits = (uint8_t)(bits & (uint8_t)(bits - 1u));
        ++count;
    }
    return count;
}
static int native_mount(const IfsFsHostRuntimeV2 *host, void **output,
                        IfsFsVolumeInfoV2 *info)
{
    if (!output || !info || info->structure_bytes < sizeof(*info) ||
        !ifs_fs_host_v2_valid(host))
        return IFS_FS_INVALID;
    *output = NULL;
    /* The native MFT/$Bitmap snapshot is qualified for read-only sessions
     * only. An exclusive writable host must not silently acquire a mount
     * until transactional V2 mutation and recovery have been qualified. */
    if (!(host->capabilities & IFS_FS_HOST_READ_ONLY))
        return IFS_FS_ACCESS;
    uint32_t variant = 0u;
    int result = native_probe(host, &variant);
    if (result) return result;
    MountedNtfs *volume = host->allocate(host->context, sizeof(*volume));
    if (!volume) return IFS_FS_NO_MEMORY;
    memset(volume, 0, sizeof(*volume));
    volume->host = *host;
    volume->media.fd = -1;
    char *error = NULL;
    if (ntfs_open_host(&volume->host, &volume->media, &error) != 0 ||
        ntfs_read_layout(&volume->media, false, &volume->layout, &error) != 0) {
        free(error);
        ntfs_layout_free(&volume->layout);
        ntfs_close_volume(&volume->media);
        host->release(host->context, volume);
        return IFS_FS_CORRUPT;
    }
    free(error);
    const uint64_t total = volume->media.total_clusters;
    const uint64_t byte_count = total / 8u;
    uint64_t used = 0u;
    for (uint64_t i = 0u; i < byte_count; ++i)
        used += popcount8(volume->layout.bitmap[i]);
    if (total % 8u) {
        const uint8_t mask = (uint8_t)((1u << (total % 8u)) - 1u);
        used += popcount8(volume->layout.bitmap[byte_count] & mask);
    }
    if (used > total) {
        ntfs_layout_free(&volume->layout);
        ntfs_close_volume(&volume->media);
        host->release(host->context, volume);
        return IFS_FS_CORRUPT;
    }
    volume->free_clusters = total - used;
    result = fill_volume(volume, info);
    if (result != IFS_FS_OK) {
        ntfs_layout_free(&volume->layout);
        ntfs_close_volume(&volume->media);
        host->release(host->context, volume);
        return result;
    }
    *output = volume;
    return IFS_FS_OK;
}
static int native_unmount(void *handle)
{
    MountedNtfs *volume = handle;
    if (!volume || !volume->layout.bitmap) return IFS_FS_INVALID;
    IfsFsHostRuntimeV2 host = volume->host;
    ntfs_layout_free(&volume->layout);
    ntfs_close_volume(&volume->media);
    host.release(host.context, volume);
    return IFS_FS_OK;
}

static int map_file(void *handle, const char *path, uint64_t first,
                    IfsFsExtentV2 *out, uint32_t capacity, uint32_t *written)
{
    const MountedNtfs *volume = handle;
    if (!written)
        return IFS_FS_INVALID;
    *written = 0u;
    if (!volume || !out || !capacity ||
        !(volume->host.capabilities & IFS_FS_HOST_READ_ONLY))
        return IFS_FS_INVALID;
    return ifs_ntfs_map_readonly(&volume->host, path, first, out, capacity, written);
}

static const IfsFsDefragOpsV2 defrag_ops = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsDefragOpsV2),
        IFS_FS_GROUP_DEFRAG, 0u, IFS_FS_DEFRAG_MAP },
    .map = map_file
};

static const IfsFsVolumeOpsV2 volume_ops = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2),
        IFS_FS_GROUP_VOLUME, 0u, IFS_FS_VOLUME_QUERY | IFS_FS_VOLUME_FORMAT },
    .query = ntfs_volume_query,
    .format = ifs_ntfs_format
};
static const IfsFsCoreOpsV2 core = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2),
                IFS_FS_GROUP_CORE, 0u, IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT },
    .probe = native_probe,
    .mount = native_mount,
    .unmount = native_unmount
};
static const IfsFsGroupHeaderV2 *query(uint32_t id, uint32_t lo, uint32_t hi) {
    if (lo > IFS_FS_GROUP_ABI_V2 || hi < IFS_FS_GROUP_ABI_V2)
        return NULL;
    if (id == IFS_FS_GROUP_CORE) return &core.header;
    if (id == IFS_FS_GROUP_VOLUME) return &volume_ops.header;
    if (id == IFS_FS_GROUP_DEFRAG) return &defrag_ops.header;
    return NULL;
}
static const IfsFsModuleApiV2 api = {
    IFS_FS_COMMON_ABI_V2, sizeof(IfsFsModuleApiV2), "ntfs",
    IFS_FS_PROFILE_HOSTED, IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) |
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME) | IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG), query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void) { return &api; }
