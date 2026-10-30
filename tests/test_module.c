/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ifs_ntfs_module_v1.h"
#include <stdio.h>
#include <string.h>
int main(void) {
 const IfsNtfsModuleV1 *a=infiltrator_filesystem_ntfs_v1();
 if (!a || a->abi_version!=IFS_NTFS_MODULE_V1 ||
     a->structure_bytes<sizeof(*a) ||
     !a->format_id || strcmp(a->format_id,"ntfs") ||
     a->capabilities!=IFS_NTFS_CAP_RUN || !a->run) {
    fputs("invalid NTFS.so ABI\n",stderr); return 1;
 }
 return 0;
}
