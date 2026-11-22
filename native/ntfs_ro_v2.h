/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_NTFS_RO_V2_H
#define IFS_NTFS_RO_V2_H
#include <infiltratr/filesystem_module_v2.h>
typedef struct NtfsReadOnlyV2 NtfsReadOnlyV2;
int ntfs_ro_open_v2(const IfsFsHostRuntimeV2 *,NtfsReadOnlyV2 **,
                    IfsFsVolumeInfoV2 *);
void ntfs_ro_close_v2(NtfsReadOnlyV2 *);
int ntfs_ro_query_v2(NtfsReadOnlyV2 *,IfsFsVolumeInfoV2 *);
int ntfs_ro_stat_v2(NtfsReadOnlyV2 *,const char *,IfsFsEntryV2 *);
int ntfs_ro_list_v2(NtfsReadOnlyV2 *,const char *,uint32_t,IfsFsEntryV2 *);
int ntfs_ro_read_v2(NtfsReadOnlyV2 *,const char *,uint64_t,void *,uint32_t);
#endif
