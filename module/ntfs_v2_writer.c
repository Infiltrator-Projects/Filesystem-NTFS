/* SPDX-License-Identifier: GPL-3.0-or-later
 * Hosted NTFS namespace mutations using the repository's pinned, statically
 * linked native NTFS engine. Its device is the Common V2 host, not an OS path.
 * The dirty flag remains set across an interrupted or failed write session.
 * This is not an implementation of DEFRAG or automatic journal recovery.
 */
#include "ntfs_v2_writer.h"
#include "ntfs_native.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/volume.h>
#include <ntfs-3g/inode.h>
#include <ntfs-3g/dir.h>
#include <ntfs-3g/attrib.h>
#include <ntfs-3g/layout.h>

struct IfsNtfsWriter {
    const IfsFsHostRuntimeV2 *host;
    ntfs_volume *volume;
    uint64_t position;
    int enabled, fault;
};

static int error_io(IfsNtfsWriter *w,int e)
{
    w->fault=1; errno=e; return -1;
}
static int open_dev(struct ntfs_device *d,int flags)
{
    IfsNtfsWriter *w=d->d_private;
    if(NDevOpen(d) || (flags&O_ACCMODE)!=O_RDWR ||
       !ifs_fs_host_v2_valid(w->host) ||
       (w->host->capabilities&IFS_FS_HOST_READ_ONLY)) {
        errno=EACCES;return -1;
    }
    NDevSetOpen(d);return 0;
}
static int close_dev(struct ntfs_device *d)
{
    NDevClearOpen(d);return 0;
}
static int sync_dev(struct ntfs_device *d)
{
    IfsNtfsWriter *w=d->d_private;
    if(w->host->flush(w->host->context))return error_io(w,EIO);
    NDevClearDirty(d);return w->fault?-1:0;
}
static s64 seek_dev(struct ntfs_device *d,s64 offset,int whence)
{
    IfsNtfsWriter *w=d->d_private;
    const s64 limit=(s64)w->host->volume_bytes;
    const s64 base=whence==SEEK_SET?0:whence==SEEK_CUR?(s64)w->position:
                    whence==SEEK_END?limit:-1;
    if(base<0 || offset < -base || offset > limit-base){errno=EINVAL;return -1;}
    w->position=(uint64_t)(base+offset);return (s64)w->position;
}
static int range_ok(IfsNtfsWriter *w,s64 offset,s64 count,const void *p)
{
    return offset>=0 && count>=0 && (!count || p) &&
        (uint64_t)offset<=w->host->volume_bytes &&
        (uint64_t)count<=w->host->volume_bytes-(uint64_t)offset;
}
static s64 read_dev_at(struct ntfs_device *d,void *data,s64 count,s64 offset)
{
    IfsNtfsWriter *w=d->d_private;
    if(!range_ok(w,offset,count,data)){errno=EINVAL;return -1;}
    uint8_t *p=data;
    for(uint64_t left=(uint64_t)count;left;) {
        uint32_t n=left>1048576u?1048576u:(uint32_t)left;
        if(ifs_fs_host_read_bytes_v2(w->host,(uint64_t)offset,p,n)!=IFS_FS_OK)
            return error_io(w,EIO);
        offset+=n;p+=n;left-=n;
    }
    return count;
}
static s64 write_dev_at(struct ntfs_device *d,const void *data,s64 count,s64 offset)
{
    IfsNtfsWriter *w=d->d_private;
    /* Mount must not rewrite transaction metadata before the dirty guard.
     * A volume requiring such mount-time changes is outside this profile. */
    if(!w->enabled || w->fault || !range_ok(w,offset,count,data))
        return error_io(w,EIO);
    const uint8_t *p=data;
    for(uint64_t left=(uint64_t)count;left;) {
        uint32_t n=left>1048576u?1048576u:(uint32_t)left;
        if(ifs_fs_host_write_bytes_v2(w->host,(uint64_t)offset,p,n)!=IFS_FS_OK)
            return error_io(w,EIO);
        offset+=n;p+=n;left-=n;
    }
    NDevSetDirty(d);return count;
}
static s64 read_dev(struct ntfs_device *d,void *p,s64 n)
{
    IfsNtfsWriter *w=d->d_private;
    s64 rc=read_dev_at(d,p,n,(s64)w->position);
    if(rc>0)w->position+=(uint64_t)rc;
    return rc;
}
static s64 write_dev(struct ntfs_device *d,const void *p,s64 n)
{
    IfsNtfsWriter *w=d->d_private;
    s64 rc=write_dev_at(d,p,n,(s64)w->position);
    if(rc>0)w->position+=(uint64_t)rc;
    return rc;
}
static int stat_dev(struct ntfs_device *d,struct stat *s)
{
    IfsNtfsWriter *w=d->d_private;
    memset(s,0,sizeof(*s));s->st_mode=S_IFREG|0600;
    s->st_size=(off_t)w->host->volume_bytes;
    s->st_blksize=(blksize_t)w->host->logical_sector_bytes;
    return 0;
}
static int ioctl_dev(struct ntfs_device *d,unsigned long req,void *arg)
{
    (void)d;(void)req;(void)arg;errno=ENOTTY;return -1;
}
static struct ntfs_device_operations ops={
    .open=open_dev,.close=close_dev,.seek=seek_dev,.read=read_dev,
    .write=write_dev,.pread=read_dev_at,.pwrite=write_dev_at,
    .sync=sync_dev,.stat=stat_dev,.ioctl=ioctl_dev
};

