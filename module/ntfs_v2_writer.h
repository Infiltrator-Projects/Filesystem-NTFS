/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef IFS_NTFS_V2_WRITER_H
#define IFS_NTFS_V2_WRITER_H
#include <infiltratr/filesystem_module_v2.h>
typedef struct IfsNtfsWriter IfsNtfsWriter;
/* Uses only the supplied exclusive Common V2 lease. No device pathname,
 * kernel filesystem mount, subprocess or previous filesystem ABI. */
int ifs_ntfs_writer_open(const IfsFsHostRuntimeV2 *,IfsNtfsWriter **);
int ifs_ntfs_writer_create(IfsNtfsWriter *,const char *,uint32_t);
int ifs_ntfs_writer_write(IfsNtfsWriter *,const char *,uint64_t,const void *,uint32_t);
int ifs_ntfs_writer_remove(IfsNtfsWriter *,const char *);
int ifs_ntfs_writer_close(IfsNtfsWriter *,int);
#endif
