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
#include "ntfs_v2_writer.h"
/* The real NTFS MFT and $Bitmap readers run against Common V2 host I/O,
 * not a copied device path. Only a completely qualified, clean volume may
 * provide its actual total/free cluster accounting. */
typedef struct {
    IfsFsHostRuntimeV2 host, reader_host;
    IfsNtfsWriter *writer;
    int fault;
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

static int ensure_reader(MountedNtfs *volume);
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
    MountedNtfs *volume=find_volume(handle);
    const int rc=ensure_reader(volume);
    return rc==IFS_FS_OK?fill_volume(volume,info):rc;
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
static void close_snapshot(MountedNtfs *v)
{
    if(v->reader)ntfs_ro_close_v2(v->reader);
    v->reader=NULL;ntfs_layout_free(&v->layout);ntfs_close_volume(&v->media);
}
static int open_snapshot(MountedNtfs *v)
{
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    int rc=ntfs_ro_open_v2(&v->reader_host,&v->reader,&info);
    if(rc!=IFS_FS_OK)return rc;
    char *error=NULL;
    if(ntfs_open_host(&v->reader_host,&v->media,&error)!=0 ||
       ntfs_read_layout(&v->media,false,&v->layout,&error)!=0) {
        free(error);close_snapshot(v);return IFS_FS_CORRUPT;
    }
    free(error);
    const uint64_t total=v->media.total_clusters;
    uint64_t used=0u;
    for(uint64_t i=0;i<total/8u;++i)used+=popcount8(v->layout.bitmap[i]);
    if(total%8u)used+=popcount8(v->layout.bitmap[total/8u] &
                              (uint8_t)((1u<<(total%8u))-1u));
    if(used>total){close_snapshot(v);return IFS_FS_CORRUPT;}
    v->free_clusters=total-used;return IFS_FS_OK;
}
static int ensure_reader(MountedNtfs *v)
{
    if(!v)return IFS_FS_INVALID;
    if(v->fault)return IFS_FS_IO_ERROR;
    if(v->writer) {
        int rc=ifs_ntfs_writer_close(v->writer,1);v->writer=NULL;
        if(rc!=IFS_FS_OK){v->fault=1;return rc;}
    }
    if(!v->reader) {
        int rc=open_snapshot(v);
        if(rc!=IFS_FS_OK){v->fault=1;return rc;}
    }
    return IFS_FS_OK;
}
static int ensure_writer(MountedNtfs *v)
{
    if(!v)return IFS_FS_INVALID;
    if(v->host.capabilities&IFS_FS_HOST_READ_ONLY)return IFS_FS_ACCESS;
    if(v->fault)return IFS_FS_IO_ERROR;
    if(v->writer)return IFS_FS_OK;
    int rc=ifs_ntfs_writer_open(&v->host,&v->writer);
    if(rc==IFS_FS_OK)close_snapshot(v);
    else if(rc==IFS_FS_IO_ERROR)v->fault=1;
    return rc;
}
static int native_mount_impl(const IfsFsHostRuntimeV2 *host,void **output,
                             IfsFsVolumeInfoV2 *info)
{
    if(!output || !info || info->structure_bytes<sizeof(*info) ||
       !ifs_fs_host_v2_valid(host))return IFS_FS_INVALID;
    *output=NULL;
    const int write=!(host->capabilities&IFS_FS_HOST_READ_ONLY);
    /* A V2 host has no portable device identity. Be conservative: do not
     * permit an exclusive mutable session alongside any other session. */
    for(size_t i=0;i<NTFS_V2_MAX_MOUNTS;++i)
        if(mounted_sessions[i].token && (write ||
           !(mounted_sessions[i].volume->host.capabilities&IFS_FS_HOST_READ_ONLY)))
            return IFS_FS_BUSY;
    const size_t slot=free_session();
    if(slot==NTFS_V2_MAX_MOUNTS || next_mount_token==UINTPTR_MAX)return IFS_FS_BUSY;
    MountedNtfs *v=host->allocate(host->context,sizeof(*v));
    if(!v)return IFS_FS_NO_MEMORY;
    memset(v,0,sizeof(*v));v->media.fd=-1;
    const size_t bytes=host->structure_bytes<sizeof(v->host)?
                       host->structure_bytes:sizeof(v->host);
    memcpy(&v->host,host,bytes);v->host.structure_bytes=(uint32_t)bytes;
    v->reader_host=v->host;
    v->reader_host.capabilities|=IFS_FS_HOST_READ_ONLY;
    v->reader_host.capabilities&=~(IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DISCARD);
    v->reader_host.write_at=NULL;v->reader_host.discard=NULL;
    int rc=open_snapshot(v);
    if(rc==IFS_FS_OK)rc=fill_volume(v,info);
    if(rc==IFS_FS_OK && write && v->media.bytes_per_sector<host->logical_sector_bytes)
        rc=IFS_FS_UNSUPPORTED;
    if(rc!=IFS_FS_OK){close_snapshot(v);host->release(host->context,v);return rc;}
    const uintptr_t token=++next_mount_token;
    mounted_sessions[slot].volume=v;mounted_sessions[slot].token=token;
    *output=(void *)token;return IFS_FS_OK;
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
    if (!volume) return IFS_FS_INVALID;
    for (size_t i=0u;i<NTFS_V2_MAX_MOUNTS;++i) {
        if (mounted_sessions[i].token==(uintptr_t)handle) {
            mounted_sessions[i].token=0u;
            mounted_sessions[i].volume=NULL;
            break;
        }
    }
    IfsFsHostRuntimeV2 host=volume->host;
    int rc=volume->fault?IFS_FS_IO_ERROR:IFS_FS_OK;
    if(volume->writer) {
        const int closed=ifs_ntfs_writer_close(volume->writer,!volume->fault);
        if(closed!=IFS_FS_OK)rc=closed;
    }
    close_snapshot(volume);host.release(host.context,volume);return rc;
}
static int native_unmount(void *handle)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(native_unmount_impl(handle));
}