/* Validate and flush both copies of the volume state using the native
 * Common V2 metadata implementation. Never clear a dirty flag after a
 * partial or failed mutation. */
static int clear_dirty(const IfsFsHostRuntimeV2 *h)
{
    NtfsVolume media={.fd=-1};NtfsLayout layout={0};char *error=NULL;
    int ok=ntfs_open_host(h,&media,&error)==0 &&
           ntfs_read_layout(&media,true,&layout,&error)==0 &&
           ntfs_set_volume_dirty(&media,&layout,false,&error)==0;
    free(error);ntfs_layout_free(&layout);ntfs_close_volume(&media);
    return ok?IFS_FS_OK:IFS_FS_IO_ERROR;
}

int ifs_ntfs_writer_open(const IfsFsHostRuntimeV2 *h,IfsNtfsWriter **out)
{
    if(!out)return IFS_FS_INVALID;
    *out=NULL;
    if(!ifs_fs_host_v2_valid(h) || (h->capabilities&IFS_FS_HOST_READ_ONLY) ||
       h->volume_bytes>INT64_MAX)return IFS_FS_ACCESS;
    IfsNtfsWriter *w=h->allocate(h->context,sizeof(*w));
    if(!w)return IFS_FS_NO_MEMORY;
    memset(w,0,sizeof(*w));w->host=h;
    struct ntfs_device *dev=ntfs_device_alloc("Common V2 partition",0,&ops,w);
    if(!dev){h->release(h->context,w);return IFS_FS_NO_MEMORY;}
    /* No RECOVER, IGNORE_HIBERFILE, or read-only fallback flag. */
    w->volume=ntfs_device_mount(dev,NTFS_MNT_EXCLUSIVE);
    if(!w->volume){
        ntfs_device_free(dev);h->release(h->context,w);return IFS_FS_UNSUPPORTED;
    }
    if(w->fault || NVolReadOnly(w->volume) ||
       (le16_to_cpu(w->volume->flags)&NTFS_VOLUME_DIRTY)) {
        (void)ntfs_umount(w->volume,FALSE);h->release(h->context,w);
        return IFS_FS_ACCESS;
    }
    w->enabled=1;
    if(ntfs_volume_write_flags(w->volume,
       cpu_to_le16(le16_to_cpu(w->volume->flags)|NTFS_VOLUME_DIRTY)) ||
       sync_dev(dev)) {
        (void)ntfs_umount(w->volume,FALSE);h->release(h->context,w);
        return IFS_FS_IO_ERROR;
    }
    *out=w;return IFS_FS_OK;
}

/* Start with a deliberately bounded namespace: ordinary root regular files,
 * portable ASCII names, no links/reparse points, no ADS or sparse streams.
 * Unsupported filenames/layouts are refused, never translated to OS paths. */
