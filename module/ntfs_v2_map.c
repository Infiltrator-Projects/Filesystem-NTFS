/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <infiltratr/filesystem_module_v2.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <ntfs-3g/device.h>
#include <ntfs-3g/dir.h>

typedef struct {
    const IfsFsHostRuntimeV2 *host;
    uint64_t position;
} NtfsReadDevice;

static int reader_open(struct ntfs_device *device, int flags)
{
    if ((flags & O_ACCMODE) != O_RDONLY) {
        errno = EROFS;
        return -1;
    }
    NDevSetReadOnly(device);
    NDevSetOpen(device);
    return 0;
}

static int reader_close(struct ntfs_device *device)
{
    NDevClearOpen(device);
    return 0;
}

static s64 reader_seek(struct ntfs_device *device, s64 offset, int origin)
{
    NtfsReadDevice *reader = device->d_private;
    const s64 limit = (s64)reader->host->volume_bytes;
    s64 start;
    switch (origin) {
        case SEEK_SET: start = 0; break;
        case SEEK_CUR: start = (s64)reader->position; break;
        case SEEK_END: start = limit; break;
        default: errno = EINVAL; return -1;
    }
    if (offset < -start || offset > limit - start) {
        errno = EINVAL;
        return -1;
    }
    reader->position = (uint64_t)(start + offset);
    return start + offset;
}

static s64 reader_pread(struct ntfs_device *device, void *data, s64 count, s64 offset)
{
    const NtfsReadDevice *reader = device->d_private;
    if (offset < 0 || count < 0 || count > UINT32_MAX ||
        ifs_fs_host_read_bytes_v2(reader->host, (uint64_t)offset, data,
                                 (uint32_t)count) != IFS_FS_OK) {
        errno = EIO;
        return -1;
    }
    return count;
}

static s64 reader_read(struct ntfs_device *device, void *data, s64 count)
{
    NtfsReadDevice *reader = device->d_private;
    const s64 bytes = reader_pread(device, data, count, (s64)reader->position);
    if (bytes >= 0)
        reader->position += (uint64_t)bytes;
    return bytes;
}

static int reader_sync(struct ntfs_device *device)
{
    (void)device;
    return 0;
}

static int reader_ioctl(struct ntfs_device *device, unsigned long request, void *arg)
{
    (void)device;
    (void)request;
    (void)arg;
    errno = ENOTTY;
    return -1;
}

static struct ntfs_device_operations readonly_operations = {
    .open = reader_open, .close = reader_close, .seek = reader_seek,
    .read = reader_read, .pread = reader_pread, .sync = reader_sync,
    .ioctl = reader_ioctl
};

static int map_errno(void)
{
    if (errno == ENOENT || errno == ENOTDIR)
        return IFS_FS_NOT_FOUND;
    if (errno == ENOMEM)
        return IFS_FS_NO_MEMORY;
    return IFS_FS_CORRUPT;
}

/* Resolve each UTF-8 component using NTFS's own directory index and case
 * table. Reject reparse points so a map never crosses into another volume. */
static int lookup_file(ntfs_volume *volume, const char *path, ntfs_inode **result)
{
    *result = NULL;
    if (!path || path[0] != '/' || strnlen(path, 4096u) == 4096u)
        return IFS_FS_INVALID;
    if (!path[1])
        return IFS_FS_UNSUPPORTED;
    iconv_t converter = iconv_open("UTF-16LE", "UTF-8");
    if (converter == (iconv_t)-1)
        return IFS_FS_IO_ERROR;
    ntfs_inode *inode = ntfs_inode_open(volume, FILE_root);
    int status = inode ? IFS_FS_OK : map_errno();
    for (const char *component = path + 1; status == IFS_FS_OK;) {
        const char *end = strchr(component, '/');
        size_t length = end ? (size_t)(end - component) : strlen(component);
        if (!length || (length == 1u && component[0] == '.') ||
            (length == 2u && !memcmp(component, "..", 2u))) {
            status = IFS_FS_INVALID;
            break;
        }
        ntfschar name[255];
        char *input = (char *)component;
        char *output = (char *)name;
        size_t space = sizeof(name);
        if (iconv(converter, &input, &length, &output, &space) == (size_t)-1 ||
            length != 0u) {
            status = IFS_FS_INVALID;
            break;
        }
        const u64 reference = ntfs_inode_lookup_by_name(
            inode, name, (int)((sizeof(name) - space) / sizeof(name[0])));
        if (IS_ERR_MREF(reference)) {
            status = map_errno();
            break;
        }
        if (ntfs_inode_close(inode)) {
            inode = NULL;
            status = IFS_FS_IO_ERROR;
            break;
        }
        inode = ntfs_inode_open(volume, reference);
        if (!inode) {
            status = map_errno();
            break;
        }
        if (inode->flags & FILE_ATTR_REPARSE_POINT) {
            status = IFS_FS_UNSUPPORTED;
            break;
        }
        if (!end)
            break;
        if (!(inode->mrec->flags & MFT_RECORD_IS_DIRECTORY)) {
            status = IFS_FS_NOT_FOUND;
            break;
        }
        component = end + 1;
    }
    iconv_close(converter);
    if (status == IFS_FS_OK)
        *result = inode;
    else if (inode)
        ntfs_inode_close(inode);
    return status;
}

