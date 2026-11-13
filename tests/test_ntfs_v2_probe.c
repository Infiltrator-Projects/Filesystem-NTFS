/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
static uint8_t disk[128u * 512u];
static int reader(void *ctx, uint64_t at, void *dst, uint32_t n) {
    (void)ctx;
    if (n > 512u || at > sizeof(disk) || n > sizeof(disk) - at) return -1;
    memcpy(dst, disk + (size_t)at, n);
    return 0;
}
static void *allocate(void *ctx, size_t n) { (void)ctx; return malloc(n); }
static void release(void *ctx, void *p) { (void)ctx; free(p); }
static void put64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (uint8_t)(v >> (i*8u));
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "NTFS V2: %s\n", #x); return 1; } } while (0)
static int forbidden_write(void *ctx,uint64_t at,const void *p,uint32_t n)
{ (void)ctx;(void)at;(void)p;(void)n;return -1; }
static int forbidden_flush(void *ctx)
{ (void)ctx;return -1; }
int main(void) {
    const IfsFsModuleApiV2 *api = infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api, "ntfs"));
    const IfsFsCoreOpsV2 *core = (const void *)ifs_fs_group_v2(api, IFS_FS_GROUP_CORE);
    CHECK(core && core->probe && core->mount && core->unmount &&
          !core->stat && !core->read && !core->write);
    const IfsFsVolumeOpsV2 *volume_ops = (const void *)
        ifs_fs_group_v2(api, IFS_FS_GROUP_VOLUME);
    CHECK(volume_ops && volume_ops->query && volume_ops->format &&
          volume_ops->header.capabilities == (IFS_FS_VOLUME_QUERY | IFS_FS_VOLUME_FORMAT));
    IfsFsHostRuntimeV2 h = {0};
    h.abi_version=IFS_FS_HOST_ABI_V2; h.structure_bytes=sizeof(h);
    h.capabilities=IFS_FS_HOST_READ_ONLY; h.volume_bytes=sizeof(disk);
    h.logical_sector_bytes=512; h.physical_sector_bytes=512;
    h.maximum_transfer_bytes=512; h.context=disk;
    h.read_at=reader; h.allocate=allocate; h.release=release;
    uint32_t variant=54321;
    CHECK(core->probe(&h,&variant)!=IFS_FS_OK && variant==54321);
    memcpy(disk+3u,"NTFS    ",8u);
    disk[510]=0x55;disk[511]=0xaa;
    disk[11]=0x00;disk[12]=0x02;disk[13]=1;
    put64(disk+40,128);put64(disk+48,4);put64(disk+56,8);
    disk[64]=(uint8_t)-10;
    CHECK(core->probe(&h,&variant)==IFS_FS_OK && variant==1);
    /* The boot geometry passes, but a missing genuine MFT/$Bitmap
     * must never be accepted as a usable mounted filesystem. */
    IfsFsVolumeInfoV2 info = {.structure_bytes=sizeof(IfsFsVolumeInfoV2)};
    void *mounted = NULL;
    h.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE;
    h.write_at=forbidden_write;h.flush=forbidden_flush;
    CHECK(ifs_fs_host_v2_valid(&h));
    CHECK(core->mount(&h,&mounted,&info)==IFS_FS_ACCESS && mounted==NULL);
    h.capabilities=IFS_FS_HOST_READ_ONLY;
    h.write_at=NULL;h.flush=NULL;
    CHECK(ifs_fs_host_v2_valid(&h));
    CHECK(core->mount(&h, &mounted, &info) != IFS_FS_OK && mounted == NULL);
    variant=54321; disk[13]=0;
    CHECK(core->probe(&h,&variant)==IFS_FS_CORRUPT && variant==54321);
    disk[13]=1;disk[64]=(uint8_t)-128;
    CHECK(core->probe(&h,&variant)!=IFS_FS_OK && variant==54321);
    disk[64]=(uint8_t)-10;put64(disk+40,UINT64_MAX);
    CHECK(core->probe(&h,&variant)==IFS_FS_CORRUPT);
    put64(disk+40,128);h.maximum_transfer_bytes=0;
    CHECK(core->probe(&h,&variant)==IFS_FS_INVALID);
    return 0;
}