static int root_name(const char *path,ntfschar name[255],u8 *length)
{
    if(!path || path[0]!='/' || !path[1])return IFS_FS_INVALID;
    const char *p=path+1;size_t n=strlen(p);
    if(n>255u || p[0]=='.' || p[0]=='$' || p[n-1]==' ' || p[n-1]=='.')
        return IFS_FS_UNSUPPORTED;
    for(size_t i=0;i<n;++i) {
        unsigned char c=(unsigned char)p[i];
        if(c<32u || c>=127u || strchr("\\/:*?\"<>|",c))return IFS_FS_UNSUPPORTED;
        name[i]=cpu_to_le16(c);
    }
    *length=(u8)n;return IFS_FS_OK;
}
static int writable(IfsNtfsWriter *w)
{
    if(!w || !w->volume || w->fault)return IFS_FS_IO_ERROR;
    if(w->host->cancelled && w->host->cancelled(w->host->context))
        return IFS_FS_BUSY;
    return IFS_FS_OK;
}
int ifs_ntfs_writer_create(IfsNtfsWriter *w,const char *path,uint32_t type)
{
    int rc=writable(w);if(rc!=IFS_FS_OK)return rc;
    if(type!=IFS_FS_ENTRY_FILE)return IFS_FS_UNSUPPORTED;
    ntfschar name[255];u8 count=0;
    rc=root_name(path,name,&count);if(rc!=IFS_FS_OK)return rc;
    ntfs_inode *root=ntfs_inode_open(w->volume,FILE_root);
    if(!root)return IFS_FS_IO_ERROR;
    ntfs_inode *file=ntfs_create(root,const_cpu_to_le32(0),name,count,S_IFREG);
    if(!file) {
        int e=errno;
        if(ntfs_inode_close(root)){w->fault=1;return IFS_FS_IO_ERROR;}
        if(e==EEXIST)return IFS_FS_EXISTS;
        w->fault=1;return e==ENOSPC?IFS_FS_NO_SPACE:IFS_FS_IO_ERROR;
    }
    int failed=ntfs_inode_close(file);
    if(ntfs_inode_close(root))failed=1;
    if(failed || w->fault){w->fault=1;return IFS_FS_IO_ERROR;}
    return IFS_FS_OK;
}
int ifs_ntfs_writer_write(IfsNtfsWriter *w,const char *path,uint64_t offset,
                         const void *data,uint32_t count)
{
    int rc=writable(w);if(rc!=IFS_FS_OK)return rc;
    ntfschar name[255];u8 length=0;
    rc=root_name(path,name,&length);if(rc!=IFS_FS_OK)return rc;
    if((!data && count) || offset>INT64_MAX || count>INT64_MAX-offset)
        return IFS_FS_INVALID;
    ntfs_inode *file=ntfs_pathname_to_inode(w->volume,NULL,path);
    if(!file)return errno==ENOENT?IFS_FS_NOT_FOUND:IFS_FS_IO_ERROR;
    rc=IFS_FS_UNSUPPORTED;
    ntfs_attr *stream=NULL;
    if(file->mft_no<24u ||
       (file->flags&(FILE_ATTR_DIRECTORY|FILE_ATTR_REPARSE_POINT|
                     FILE_ATTR_ENCRYPTED|FILE_ATTR_COMPRESSED|
                     FILE_ATTR_SPARSE_FILE|FILE_ATTR_READONLY)))goto done;
    stream=ntfs_attr_open(file,AT_DATA,AT_UNNAMED,0);
    if(!stream){rc=IFS_FS_IO_ERROR;goto done;}
    if(stream->data_size<0 || offset>(uint64_t)stream->data_size)goto done;
    if(!count){rc=IFS_FS_OK;goto done;}
    if(ntfs_attr_pwrite(stream,(s64)offset,(s64)count,data)!=(s64)count) {
        w->fault=1;rc=IFS_FS_IO_ERROR;goto done;
    }
    rc=IFS_FS_OK;
done:
    if(stream)ntfs_attr_close(stream);
    if(ntfs_inode_close(file)){w->fault=1;rc=IFS_FS_IO_ERROR;}
    return w->fault?IFS_FS_IO_ERROR:rc;
}
int ifs_ntfs_writer_close(IfsNtfsWriter *w,int commit)
{
    if(!w)return IFS_FS_INVALID;
    const IfsFsHostRuntimeV2 *h=w->host;
    int failed=ntfs_umount(w->volume,FALSE);
    if(w->fault || failed || !commit){h->release(h->context,w);return IFS_FS_IO_ERROR;}
    const int rc=clear_dirty(h);
    h->release(h->context,w);return rc;
}
