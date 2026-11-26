/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_NTFS_RO_V2_H
#define IFS_NTFS_RO_V2_H
#include <infiltratr/filesystem_module_v2.h>
typedef struct NtfsReadOnlyV2 NtfsReadOnlyV2;
int ntfs_ro_open_v2(const IfsFsHostRuntimeV2 *,NtfsReadOnlyV2 **,
                    IfsFsVolumeInfoV2 *);
void ntfs_ro_close_v2(NtfsReadOnlyV2 *);
int ntfs_ro_label_v2(NtfsReadOnlyV2 *,char *,uint32_t,uint32_t *);
int ntfs_ro_query_v2(NtfsReadOnlyV2 *,IfsFsVolumeInfoV2 *);
int ntfs_ro_stat_v2(NtfsReadOnlyV2 *,const char *,IfsFsEntryV2 *);
int ntfs_ro_list_v2(NtfsReadOnlyV2 *,const char *,uint32_t,IfsFsEntryV2 *);
int ntfs_ro_read_v2(NtfsReadOnlyV2 *,const char *,uint64_t,void *,uint32_t);
/* Hosted DEFRAG.MAP reads the same validated NTFS runlist and namespace
 * as the freestanding Common V2 reader. No alternate NTFS-3G mapper. */
int ntfs_ro_map_v2(NtfsReadOnlyV2 *,const char *,uint64_t,
                   IfsFsExtentV2 *,uint32_t,uint32_t *);
/* Internal hosted native relocation inspection. No public non-V2 export. */
int ntfs_ro_visit_records_v2(NtfsReadOnlyV2 *,
    int (*)(void *,uint64_t,const uint8_t *,uint32_t),void *);
int ntfs_ro_relocation_reference_v2(NtfsReadOnlyV2 *,const char *,uint64_t *);
#endif
