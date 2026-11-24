/* SPDX-License-Identifier: GPL-3.0-or-later
 * Common V2 probing, volume accounting and offline formatting.
 * The host owns the partition lease and all raw I/O.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stdatomic.h>

/* Protect the session registry and native reader lifetime against concurrent
 * and reentrant host callbacks. This conservative nonblocking gate does not
 * claim parallel I/O: callers retry BUSY, and no callback can deadlock by
 * attempting to unmount an in-flight reader. */
static atomic_flag operation_gate=ATOMIC_FLAG_INIT;
static int enter_operation(void)
{
    return !atomic_flag_test_and_set_explicit(&operation_gate,memory_order_acquire);
}
static int leave_operation(int rc)
{
    atomic_flag_clear_explicit(&operation_gate,memory_order_release);
    return rc;
}

#include <string.h>
#include <stdlib.h>
#include "ntfs_native.h"
#include "ntfs_disk.h"
#include "ntfs_ro_v2.h"
#include "../formatter/format_device.h"
/* The real NTFS MFT and $Bitmap readers run against Common V2 host I/O,
 * not a copied device path. Only a completely qualified, clean volume may
 * provide its actual total/free cluster accounting. */
typedef struct {
    IfsFsHostRuntimeV2 host;
    NtfsVolume media;
    NtfsLayout layout;
    NtfsReadOnlyV2 *reader;
    uint64_t free_clusters;
} MountedNtfs;
/* Non-recycled tokens prevent stale pointers from being reused by subsequent
 * mounts. The provider enforces nonblocking operation exclusion. */
#define NTFS_V2_MAX_MOUNTS 32u
static struct { uintptr_t token; MountedNtfs *volume; } mounted_sessions[NTFS_V2_MAX_MOUNTS];
static uintptr_t next_mount_token;
static MountedNtfs *find_volume(void *handle)
{
    const uintptr_t token=(uintptr_t)handle;
    if (!token) return NULL;
    for (size_t i=0u;i<NTFS_V2_MAX_MOUNTS;++i)
        if (mounted_sessions[i].token==token) return mounted_sessions[i].volume;
    return NULL;
}
static size_t free_session(void)
{
    for (size_t i=0u;i<NTFS_V2_MAX_MOUNTS;++i)
        if (!mounted_sessions[i].token) return i;
    return NTFS_V2_MAX_MOUNTS;
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
static int ntfs_volume_query_impl(void *handle, IfsFsVolumeInfoV2 *info)
{
    return fill_volume(find_volume(handle), info);
}
static int ntfs_volume_query(void *handle,IfsFsVolumeInfoV2 *info)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(ntfs_volume_query_impl(handle,info));
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
static int native_mount_impl(const IfsFsHostRuntimeV2 *host, void **output,
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
    const size_t session_index=free_session();
    if (session_index==NTFS_V2_MAX_MOUNTS || next_mount_token==UINTPTR_MAX)
        return IFS_FS_BUSY;
    uint32_t variant = 0u;
    int result = ntfs_disk_probe(host, &variant);
    if (result) return result;
    MountedNtfs *volume = host->allocate(host->context, sizeof(*volume));
    if (!volume) return IFS_FS_NO_MEMORY;
    memset(volume, 0, sizeof(*volume));
    /* V2 is size-versioned: older callers need not allocate fields added to
     * a later host structure. Copy only their supplied prefix and clamp the
     * saved size to the storage actually owned by this module. */
    const size_t host_bytes = host->structure_bytes < sizeof(volume->host) ?
        host->structure_bytes : sizeof(volume->host);
    memcpy(&volume->host, host, host_bytes);
    volume->host.structure_bytes = (uint32_t)host_bytes;
    volume->media.fd = -1;
    /* CORE.MOUNT must apply the same native clean-volume policy in both
     * profiles before the hosted allocation inventory is inspected. A
     * dirty NTFS volume is UNSUPPORTED (needs journal replay), not an
     * apparently corrupt bitmap just because hosted inventory refused it. */
    IfsFsVolumeInfoV2 reader_info={.structure_bytes=sizeof(reader_info)};
    result=ntfs_ro_open_v2(&volume->host,&volume->reader,&reader_info);
    if(result!=IFS_FS_OK) {
        host->release(host->context, volume);
        return result;
    }
    char *error = NULL;
    if (ntfs_open_host(&volume->host, &volume->media, &error) != 0 ||
        ntfs_read_layout(&volume->media, false, &volume->layout, &error) != 0) {
        free(error);
        ntfs_ro_close_v2(volume->reader);
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
        ntfs_ro_close_v2(volume->reader);
        ntfs_layout_free(&volume->layout);
        ntfs_close_volume(&volume->media);
        host->release(host->context, volume);
        return IFS_FS_CORRUPT;
    }
    volume->free_clusters = total - used;
    result = fill_volume(volume, info);
    if (result != IFS_FS_OK) {
        ntfs_ro_close_v2(volume->reader);
        ntfs_layout_free(&volume->layout);
        ntfs_close_volume(&volume->media);
        host->release(host->context, volume);
        return result;
    }
    const uintptr_t token=++next_mount_token;
    mounted_sessions[session_index].volume=volume;
    mounted_sessions[session_index].token=token;
    *output=(void *)token;
    return IFS_FS_OK;
}
static int native_mount(const IfsFsHostRuntimeV2 *host, void **output, IfsFsVolumeInfoV2 *info)
{
    if (output) *output=NULL;
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(native_mount_impl(host,output,info));
}
static int native_unmount_impl(void *handle)
{
    MountedNtfs *volume=find_volume(handle);
    if (!volume || !volume->layout.bitmap) return IFS_FS_INVALID;
    for (size_t i=0u;i<NTFS_V2_MAX_MOUNTS;++i) {
        if (mounted_sessions[i].token==(uintptr_t)handle) {
            mounted_sessions[i].token=0u;
            mounted_sessions[i].volume=NULL;
            break;
        }
    }
    IfsFsHostRuntimeV2 host=volume->host;
    ntfs_ro_close_v2(volume->reader);
    ntfs_layout_free(&volume->layout);
    ntfs_close_volume(&volume->media);
    host.release(host.context, volume);
    return IFS_FS_OK;
}
static int native_unmount(void *handle)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(native_unmount_impl(handle));
}

