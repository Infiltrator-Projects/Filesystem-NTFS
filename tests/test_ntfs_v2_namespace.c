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
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);
typedef struct { int fd; uint64_t bytes; uint32_t sector; } Image;
static int read_at(void *context,uint64_t offset,void *dst,uint32_t length)
{
    Image *image=context;
    if (!length || (offset%image->sector) || (length%image->sector) || length>4096u ||
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

/* Compare the genuine on-disk boot payload, not just file names or a tiny
 * synthetic buffer. Read in bounded chunks so kernel sizes do not inflate
 * the test host's stack or require a single huge CORE transfer. */
static int verify_payload(const IfsFsCoreOpsV2 *core,void *mount,
                          const char *disk_path,const char *source_path)
{
    struct stat st;
    CHECK(stat(source_path,&st)==0 && st.st_size>0 &&
          (uint64_t)st.st_size<=64u*1024u*1024u);
    int fd=open(source_path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    CHECK(fd>=0);
    uint8_t expected[65536],actual[65536];
    for(uint64_t offset=0u;offset<(uint64_t)st.st_size;) {
        uint64_t pending=(uint64_t)st.st_size-offset;
        uint32_t count=pending>sizeof(expected)?
            (uint32_t)sizeof(expected):(uint32_t)pending;
        size_t got=0u;
        while(got<count) {
            ssize_t n=pread(fd,expected+got,count-got,(off_t)(offset+got));
            CHECK(n>0);
            got+=(size_t)n;
        }
        CHECK(core->read(mount,disk_path,offset,actual,count)==IFS_FS_OK);
        CHECK(memcmp(expected,actual,count)==0);
        offset+=count;
    }
    CHECK(close(fd)==0);
    return 0;
}

int main(int argc,char **argv)
{
    if (argc!=4 && argc!=5 && argc!=6)return 2;
    Image image={.sector=argc>=5 ? (uint32_t)strtoul(argv[4],NULL,10):512u};
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
        .logical_sector_bytes=image.sector,
        .physical_sector_bytes=image.sector,
        .maximum_transfer_bytes=4096u,
        .context=&image,
        .read_at=read_at,.allocate=allocate,.release=release
    };
    void *firmware=NULL;
    const IfsFsModuleApiV2 *api=NULL;
    if(argc==6) {
        firmware=dlopen(argv[5],RTLD_NOW|RTLD_LOCAL);
        CHECK(firmware);
        void *symbol=dlsym(firmware,IFS_FS_COMMON_EXPORT_V2);
        IfsFsModuleApiEntryV2 entry=NULL;
        CHECK(symbol && sizeof(entry)==sizeof(symbol));
        memcpy(&entry,&symbol,sizeof(entry));
        api=entry ? entry() : NULL;
    } else {
        api=infiltrator_filesystem_api_v2();
    }
    CHECK(ifs_fs_api_v2_valid(api,"ntfs"));
    CHECK(api->runtime_profiles==(firmware ?
        (IFS_FS_PROFILE_FREESTANDING|IFS_FS_PROFILE_FIRMWARE):
        IFS_FS_PROFILE_HOSTED));
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
    if (!strcmp(argv[3],"--bad-mft")) {
        CHECK(core->mount(&host,&mounted,&info)==IFS_FS_CORRUPT && !mounted);
        CHECK(close(image.fd)==0);
        return 0;
    }
    if (!strcmp(argv[3],"--dirty-volume")) {
        CHECK(firmware!=NULL);
        CHECK(core->mount(&host,&mounted,&info)==IFS_FS_UNSUPPORTED &&
              !mounted);
        CHECK(close(image.fd)==0 && dlclose(firmware)==0);
        return 0;
    }
    CHECK(core->mount(&host,&mounted,&info)==IFS_FS_OK && mounted);
    void *peer=NULL;
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&peer,&info)==IFS_FS_OK &&
          peer && peer!=mounted);
    const IfsFsVolumeOpsV2 *volume_ops=(const void *)
        ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    CHECK(volume_ops && volume_ops->query);
    info.structure_bytes=sizeof(info);
    CHECK(volume_ops->query(peer,&info)==IFS_FS_OK);
    char name[1024]={0};
    IfsFsEntryV2 entry={.structure_bytes=sizeof(entry),.name=name,
                        .name_capacity=sizeof(name)};
    CHECK(core->stat(mounted,"/",&entry)==IFS_FS_OK &&
          entry.type==IFS_FS_ENTRY_DIRECTORY);
    CHECK(core->stat(mounted,"/kernel.img",&entry)==IFS_FS_OK &&
          entry.type==IFS_FS_ENTRY_FILE);
    struct stat source,initramfs;
    const char *basename=strrchr(argv[2],'/');
    CHECK(basename);
    const size_t prefix=(size_t)(basename-argv[2])+1u;
    char initramfs_source[4096];
    CHECK(prefix+sizeof("initramfs")<=sizeof(initramfs_source));
    memcpy(initramfs_source,argv[2],prefix);
    memcpy(initramfs_source+prefix,"initramfs",sizeof("initramfs"));
    CHECK(stat(argv[2],&source)==0 && source.st_size>0 &&
          (uint64_t)source.st_size<=image.bytes);
    CHECK(stat(initramfs_source,&initramfs)==0 && initramfs.st_size>0 &&
          (uint64_t)initramfs.st_size<=image.bytes);
    CHECK(entry.size_bytes==(uint64_t)source.st_size);
    int found_kernel=0,found_initramfs=0,ended=0;
    for (uint32_t i=0u;i<1024u;++i) {
        int rc=core->directory_entry(mounted,"/",i,&entry);
        if (rc==IFS_FS_END){ended=1;break;}
        if (rc!=IFS_FS_OK)
            fprintf(stderr,"NTFS V2 directory entry %u failed: %d\n",i,rc);
        CHECK(rc==IFS_FS_OK);
        if (!strcmp(name,"kernel.img")) {
            CHECK(entry.type==IFS_FS_ENTRY_FILE &&
                  entry.size_bytes==(uint64_t)source.st_size);
            ++found_kernel;
        }
        if (!strcmp(name,"initramfs.img")) {
            CHECK(entry.type==IFS_FS_ENTRY_FILE &&
                  entry.size_bytes==(uint64_t)initramfs.st_size);
            ++found_initramfs;
        }
    }
    CHECK(found_kernel==1 && found_initramfs==1 && ended);
    CHECK(core->stat(mounted,"/initramfs.img",&entry)==IFS_FS_OK &&
          entry.type==IFS_FS_ENTRY_FILE &&
          entry.size_bytes==(uint64_t)initramfs.st_size);
    CHECK(verify_payload(core,mounted,"/kernel.img",argv[2])==0);
    CHECK(verify_payload(core,mounted,"/initramfs.img",initramfs_source)==0);
    uint8_t expected_prefix[32]={0},actual[32]={0};
    int input=open(argv[2],O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    CHECK(input>=0);
    CHECK(pread(input,expected_prefix,17u,5)==17 &&
          close(input)==0);
    CHECK(core->read(mounted,"/kernel.img",5u,actual,17u)==IFS_FS_OK &&
          memcmp(expected_prefix,actual,17u)==0);
    CHECK(core->read(mounted,"/kernel.img",(uint64_t)source.st_size,
                     actual,0u)==IFS_FS_OK);
    CHECK(core->read(mounted,"/kernel.img",(uint64_t)source.st_size,
                     actual,1u)==IFS_FS_INVALID);
    CHECK(core->stat(mounted,"/missing.img",&entry)==IFS_FS_NOT_FOUND);
    CHECK(core->read(mounted,"/../kernel.img",0u,actual,1u)==IFS_FS_INVALID);
    CHECK(core->unmount(mounted)==IFS_FS_OK);
    info.structure_bytes=sizeof(info);
    CHECK(volume_ops->query(mounted,&info)==IFS_FS_INVALID);
    CHECK(core->stat(mounted,"/kernel.img",&entry)==IFS_FS_INVALID);
    CHECK(core->read(mounted,"/kernel.img",0u,actual,16u)==IFS_FS_INVALID);
    CHECK(core->unmount(mounted)==IFS_FS_INVALID);
    CHECK(volume_ops->query(peer,&info)==IFS_FS_OK);
    CHECK(verify_payload(core,peer,"/kernel.img",argv[2])==0);
    CHECK(verify_payload(core,peer,"/initramfs.img",initramfs_source)==0);
    CHECK(core->unmount(peer)==IFS_FS_OK);
    CHECK(volume_ops->query(peer,&info)==IFS_FS_INVALID);
    void *replacement=NULL;
    info.structure_bytes=sizeof(info);
    CHECK(core->mount(&host,&replacement,&info)==IFS_FS_OK &&
          replacement && replacement!=peer && replacement!=mounted);
    CHECK(volume_ops->query(mounted,&info)==IFS_FS_INVALID);
    CHECK(core->unmount(replacement)==IFS_FS_OK);
    CHECK(close(image.fd)==0);
    CHECK(!firmware || dlclose(firmware)==0);
    puts("NTFS_COMMON_V2_REAL_NAMESPACE_PASS");
    return 0;
}
