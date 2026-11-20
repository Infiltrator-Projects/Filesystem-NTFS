/* SPDX-License-Identifier: GPL-3.0-or-later
 * End-to-end hosted Common V2 test on independently formatted NTFS media.
 * This harness receives only a read-only, partition-relative block host.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
typedef struct { int fd; uint64_t bytes; } Image;
static int read_at(void *context,uint64_t offset,void *dst,uint32_t length)
{
    Image *image=context;
    if (!length || (offset%512u) || (length%512u) || length>4096u ||
        offset>image->bytes || length>image->bytes-offset)
        return IFS_FS_IO_ERROR;
    return pread(image->fd,dst,length,(off_t)offset)==(ssize_t)length ?
        IFS_FS_OK : IFS_FS_IO_ERROR;
}
static void *allocate(void *unused,size_t bytes)
{ (void)unused;return malloc(bytes); }
static void release(void *unused,void *p)
{ (void)unused;free(p); }
#define CHECK(v) do { if (!(v)) { \
    fprintf(stderr,"NTFS V2 media check failed at %d: %s\n",__LINE__,#v); \
    return 1; } } while(0)
int main(int argc,char **argv)
{
    if (argc!=4)return 2;
    Image image={0};
    image.fd=open(argv[1],O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    CHECK(image.fd>=0);
    struct stat metadata;
    CHECK(fstat(image.fd,&metadata)==0 && metadata.st_size>0);
    image.bytes=(uint64_t)metadata.st_size;
    IfsFsHostRuntimeV2 host={
        .abi_version=IFS_FS_HOST_ABI_V2,
        .structure_bytes=sizeof(IfsFsHostRuntimeV2),
        .capabilities=IFS_FS_HOST_READ_ONLY,
        .volume_bytes=image.bytes,
        .logical_sector_bytes=512u,
        .physical_sector_bytes=512u,
        .maximum_transfer_bytes=4096u,
        .context=&image,
        .read_at=read_at,.allocate=allocate,.release=release
    };
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api,"ntfs"));
    CHECK(api->runtime_profiles==IFS_FS_PROFILE_HOSTED);
    const IfsFsCoreOpsV2 *core=(const void *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    CHECK(core && core->probe && core->mount && core->unmount &&
          core->stat && core->directory_entry && core->read);
    CHECK((core->header.capabilities &
        (IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|
         IFS_FS_CORE_LIST|IFS_FS_CORE_READ)) ==
        (IFS_FS_CORE_PROBE|IFS_FS_CORE_MOUNT|IFS_FS_CORE_STAT|
         IFS_FS_CORE_LIST|IFS_FS_CORE_READ));
    uint32_t variant=0u;
    void *mounted=NULL;
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    if (!strcmp(argv[3],"--reject")) {
        CHECK(core->probe(&host,&variant)!=IFS_FS_OK);
        CHECK(core->mount(&host,&mounted,&info)!=IFS_FS_OK && !mounted);
        CHECK(close(image.fd)==0);
        return 0;
    }
    CHECK(core->probe(&host,&variant)==IFS_FS_OK && variant==1u);
    CHECK(core->mount(&host,&mounted,&info)==IFS_FS_OK && mounted);
    char name[1024]={0};
    IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),.name=name,
                        .name_capacity=sizeof(name)};
    CHECK(core->stat(mounted,"/",&entry)==IFS_FS_OK &&
          entry.type==IFS_FS_ENTRY_DIRECTORY);
    CHECK(core->stat(mounted,"/kernel.img",&entry)==IFS_FS_OK &&
          entry.type==IFS_FS_ENTRY_FILE);
    struct stat source;
    CHECK(stat(argv[2],&source)==0 && source.st_size>0 &&
          source.st_size<=131072);
    CHECK(entry.size_bytes==(uint64_t)source.st_size);
    int found=0,ended=0;
    for (uint32_t i=0u;i<1024u;++i) {
        int rc=core->directory_entry(mounted,"/",i,&entry);
        if (rc==IFS_FS_END){ended=1;break;}
        CHECK(rc==IFS_FS_OK);
        if (!strcmp(name,"kernel.img")) {
            CHECK(entry.type==IFS_FS_ENTRY_FILE &&
                  entry.size_bytes==(uint64_t)source.st_size);
            ++found;
        }
    }
    CHECK(found==1 && ended);
    uint8_t expected[131072],actual[131072];
    int input=open(argv[2],O_RDONLY|O_CLOEXEC);
    CHECK(input>=0);
    CHECK(read(input,expected,(size_t)source.st_size)==source.st_size);
    CHECK(close(input)==0);
    CHECK(core->read(mounted,"/kernel.img",0u,actual,
                     (uint32_t)source.st_size)==IFS_FS_OK);
    CHECK(!memcmp(expected,actual,(size_t)source.st_size));
    CHECK(core->read(mounted,"/kernel.img",5u,actual,17u)==IFS_FS_OK);
    CHECK(!memcmp(expected+5,actual,17u));
    CHECK(core->read(mounted,"/kernel.img",(uint64_t)source.st_size,
                     actual,0u)==IFS_FS_OK);
    CHECK(core->read(mounted,"/kernel.img",(uint64_t)source.st_size,
                     actual,1u)==IFS_FS_INVALID);
    CHECK(core->stat(mounted,"/missing.img",&entry)==IFS_FS_NOT_FOUND);
    CHECK(core->read(mounted,"/../kernel.img",0u,actual,1u)==IFS_FS_INVALID);
    CHECK(core->unmount(mounted)==IFS_FS_OK);
    CHECK(close(image.fd)==0);
    puts("NTFS_COMMON_V2_REAL_NAMESPACE_PASS");
    return 0;
}