static int map_file_impl(void *handle, const char *path, uint64_t first,
                    IfsFsExtentV2 *out, uint32_t capacity, uint32_t *written)
{
    const MountedNtfs *volume = find_volume(handle);
    if (!written)
        return IFS_FS_INVALID;
    *written = 0u;
    if (!volume || !volume->reader || !out || !capacity ||
        !(volume->host.capabilities & IFS_FS_HOST_READ_ONLY))
        return IFS_FS_INVALID;
    return ntfs_ro_map_v2(volume->reader, path, first, out, capacity, written);
}
static int map_file(void *handle,const char *path,uint64_t first,IfsFsExtentV2 *out,uint32_t capacity,uint32_t *written)
{
    if (written) *written=0u;
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(map_file_impl(handle,path,first,out,capacity,written));
}

static const IfsFsDefragOpsV2 defrag_ops = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsDefragOpsV2),
        IFS_FS_GROUP_DEFRAG, 0u, IFS_FS_DEFRAG_MAP },
    .map = map_file
};

static int ntfs_label(void *handle,char *out,uint32_t capacity,uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0u;
    if(!enter_operation())return IFS_FS_BUSY;
    const MountedNtfs *volume=find_volume(handle);
    return leave_operation(volume && volume->reader ?
        ntfs_ro_label_v2(volume->reader,out,capacity,written):IFS_FS_INVALID);
}
static const IfsFsVolumeOpsV2 volume_ops = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2),
        IFS_FS_GROUP_VOLUME, 0u, IFS_FS_VOLUME_QUERY | IFS_FS_VOLUME_FORMAT | IFS_FS_VOLUME_LABEL_GET },
    .query = ntfs_volume_query, .label=ntfs_label,
    .format = ifs_ntfs_format
};

static int stat_file_impl(void *handle, const char *path, IfsFsEntryV2 *entry)
{
    const MountedNtfs *volume = find_volume(handle);
    if (!volume || !volume->reader) return IFS_FS_INVALID;
    return ntfs_ro_stat_v2(volume->reader, path, entry);
}
static int stat_file(void *handle,const char *path,IfsFsEntryV2 *entry)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(stat_file_impl(handle,path,entry));
}
static int list_file_impl(void *handle, const char *path, uint32_t index,
                     IfsFsEntryV2 *entry)
{
    const MountedNtfs *volume = find_volume(handle);
    if (!volume || !volume->reader) return IFS_FS_INVALID;
    return ntfs_ro_list_v2(volume->reader, path, index, entry);
}
static int list_file(void *handle,const char *path,uint32_t index,IfsFsEntryV2 *entry)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(list_file_impl(handle,path,index,entry));
}
static int read_file_impl(void *handle, const char *path, uint64_t offset,
                     void *destination, uint32_t length)
{
    const MountedNtfs *volume = find_volume(handle);
    if (!volume || !volume->reader) return IFS_FS_INVALID;
    return ntfs_ro_read_v2(volume->reader, path, offset,
                           destination, length);
}
static int read_file(void *handle,const char *path,uint64_t offset,void *data,uint32_t length)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(read_file_impl(handle,path,offset,data,length));
}

static const IfsFsCoreOpsV2 core = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2),
                IFS_FS_GROUP_CORE, 0u, IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT |
                IFS_FS_CORE_STAT | IFS_FS_CORE_LIST | IFS_FS_CORE_READ },
    .probe = ntfs_disk_probe,
    .mount = native_mount,
    .unmount = native_unmount,
    .stat = stat_file,
    .directory_entry = list_file,
    .read = read_file
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