static int map_file_impl(void *handle, const char *path, uint64_t first,
                    IfsFsExtentV2 *out, uint32_t capacity, uint32_t *written)
{
    MountedNtfs *volume=find_volume(handle);
    if(!written || !out || !capacity)return IFS_FS_INVALID;
    *written=0u;
    const int rc=ensure_reader(volume);if(rc!=IFS_FS_OK)return rc;
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
    MountedNtfs *volume=find_volume(handle);
    const int rc=ensure_reader(volume);
    return leave_operation(rc==IFS_FS_OK?
        ntfs_ro_label_v2(volume->reader,out,capacity,written):rc);
}
/* Serialize the formatter with the mounted-reader registry. An exclusive
 * host lease is still mandatory; the provider additionally refuses format
 * while any reader is live, since it cannot prove distinct device identity. */
static int native_format(const IfsFsHostRuntimeV2 *host, uint32_t variant,
                         const char *label, uint32_t flags)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    for (size_t i=0u; i<NTFS_V2_MAX_MOUNTS; ++i)
        if (mounted_sessions[i].token) return leave_operation(IFS_FS_BUSY);
    return leave_operation(ifs_ntfs_format(host,variant,label,flags));
}

static const IfsFsVolumeOpsV2 volume_ops = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsVolumeOpsV2),
        IFS_FS_GROUP_VOLUME, 0u, IFS_FS_VOLUME_QUERY | IFS_FS_VOLUME_FORMAT | IFS_FS_VOLUME_LABEL_GET },
    .query = ntfs_volume_query, .label=ntfs_label,
    .format = native_format
};

static int stat_file_impl(void *handle, const char *path, IfsFsEntryV2 *entry)
{
    MountedNtfs *volume=find_volume(handle);
    const int rc=ensure_reader(volume);if(rc!=IFS_FS_OK)return rc;
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
    MountedNtfs *volume=find_volume(handle);
    const int rc=ensure_reader(volume);if(rc!=IFS_FS_OK)return rc;
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
    MountedNtfs *volume=find_volume(handle);
    const int rc=ensure_reader(volume);if(rc!=IFS_FS_OK)return rc;
    return ntfs_ro_read_v2(volume->reader, path, offset,
                           destination, length);
}
static int read_file(void *handle,const char *path,uint64_t offset,void *data,uint32_t length)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(read_file_impl(handle,path,offset,data,length));
}

static int create_file(void *handle,const char *path,uint32_t type)
{
    if(!enter_operation())return IFS_FS_BUSY;
    MountedNtfs *v=find_volume(handle);
    int rc=ensure_writer(v);
    if(rc==IFS_FS_OK)rc=ifs_ntfs_writer_create(v->writer,path,type);
    if(v && rc==IFS_FS_IO_ERROR)v->fault=1;
    return leave_operation(rc);
}
static int write_file(void *handle,const char *path,uint64_t pos,
                      const void *data,uint32_t count)
{
    if(!enter_operation())return IFS_FS_BUSY;
    MountedNtfs *v=find_volume(handle);
    int rc=ensure_writer(v);
    if(rc==IFS_FS_OK)rc=ifs_ntfs_writer_write(v->writer,path,pos,data,count);
    if(v && rc==IFS_FS_IO_ERROR)v->fault=1;
    return leave_operation(rc);
}
static const IfsFsCoreOpsV2 core = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2),
                IFS_FS_GROUP_CORE, 0u, IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT |
                IFS_FS_CORE_STAT | IFS_FS_CORE_LIST | IFS_FS_CORE_READ |
                IFS_FS_CORE_CREATE | IFS_FS_CORE_WRITE },
    .probe = ntfs_disk_probe,
    .mount = native_mount,
    .unmount = native_unmount,
    .stat = stat_file,
    .directory_entry = list_file,
    .read = read_file,.create=create_file,.write=write_file
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
