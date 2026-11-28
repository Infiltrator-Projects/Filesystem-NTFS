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
    uint64_t record,first,clusters,reserve;
    uint32_t attribute_offset,attribute_type;
    int relocate;
} NtfsV2Placement;
typedef struct {
    NtfsV2Placement *items;
    size_t count,capacity;
    uint32_t policy;
} NtfsV2Plan;
typedef struct {
    IfsFsHostRuntimeV2 host, reader_host;
    IfsNtfsWriter *writer;
    int fault;
    NtfsVolume media;
    NtfsLayout layout;
    NtfsReadOnlyV2 *reader;
    uint64_t free_clusters;
    NtfsV2Plan *plan;
    uintptr_t plan_token;
} MountedNtfs;
/* Non-recycled tokens prevent stale pointers from being reused by subsequent
 * mounts. The provider enforces nonblocking operation exclusion. */
#define NTFS_V2_MAX_MOUNTS 32u
static struct { uintptr_t token; MountedNtfs *volume; } mounted_sessions[NTFS_V2_MAX_MOUNTS];
static uintptr_t next_mount_token,next_plan_token;
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
    if(v->plan)return IFS_FS_BUSY;
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
static void free_plan(MountedNtfs *v)
{
    if(!v || !v->plan)return;
    if(v->plan->items)v->host.release(v->host.context,v->plan->items);
    v->host.release(v->host.context,v->plan);v->plan=NULL;v->plan_token=0u;
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
    free_plan(volume);
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


/* Native relocation placement. All occupied runs are reconciled against
 * $Bitmap before any destination is accepted; all destination/growth
 * reservations are private and do not touch the medium. This bounded
 * copy-before-switch plan excludes attribute-list extensions, reparse,
 * sparse, encrypted and compressed user streams. */
typedef struct {
    MountedNtfs *v;
    NtfsV2Plan *plan;
    uint8_t *reserved,*claimed;
    uint64_t selected;
    int selected_seen;
} NtfsV2Planner;
static int bitmap_get(const uint8_t *bits,uint64_t n)
{return (bits[n/8u]>>(n%8u))&1u;}
static void bitmap_mark(uint8_t *bits,uint64_t n)
{bits[n/8u]|=(uint8_t)(1u<<(n%8u));}
static int planner_cancelled(const NtfsV2Planner *p)
{return p->v->host.cancelled && p->v->host.cancelled(p->v->host.context);}
static int planner_free(const NtfsV2Planner *p,uint64_t first,uint64_t count)
{
    const uint64_t total=p->v->media.total_clusters;
    if(first>total || count>total-first)return 0;
    for(uint64_t i=0;i<count;++i)if(bitmap_get(p->reserved,first+i))return 0;
    return 1;
}
static int planner_run(const NtfsV2Planner *p,uint64_t count,uint64_t *first)
{
    uint64_t start=0u,run=0u;
    for(uint64_t at=0u;at<p->v->media.total_clusters;++at) {
        if(!(at%4096u) && planner_cancelled(p))return IFS_FS_BUSY;
        if(bitmap_get(p->reserved,at)){run=0u;continue;}
        if(!run)start=at;
        if(++run==count){*first=start;return IFS_FS_OK;}
    }
    return IFS_FS_NO_SPACE;
}
static int planner_append(NtfsV2Planner *p,NtfsV2Placement entry)
{
    NtfsV2Plan *plan=p->plan;
    if(plan->count==plan->capacity) {
        size_t next=plan->capacity?plan->capacity*2u:32u;
        if(next<=plan->capacity || next>SIZE_MAX/sizeof(*plan->items))return IFS_FS_NO_MEMORY;
        NtfsV2Placement *items=p->v->host.allocate(p->v->host.context,next*sizeof(*items));
        if(!items)return IFS_FS_NO_MEMORY;
        if(plan->count)memcpy(items,plan->items,plan->count*sizeof(*items));
        if(plan->items)p->v->host.release(p->v->host.context,plan->items);
        plan->items=items;plan->capacity=next;
    }
    plan->items[plan->count++]=entry;return IFS_FS_OK;
}
static int planner_record(void *context,uint64_t record,const uint8_t *fixed,uint32_t bytes)
{
    NtfsV2Planner *p=context;MountedNtfs *v=p->v;
    if(planner_cancelled(p))return IFS_FS_BUSY;
    /* Strict framing is checked before the shared native attribute decoder:
     * a truncated attribute chain must not silently become an empty file. */
    uint32_t at=ntfs_u16(fixed,20u),used=ntfs_u32(fixed,24u);
    if(at<48u || (at&7u) || used>bytes || at>=used)return IFS_FS_CORRUPT;
    int terminated=0,restricted=0;
    for(uint32_t pos=at;pos<=used-4u;) {
        uint32_t type=ntfs_u32(fixed,pos);
        if(type==UINT32_MAX){terminated=1;break;}
        if(used-pos<24u)return IFS_FS_CORRUPT;
        uint32_t n=ntfs_u32(fixed,pos+4u);
        if(n<24u || (n&7u) || n>used-pos || fixed[pos+8u]>1u)
            return IFS_FS_CORRUPT;
        if(type==NTFS_ATTR_ATTRIBUTE_LIST || type==0xc0u)restricted=1;
        pos+=n;
    }
    if(!terminated)return IFS_FS_CORRUPT;
    const uint64_t base=ntfs_u64(fixed,32u)&NTFS_FILE_REFERENCE_MASK;
    const int selected=record>=NTFS_FIRST_USER_RECORD &&
        (!p->selected || record==p->selected || base==p->selected);
    if(selected && (restricted || base))return IFS_FS_UNSUPPORTED;
    if(record==p->selected)p->selected_seen=1;
    const int directory=(ntfs_u16(fixed,22u)&NTFS_RECORD_DIRECTORY)!=0u;
    NtfsAttributeVec attrs={0};char *error=NULL;
    int rc=ntfs_parse_attributes(fixed,bytes,&attrs,&error)==0?IFS_FS_OK:IFS_FS_CORRUPT;
    free(error);error=NULL;
    if(rc!=IFS_FS_OK)return rc;
    for(size_t i=0u;i<attrs.count;++i) {
        const NtfsAttribute *a=&attrs.items[i];
        if(!a->nonresident)continue;
        uint64_t count=0u,first=0u,end=0u;
        int packed=1,sparse=0;
        for(size_t j=0u;j<a->runs.count;++j) {
            const NtfsRun *run=&a->runs.items[j];
            if(!run->length || run->length>UINT64_MAX-count){rc=IFS_FS_CORRUPT;break;}
            count+=run->length;
            if(run->sparse){sparse=1;packed=0;continue;}
            if(run->lcn>=v->media.total_clusters ||
               run->length>v->media.total_clusters-run->lcn){rc=IFS_FS_CORRUPT;break;}
            if(!j)first=run->lcn;
            else if(end!=run->lcn)packed=0;
            end=run->lcn+run->length;
            for(uint64_t k=0u;k<run->length;++k) {
                uint64_t cluster=run->lcn+k;
                if(!(k%4096u) && planner_cancelled(p)){rc=IFS_FS_BUSY;break;}
                if(!ntfs_bitmap_bit(&v->layout,cluster) || bitmap_get(p->claimed,cluster)) {
                    rc=IFS_FS_CORRUPT;break;
                }
                bitmap_mark(p->claimed,cluster);
            }
            if(rc!=IFS_FS_OK)break;
        }
        if(rc!=IFS_FS_OK)break;
        if(a->highest_vcn<a->lowest_vcn ||
           a->highest_vcn-a->lowest_vcn==UINT64_MAX ||
           count!=a->highest_vcn-a->lowest_vcn+1u) {rc=IFS_FS_CORRUPT;break;}
        if(!selected || (a->type!=NTFS_ATTR_DATA && a->type!=NTFS_ATTR_INDEX_ALLOCATION))
            continue;
        if(!a->data_size)continue;
        if(a->lowest_vcn || sparse ||
           a->flags&(NTFS_ATTR_COMPRESSED|NTFS_ATTR_ENCRYPTED|NTFS_ATTR_SPARSE) ||
           a->initialized_size>a->data_size ||
           count>UINT64_MAX/v->media.cluster_size ||
           a->allocated_size!=count*v->media.cluster_size ||
           a->data_size>a->allocated_size || a->run_offset>=a->length ||
           (directory && a->type!=NTFS_ATTR_INDEX_ALLOCATION)) {
            rc=IFS_FS_UNSUPPORTED;break;
        }
        /* Growth reserve applies to ordinary nonresident file data;
         * directory index allocation is compacted without a data gap. */
        uint64_t reserve=p->plan->policy && !directory?
            count/10u+(count%10u!=0u):0u;
        if(packed && reserve && !planner_free(p,end,reserve))packed=0;
        uint64_t destination=first;
        if(!packed) {
            if(reserve>UINT64_MAX-count){rc=IFS_FS_NO_SPACE;break;}
            rc=planner_run(p,count+reserve,&destination);
            if(rc!=IFS_FS_OK)break;
            uint8_t encoded[18];size_t n=0u;
            size_t cap=a->length-a->run_offset;
            if(cap>sizeof(encoded))cap=sizeof(encoded);
            if(ntfs_encode_single_run(destination,count,encoded,cap,&n,&error)) {
                free(error);error=NULL;rc=IFS_FS_UNSUPPORTED;break;
            }
            for(uint64_t j=0u;j<count+reserve;++j)bitmap_mark(p->reserved,destination+j);
        } else {
            for(uint64_t j=0u;j<reserve;++j)bitmap_mark(p->reserved,end+j);
        }
        rc=planner_append(p,(NtfsV2Placement){record,destination,count,reserve,
            a->offset,a->type,!packed});
        if(rc!=IFS_FS_OK)break;
    }
    ntfs_attributes_free(&attrs);return rc;
}
static int relocation_plan(void *handle,const char *path,uint32_t policy,void **out)
{
    if(!out)return IFS_FS_INVALID;
    *out=NULL;
    if(!path || (policy!=0u && policy!=10u))return IFS_FS_INVALID;
    if(!enter_operation())return IFS_FS_BUSY;
    MountedNtfs *v=find_volume(handle);
    if(!v)return leave_operation(IFS_FS_INVALID);
    if(v->host.capabilities&IFS_FS_HOST_READ_ONLY)return leave_operation(IFS_FS_ACCESS);
    /* Planning itself must be write-free. A prior ordinary namespace write
     * session must first be independently committed by its caller. */
    if(v->writer || v->plan || next_plan_token==UINTPTR_MAX)return leave_operation(IFS_FS_BUSY);
    int rc=ensure_reader(v);
    if(rc!=IFS_FS_OK)return leave_operation(rc);
    uint64_t selected=0u;
    if(strcmp(path,"/")) {
        rc=ntfs_ro_relocation_reference_v2(v->reader,path,&selected);
        if(rc!=IFS_FS_OK)return leave_operation(rc);
    }
    /* Never plan against a live hibernation image. A valid clean volume
     * flag alone does not establish that Windows has released ownership. */
    char name[768];IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),.name=name,
        .name_capacity=sizeof(name)};
    rc=ntfs_ro_stat_v2(v->reader,"/hiberfil.sys",&entry);
    if(rc==IFS_FS_OK && entry.size_bytes) {
        uint8_t prefix[4]={0};uint32_t n=entry.size_bytes<4u?(uint32_t)entry.size_bytes:4u;
        rc=ntfs_ro_read_v2(v->reader,"/hiberfil.sys",0u,prefix,n);
        if(rc!=IFS_FS_OK)return leave_operation(rc);
        if(prefix[0]||prefix[1]||prefix[2]||prefix[3])return leave_operation(IFS_FS_ACCESS);
    } else if(rc!=IFS_FS_OK && rc!=IFS_FS_NOT_FOUND)return leave_operation(rc);
    const uint64_t total=v->media.total_clusters;
    const uint64_t bytes=total/8u+(total%8u!=0u);
    if(!bytes || bytes>SIZE_MAX || bytes>v->layout.bitmap_bytes)
        return leave_operation(IFS_FS_CORRUPT);
    NtfsV2Plan *plan=v->host.allocate(v->host.context,sizeof(*plan));
    uint8_t *reserved=v->host.allocate(v->host.context,(size_t)bytes);
    uint8_t *claimed=v->host.allocate(v->host.context,(size_t)bytes);
    if(plan){memset(plan,0,sizeof(*plan));plan->policy=policy;}
    if(!plan || !reserved || !claimed)rc=IFS_FS_NO_MEMORY;
    else {
        memcpy(reserved,v->layout.bitmap,(size_t)bytes);
        memset(claimed,0,(size_t)bytes);
        NtfsV2Planner planner={v,plan,reserved,claimed,selected,0};
        rc=ntfs_ro_visit_records_v2(v->reader,planner_record,&planner);
        if(rc==IFS_FS_OK && selected && !planner.selected_seen)rc=IFS_FS_CORRUPT;
    }
    if(reserved)v->host.release(v->host.context,reserved);
    if(claimed)v->host.release(v->host.context,claimed);
    if(rc==IFS_FS_OK) {
        v->plan=plan;v->plan_token=++next_plan_token;*out=(void*)v->plan_token;
    } else if(plan) {
        if(plan->items)v->host.release(v->host.context,plan->items);
        v->host.release(v->host.context,plan);
    }
    return leave_operation(rc);
}
static void relocation_release(void *handle,void *plan)
{
    if(!enter_operation())return;
    MountedNtfs *v=find_volume(handle);
    if(v && v->plan && v->plan_token==(uintptr_t)plan)free_plan(v);
    (void)leave_operation(IFS_FS_OK);
}