static int map_data(ntfs_attr *data, uint64_t first, IfsFsExtentV2 *out,
                    uint32_t capacity, uint32_t *written)
{
    if (!NAttrNonResident(data) || data->data_size == 0)
        return IFS_FS_END;
    if (NAttrCompressed(data) || NAttrEncrypted(data))
        return IFS_FS_UNSUPPORTED;
    if (data->data_size < 0 || ntfs_attr_map_whole_runlist(data) || !data->rl)
        return IFS_FS_CORRUPT;
    const uint64_t cluster = data->ni->vol->cluster_size;
    if (!cluster || data->ni->vol->nr_clusters <= 0)
        return IFS_FS_CORRUPT;
    const uint64_t clusters = (uint64_t)data->ni->vol->nr_clusters;
    const uint64_t length = (uint64_t)data->data_size;
    uint64_t page = 0u;
    for (const runlist_element *run = data->rl; run->length; ++run) {
        if (run->vcn < 0 || run->length < 0 ||
            (uint64_t)run->vcn > UINT64_MAX / cluster ||
            (uint64_t)run->length > UINT64_MAX / cluster)
            return IFS_FS_CORRUPT;
        const uint64_t logical = (uint64_t)run->vcn * cluster;
        if (logical >= length)
            break;
        if (run->lcn == LCN_HOLE)
            continue;
        if (run->lcn < 0 || (uint64_t)run->lcn >= clusters ||
            (uint64_t)run->length > clusters - (uint64_t)run->lcn)
            return IFS_FS_CORRUPT;
        if (page++ < first)
            continue;
        const uint64_t span = (uint64_t)run->length * cluster;
        out[*written] = (IfsFsExtentV2){
            .logical_offset = logical, .physical_offset = (uint64_t)run->lcn * cluster,
            .length = span < length - logical ? span : length - logical
        };
        if (++*written == capacity)
            break;
    }
    return *written ? IFS_FS_OK : IFS_FS_END;
}

int ifs_ntfs_map_readonly(const IfsFsHostRuntimeV2 *host, const char *path,
                         uint64_t first, IfsFsExtentV2 *out,
                         uint32_t capacity, uint32_t *written)
{
    NtfsReadDevice reader = {.host = host};
    struct ntfs_device *device = ntfs_device_alloc("Common V2 read-only partition", 0,
                                                   &readonly_operations, &reader);
    if (!device)
        return IFS_FS_NO_MEMORY;
    ntfs_volume *volume = ntfs_device_mount(device, NTFS_MNT_RDONLY | NTFS_MNT_FORENSIC);
    if (!volume) {
        int status = map_errno();
        ntfs_device_free(device);
        return status;
    }
    ntfs_inode *inode = NULL;
    int status = ntfs_set_ignore_case(volume) ? map_errno() :
                 lookup_file(volume, path, &inode);
    if (status == IFS_FS_OK) {
        if ((inode->mrec->flags & MFT_RECORD_IS_DIRECTORY) ||
            (inode->flags & (FILE_ATTR_COMPRESSED | FILE_ATTR_ENCRYPTED))) {
            status = IFS_FS_UNSUPPORTED;
        } else {
            ntfs_attr *data = ntfs_attr_open(inode, AT_DATA, AT_UNNAMED, 0u);
            if (!data) {
                status = map_errno();
            } else {
                status = map_data(data, first, out, capacity, written);
                ntfs_attr_close(data);
            }
        }
        if (ntfs_inode_close(inode) && status == IFS_FS_OK)
            status = IFS_FS_IO_ERROR;
    }
    if (ntfs_umount(volume, FALSE))
        status = IFS_FS_IO_ERROR;
    if (status != IFS_FS_OK)
        *written = 0u;
    return status;
}
