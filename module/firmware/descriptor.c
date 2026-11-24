/* SPDX-License-Identifier: GPL-3.0-or-later
 * Common V2 UEFI-profile NTFS provider. The same native on-media reader
 * is compiled without libc, NTFS-3G, POSIX or operating-system services.
 */
#include "ntfs_disk.h"
#include "ntfs_ro_v2.h"
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

#include <stddef.h>

#define NTFS_MAX_SESSIONS 16u
static struct {uintptr_t token;NtfsReadOnlyV2 *reader;} sessions[NTFS_MAX_SESSIONS];
static uintptr_t generation;
static NtfsReadOnlyV2 *active(void *handle)
{
    uintptr_t token=(uintptr_t)handle;
    if(!token)return NULL;
    for(unsigned i=0u;i<NTFS_MAX_SESSIONS;++i)
        if(sessions[i].token==token)return sessions[i].reader;
    return NULL;
}
static int mount_impl(const IfsFsHostRuntimeV2 *host,void **output,
                 IfsFsVolumeInfoV2 *info)
{
    if(!output || !info || info->structure_bytes<sizeof(*info))
        return IFS_FS_INVALID;
    *output=NULL;
    if(generation==UINTPTR_MAX)return IFS_FS_BUSY;
    unsigned free_slot=NTFS_MAX_SESSIONS;
    for(unsigned i=0u;i<NTFS_MAX_SESSIONS;++i)
        if(!sessions[i].token){free_slot=i;break;}
    if(free_slot==NTFS_MAX_SESSIONS)return IFS_FS_BUSY;
    NtfsReadOnlyV2 *reader=NULL;
    int rc=ntfs_ro_open_v2(host,&reader,info);
    if(rc!=IFS_FS_OK)return rc;
    uintptr_t token=++generation;
    sessions[free_slot].token=token;
    sessions[free_slot].reader=reader;
    *output=(void *)token;
    return IFS_FS_OK;
}
static int mount(const IfsFsHostRuntimeV2 *host, void **output, IfsFsVolumeInfoV2 *info)
{
    if (output) *output=NULL;
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(mount_impl(host,output,info));
}
static int unmount_impl(void *handle)
{
    uintptr_t token=(uintptr_t)handle;
    if(!token)return IFS_FS_INVALID;
    for(unsigned i=0u;i<NTFS_MAX_SESSIONS;++i) {
        if(sessions[i].token==token) {
            NtfsReadOnlyV2 *reader=sessions[i].reader;
            sessions[i].token=0u;sessions[i].reader=NULL;
            ntfs_ro_close_v2(reader);
            return IFS_FS_OK;
        }
    }
    return IFS_FS_INVALID;
}
static int unmount(void *handle)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(unmount_impl(handle));
}
static int stat_file_impl(void *handle,const char *path,IfsFsEntryV2 *entry)
{
    NtfsReadOnlyV2 *reader=active(handle);
    return reader?ntfs_ro_stat_v2(reader,path,entry):IFS_FS_INVALID;
}
static int stat_file(void *handle,const char *path,IfsFsEntryV2 *entry)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(stat_file_impl(handle,path,entry));
}
static int list_file_impl(void *handle,const char *path,uint32_t index,
                     IfsFsEntryV2 *entry)
{
    NtfsReadOnlyV2 *reader=active(handle);
    return reader?ntfs_ro_list_v2(reader,path,index,entry):IFS_FS_INVALID;
}
static int list_file(void *handle,const char *path,uint32_t index,IfsFsEntryV2 *entry)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(list_file_impl(handle,path,index,entry));
}
static int read_file_impl(void *handle,const char *path,uint64_t offset,
                     void *data,uint32_t count)
{
    NtfsReadOnlyV2 *reader=active(handle);
    return reader?ntfs_ro_read_v2(reader,path,offset,data,count):IFS_FS_INVALID;
}
static int read_file(void *handle,const char *path,uint64_t offset,void *data,uint32_t length)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(read_file_impl(handle,path,offset,data,length));
}
static int volume_info_impl(void *handle,IfsFsVolumeInfoV2 *info)
{
    NtfsReadOnlyV2 *reader=active(handle);
    return reader?ntfs_ro_query_v2(reader,info):IFS_FS_INVALID;
}
static int volume_info(void *handle,IfsFsVolumeInfoV2 *info)
{
    if (!enter_operation()) return IFS_FS_BUSY;
    return leave_operation(volume_info_impl(handle,info));
}
static const IfsFsCoreOpsV2 core = {
    .header = {IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),
               IFS_FS_GROUP_CORE,0u,
               IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|
               IFS_FS_CORE_LIST|IFS_FS_CORE_READ},
    .probe=ntfs_disk_probe,
    .mount=mount,.unmount=unmount,.stat=stat_file,
    .directory_entry=list_file,.read=read_file
};
static int volume_label(void *handle,char *out,uint32_t capacity,uint32_t *written)
{
    if(!written)return IFS_FS_INVALID;
    *written=0u;
    if(!enter_operation())return IFS_FS_BUSY;
    NtfsReadOnlyV2 *reader=active(handle);
    return leave_operation(reader?ntfs_ro_label_v2(reader,out,capacity,written):IFS_FS_INVALID);
}
static const IfsFsVolumeOpsV2 volume = {
    .header={IFS_FS_GROUP_ABI_V2,sizeof(IfsFsVolumeOpsV2),
             IFS_FS_GROUP_VOLUME,0u,IFS_FS_VOLUME_QUERY|IFS_FS_VOLUME_LABEL_GET},
    .query=volume_info,.label=volume_label
};
static const IfsFsGroupHeaderV2 *query(uint32_t id,uint32_t low,uint32_t high)
{
    if(low>IFS_FS_GROUP_ABI_V2 || high<IFS_FS_GROUP_ABI_V2)return NULL;
    if(id==IFS_FS_GROUP_CORE)return &core.header;
    if(id==IFS_FS_GROUP_VOLUME)return &volume.header;
    return NULL;
}
static const IfsFsModuleApiV2 api = {
    .abi_version=IFS_FS_COMMON_ABI_V2,
    .structure_bytes=sizeof(IfsFsModuleApiV2),
    .format_id="ntfs",
    .runtime_profiles=IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE,
    .group_mask=IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE)|
                IFS_FS_GROUP_BIT(IFS_FS_GROUP_VOLUME),
    .query_group=query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void){return &api;}
