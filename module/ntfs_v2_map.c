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
    if (errno == EIO)
        return IFS_FS_IO_ERROR;
    if (errno == EOPNOTSUPP)
        return IFS_FS_UNSUPPORTED;
    return IFS_FS_CORRUPT;
}

/* Resolve each UTF-8 component using NTFS's own directory index and case
 * table. Reject reparse points so a map never crosses into another volume. */
static int lookup_file(ntfs_volume *volume, const char *path, ntfs_inode **result)
{
    *result = NULL;
    if (!path || path[0] != '/' || strnlen(path, 4096u) == 4096u)
        return IFS_FS_INVALID;
    if (!path[1]) {
        *result = ntfs_inode_open(volume, FILE_root);
        return *result ? IFS_FS_OK : map_errno();
    }
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


/* Hosted Common V2 namespace: one on-disk NTFS engine and one bounded
 * partition-relative, strictly read-only device. This does not claim that
 * libntfs-3g is freestanding: BOOT must reject this hosted-only .so until a
 * separate source-owned, no-import firmware profile is qualified. */
static int ntfs_open_readonly_v2(const IfsFsHostRuntimeV2 *host,
                                NtfsReadDevice *reader,
                                ntfs_volume **volume)
{
    struct ntfs_device *device;
    if (!host || !reader || !volume || !ifs_fs_host_v2_valid(host) ||
        !(host->capabilities & IFS_FS_HOST_READ_ONLY))
        return IFS_FS_INVALID;
    *volume = NULL;
    reader->host = host;
    reader->position = 0;
    device = ntfs_device_alloc("Common V2 NTFS read-only", 0,
                               &readonly_operations, reader);
    if (!device) return IFS_FS_NO_MEMORY;
    *volume = ntfs_device_mount(device, NTFS_MNT_RDONLY | NTFS_MNT_FORENSIC);
    if (!*volume) {
        int rc = map_errno();
        ntfs_device_free(device);
        return rc;
    }
    if (ntfs_set_ignore_case(*volume)) {
        int rc = map_errno();
        (void)ntfs_umount(*volume, FALSE);
        *volume = NULL;
        return rc;
    }
    return IFS_FS_OK;
}

static int ntfs_entry_v2(ntfs_inode *inode, const char *name,
                         IfsFsEntryV2 *entry)
{
    size_t len;
    ntfs_attr *data;
    if (!inode || !name || !entry ||
        entry->structure_bytes < sizeof(*entry) || !entry->name)
        return IFS_FS_INVALID;
    len = strlen(name);
    if (!len || len > 1020u) return IFS_FS_CORRUPT;
    if (len >= entry->name_capacity) return IFS_FS_NO_SPACE;
    if (inode->flags & FILE_ATTR_REPARSE_POINT)
        return IFS_FS_UNSUPPORTED;
    if (inode->mrec->flags & MFT_RECORD_IS_DIRECTORY) {
        entry->type = IFS_FS_ENTRY_DIRECTORY;
        entry->size_bytes = 0u;
    } else {
        data = ntfs_attr_open(inode, AT_DATA, AT_UNNAMED, 0u);
        if (!data) return map_errno();
        if (data->data_size < 0) {
            (void)ntfs_attr_close(data);
            return IFS_FS_CORRUPT;
        }
        entry->size_bytes = (uint64_t)data->data_size;
        entry->type = IFS_FS_ENTRY_FILE;
        ntfs_attr_close(data);
    }
    memcpy(entry->name, name, len);
    entry->name[len] = '\0';
    entry->name_bytes = (uint32_t)len;
    entry->attributes = inode->flags;
    return IFS_FS_OK;
}

int ifs_ntfs_stat_readonly_v2(const IfsFsHostRuntimeV2 *host,
                             const char *path, IfsFsEntryV2 *entry)
{
    NtfsReadDevice reader = {0};
    ntfs_volume *volume = NULL;
    ntfs_inode *inode = NULL;
    const char *name;
    int rc;
    if (!path || !entry || entry->structure_bytes < sizeof(*entry) ||
        !entry->name || !entry->name_capacity) return IFS_FS_INVALID;
    rc = ntfs_open_readonly_v2(host, &reader, &volume);
    if (rc != IFS_FS_OK) return rc;
    rc = lookup_file(volume, path, &inode);
    if (rc == IFS_FS_OK) {
        name = strrchr(path, '/');
        name = name && name[1] ? name + 1 : "/";
        rc = ntfs_entry_v2(inode, name, entry);
        if (ntfs_inode_close(inode) && rc == IFS_FS_OK)
            rc = IFS_FS_IO_ERROR;
    }
    if (ntfs_umount(volume, FALSE) && rc == IFS_FS_OK)
        rc = IFS_FS_IO_ERROR;
    return rc;
}

typedef struct {
    ntfs_volume *volume;
    IfsFsEntryV2 *entry;
    uint32_t wanted;
    uint32_t ordinal;
    int found;
    int error;
} NtfsListV2;

/* Called exclusively by the format-owned NTFS index reader. DOS aliases,
 * dot records and root synthetic parents are not extra V2 entries. */
static int ntfs_list_callback_v2(void *arg, const ntfschar *name,
                                 const int name_len, const int name_type,
                                 const s64 pos, const MFT_REF mref,
                                 const unsigned dt_type)
{
    NtfsListV2 *ctx = arg;
    ntfs_inode *child;
    iconv_t converter;
    char utf8[1024];
    char *input, *output;
    size_t available, remaining, length;
    int rc;
    (void)pos;
    (void)dt_type;
    if (ctx->found || ctx->error != IFS_FS_OK) return 0;
    if (name_type == FILE_NAME_DOS) return 0;
    if (!name || name_len <= 0 || name_len > 255) {
        ctx->error = IFS_FS_CORRUPT;
        return 0;
    }
    if ((name_len == 1 && name[0] == (ntfschar)'.') ||
        (name_len == 2 && name[0] == (ntfschar)'.' &&
         name[1] == (ntfschar)'.')) return 0;
    if (ctx->ordinal != ctx->wanted) {
        if (ctx->ordinal == UINT32_MAX) ctx->error = IFS_FS_CORRUPT;
        else ++ctx->ordinal;
        return 0;
    }
    converter = iconv_open("UTF-8", "UTF-16LE");
    if (converter == (iconv_t)-1) {
        ctx->error = IFS_FS_UNSUPPORTED;
        return 0;
    }
    input = (char *)(void *)name;
    output = utf8;
    remaining = (size_t)name_len * sizeof(*name);
    available = sizeof(utf8) - 1u;
    rc = iconv(converter, &input, &remaining, &output, &available) == (size_t)-1 ||
         remaining != 0u ? IFS_FS_CORRUPT : IFS_FS_OK;
    (void)iconv_close(converter);
    if (rc != IFS_FS_OK) {
        ctx->error = rc;
        return 0;
    }
    length = (size_t)(output - utf8);
    utf8[length] = '\0';
    child = ntfs_inode_open(ctx->volume, mref);
    if (!child) {
        ctx->error = map_errno();
        return 0;
    }
    ctx->error = ntfs_entry_v2(child, utf8, ctx->entry);
    if (ntfs_inode_close(child) && ctx->error == IFS_FS_OK)
        ctx->error = IFS_FS_IO_ERROR;
    ctx->found = (ctx->error == IFS_FS_OK);
    return 0;
}

int ifs_ntfs_list_readonly_v2(const IfsFsHostRuntimeV2 *host,
                             const char *path, uint32_t index,
                             IfsFsEntryV2 *entry)
{
    NtfsReadDevice reader = {0};
    ntfs_volume *volume = NULL;
    ntfs_inode *dir = NULL;
    NtfsListV2 ctx = {0};
    s64 position = 0;
    int rc;
    if (!path || !entry || entry->structure_bytes < sizeof(*entry) ||
        !entry->name || !entry->name_capacity) return IFS_FS_INVALID;
    rc = ntfs_open_readonly_v2(host, &reader, &volume);
    if (rc != IFS_FS_OK) return rc;
    rc = lookup_file(volume, path, &dir);
    if (rc == IFS_FS_OK) {
        if (!(dir->mrec->flags & MFT_RECORD_IS_DIRECTORY))
            rc = IFS_FS_INVALID;
        else {
            ctx.volume = volume;
            ctx.entry = entry;
            ctx.wanted = index;
            if (ntfs_readdir(dir, &position, &ctx, ntfs_list_callback_v2))
                rc = map_errno();
            else
                rc = ctx.error == IFS_FS_OK ?
                     (ctx.found ? IFS_FS_OK : IFS_FS_END) : ctx.error;
        }
        if (ntfs_inode_close(dir) && rc >= 0)
            rc = IFS_FS_IO_ERROR;
    }
    if (ntfs_umount(volume, FALSE) && rc >= 0)
        rc = IFS_FS_IO_ERROR;
    return rc;
}

int ifs_ntfs_read_readonly_v2(const IfsFsHostRuntimeV2 *host,
                             const char *path, uint64_t offset,
                             void *destination, uint32_t length)
{
    NtfsReadDevice reader = {0};
    ntfs_volume *volume = NULL;
    ntfs_inode *inode = NULL;
    ntfs_attr *attr = NULL;
    int rc;
    if (!path || (!destination && length)) return IFS_FS_INVALID;
    rc = ntfs_open_readonly_v2(host, &reader, &volume);
    if (rc != IFS_FS_OK) return rc;
    rc = lookup_file(volume, path, &inode);
    if (rc == IFS_FS_OK) {
        if ((inode->mrec->flags & MFT_RECORD_IS_DIRECTORY))
            rc = IFS_FS_INVALID;
        else if (inode->flags & (FILE_ATTR_REPARSE_POINT |
                                 FILE_ATTR_COMPRESSED | FILE_ATTR_ENCRYPTED))
            rc = IFS_FS_UNSUPPORTED;
        else {
            attr = ntfs_attr_open(inode, AT_DATA, AT_UNNAMED, 0u);
            if (!attr) rc = map_errno();
            else if (attr->data_size < 0 || offset > (uint64_t)attr->data_size ||
                     length > (uint64_t)attr->data_size - offset ||
                     offset > INT64_MAX)
                rc = IFS_FS_INVALID;
            else if (length &&
                     ntfs_attr_pread(attr, (s64)offset, (s64)length, destination)
                       != (s64)length)
                rc = map_errno();
            if (attr) ntfs_attr_close(attr);
        }
        if (ntfs_inode_close(inode) && rc == IFS_FS_OK)
            rc = IFS_FS_IO_ERROR;
    }
    if (ntfs_umount(volume, FALSE) && rc == IFS_FS_OK)
        rc = IFS_FS_IO_ERROR;
    return rc;
}
