/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ntfs_disk.h"

static const IfsFsCoreOpsV2 core = {
    .header = {IFS_FS_GROUP_ABI_V2,sizeof(IfsFsCoreOpsV2),IFS_FS_GROUP_CORE,0,IFS_FS_CORE_PROBE},
    .probe = ntfs_disk_probe
};
static const IfsFsGroupHeaderV2 *query(uint32_t id,uint32_t low,uint32_t high)
{
    return id==IFS_FS_GROUP_CORE && low<=IFS_FS_GROUP_ABI_V2 && high>=IFS_FS_GROUP_ABI_V2 ?
           &core.header:NULL;
}
static const IfsFsModuleApiV2 api = {
    IFS_FS_COMMON_ABI_V2,sizeof(IfsFsModuleApiV2),"ntfs",
    IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE,IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE),query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void) { return &api; }