static const IfsFsDefragOpsV2 defrag_ops = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsDefragOpsV2),
        IFS_FS_GROUP_DEFRAG, 0u, IFS_FS_DEFRAG_MAP|IFS_FS_DEFRAG_PLAN },
    .map = map_file,.plan=relocation_plan,.release_plan=relocation_release
};

/* Whole-volume allocation is sourced from NTFS's own validated $Bitmap,
 * not the file catalogue. This includes metadata and bad-cluster ownership
 * wherever their bits are set. Unallocated clusters in the dynamic MFT
 * preference zone remain free according to NTFS volume accounting: that
 * zone is a placement preference, not permanently allocated storage.
 * A partial cluster or partition tail outside allocator geometry is
 * RESERVED rather than incorrectly presented as available file space.
 */
static int allocation_map_impl(void *handle,uint64_t first,
    IfsFsAllocationRangeV2 *ranges,uint32_t capacity,uint32_t *written)
{
    MountedNtfs *v=find_volume(handle);
    if(!written)return IFS_FS_INVALID;
    *written=0u;
    if(!ranges || !capacity)return IFS_FS_INVALID;
    const int rc=ensure_reader(v);
    if(rc!=IFS_FS_OK)return rc;
    const uint64_t unit=v->media.cluster_size,total=v->media.total_clusters;
    const uint64_t bytes=v->host.volume_bytes;
    if(!unit || total>bytes/unit ||
       v->layout.bitmap_bytes<total/8u+(total%8u!=0u))
        return IFS_FS_CORRUPT;
    const uint64_t data_end=total*unit;
    if(first>bytes)return IFS_FS_INVALID;
    if(first==bytes)return IFS_FS_END;
    uint64_t at=first;
    uint32_t n=0u;
    while(at<bytes && n<capacity) {
        if(v->host.cancelled && v->host.cancelled(v->host.context))
            return IFS_FS_BUSY;
        uint64_t end=bytes;
        uint32_t state=IFS_FS_SPACE_RESERVED;
        if(at<data_end) {
            uint64_t cluster=at/unit;
            const int used=ntfs_bitmap_bit(&v->layout,cluster);
            state=used?IFS_FS_SPACE_ALLOCATED:IFS_FS_SPACE_FREE;
            ++cluster;
            /* Skip homogeneous full bitmap bytes while preserving exact
             * boundaries at every bit transition. Check cancellation
             * periodically even on very large contiguous volumes. */
            while(cluster<total) {
                if(!(cluster%4096u) && v->host.cancelled &&
                   v->host.cancelled(v->host.context))return IFS_FS_BUSY;
                if(!(cluster%8u) && total-cluster>=8u &&
                   v->layout.bitmap[cluster/8u]==(used?0xffu:0u)) {
                    cluster+=8u;continue;
                }
                if(ntfs_bitmap_bit(&v->layout,cluster)!=used)break;
                ++cluster;
            }
            end=cluster*unit;
        }
        ranges[n++]=(IfsFsAllocationRangeV2){at,end-at,state,0u};
        at=end;
    }
    *written=n;
    return IFS_FS_OK;
}
static int allocation_map(void *handle,uint64_t first,
    IfsFsAllocationRangeV2 *ranges,uint32_t capacity,uint32_t *written)
{
    if(written)*written=0u;
    if(!enter_operation())return IFS_FS_BUSY;
    return leave_operation(allocation_map_impl(handle,first,ranges,capacity,written));
}
static const IfsFsAllocationOpsV2 allocation_ops={
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsAllocationOpsV2),
             IFS_FS_GROUP_ALLOCATION,0u,IFS_FS_ALLOCATION_MAP},
    .map=allocation_map
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
static int remove_file(void *handle,const char *path)
{
    if(!enter_operation())return IFS_FS_BUSY;
    MountedNtfs *v=find_volume(handle);
    int rc=ensure_writer(v);
    if(rc==IFS_FS_OK)rc=ifs_ntfs_writer_remove(v->writer,path);
    if(v && rc==IFS_FS_IO_ERROR)v->fault=1;
    return leave_operation(rc);
}
static int truncate_file(void *handle,const char *path,uint64_t length)
{
    if(!enter_operation())return IFS_FS_BUSY;
    MountedNtfs *v=find_volume(handle);
    int rc=ensure_writer(v);
    if(rc==IFS_FS_OK)rc=ifs_ntfs_writer_truncate(v->writer,path,length);
    if(v && rc==IFS_FS_IO_ERROR)v->fault=1;
    return leave_operation(rc);
}
static const IfsFsCoreOpsV2 core = {
    .header = { IFS_FS_GROUP_ABI_V2, sizeof(IfsFsCoreOpsV2),
                IFS_FS_GROUP_CORE, 0u, IFS_FS_CORE_PROBE | IFS_FS_CORE_MOUNT |
                IFS_FS_CORE_STAT | IFS_FS_CORE_LIST | IFS_FS_CORE_READ |
                IFS_FS_CORE_CREATE | IFS_FS_CORE_WRITE | IFS_FS_CORE_REMOVE | IFS_FS_CORE_TRUNCATE },
    .probe = ntfs_disk_probe,
    .mount = native_mount,
    .unmount = native_unmount,
    .stat = stat_file,
    .directory_entry = list_file,
    .read = read_file,.create=create_file,.write=write_file,.remove=remove_file,
    .truncate=truncate_file
};
static const IfsFsGroupHeaderV2 *query(uint32_t id, uint32_t lo, uint32_t hi) {
    if (lo > IFS_FS_GROUP_ABI_V2 || hi < IFS_FS_GROUP_ABI_V2)
        return NULL;
    if (id == IFS_FS_GROUP_CORE) return &core.header;
    if (id == IFS_FS_GROUP_VOLUME) return &volume_ops.header;
    if (id == IFS_FS_GROUP_DEFRAG) return &defrag_ops.header;
    if (id == IFS_FS_GROUP_ALLOCATION) return &allocation_ops.header;
    return NULL;
}
static const IfsFsModuleApiV2 api = {
    IFS_FS_COMMON_ABI_V2, sizeof(IfsFsModuleApiV2), "ntfs",
    IFS_FS_PROFILE_HOSTED, IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE) |
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME) | IFS_FS_GROUP_BIT(IFS_FS_GROUP_DEFRAG) |
        IFS_FS_GROUP_BIT(IFS_FS_GROUP_ALLOCATION), query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void) { return &api; }
