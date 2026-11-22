/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "format_device.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "device.h"

static int failed(NtfsFormatDevice *target, int error)
{
    target->failed = true;
    errno = error;
    return -1;
}

static int sync_device(struct ntfs_device *device)
{
    NtfsFormatDevice *target = device->d_private;
    if (target->failed || target->host->flush(target->host->context))
        return failed(target, EIO);
    return 0;
}

static int close_device(struct ntfs_device *device)
{
    NDevClearOpen(device);
    return 0;
}

static s64 seek_device(struct ntfs_device *device, s64 displacement, int origin)
{
    NtfsFormatDevice *target = device->d_private;
    const s64 limit = (s64)target->host->volume_bytes;
    s64 base;
    switch (origin) {
        case SEEK_SET: base = 0; break;
        case SEEK_CUR: base = (s64)target->position; break;
        case SEEK_END: base = limit; break;
        default: return failed(target, EINVAL);
    }
    if (displacement < -base || displacement > limit - base)
        return failed(target, EINVAL);
    target->position = (uint64_t)(base + displacement);
    return (s64)target->position;
}

static int store(NtfsFormatDevice *target, uint64_t offset,
                 const void *data, uint32_t bytes)
{
    if (target->host->write_at(target->host->context, offset, data, bytes))
        return failed(target, EIO);
    return 0;
}

/* Keep both identifying boot sectors invalid until all filesystem structures
 * have reached durable storage. This also removes any previous NTFS backup
 * boot signature before changing its metadata. */
static int begin_format(NtfsFormatDevice *target)
{
    const uint8_t zero[4096] = {0};
    const uint32_t sector = target->host->logical_sector_bytes;
    if (store(target, 0u, zero, sector) ||
        store(target, target->host->volume_bytes - sector, zero, sector) ||
        target->host->flush(target->host->context))
        return failed(target, EIO);
    target->started = true;
    return 0;
}

static s64 pwrite_device(struct ntfs_device *device, const void *data,
                         s64 count, s64 offset)
{
    NtfsFormatDevice *target = device->d_private;
    const IfsFsHostRuntimeV2 *host = target->host;
    if (target->failed)
        return failed(target, EIO);
    if (offset < 0 || count < 0 || (uint64_t)offset > host->volume_bytes ||
        (uint64_t)count > host->volume_bytes - (uint64_t)offset ||
        (!data && count))
        return failed(target, EINVAL);
    if (!count)
        return 0;
    if (host->cancelled && host->cancelled(host->context))
        return failed(target, ECANCELED);
    if (!target->started && begin_format(target))
        return -1;

    const uint8_t *input = data;
    const uint32_t sector_bytes = host->logical_sector_bytes;
    const uint64_t last = host->volume_bytes - sector_bytes;
    const uint32_t transfer = host->maximum_transfer_bytes & ~(sector_bytes - 1u);
    uint64_t at = (uint64_t)offset;
    uint64_t left = (uint64_t)count;
    while (left) {
        const uint32_t skip = (uint32_t)(at & (sector_bytes - 1u));
        uint32_t bytes = left < sector_bytes - skip ? (uint32_t)left : sector_bytes - skip;
        if (at < sector_bytes) {
            memcpy(target->primary + skip, input, bytes);
            target->have_primary = true;
        } else if (at >= last) {
            memcpy(target->backup + skip, input, bytes);
            target->have_backup = true;
        } else if (skip || bytes < sector_bytes) {
            uint8_t sector[4096];
            const uint64_t base = at - skip;
            if (host->read_at(host->context, base, sector, sector_bytes))
                return failed(target, EIO);
            memcpy(sector + skip, input, bytes);
            if (store(target, base, sector, sector_bytes))
                return -1;
        } else {
            uint64_t available = last - at;
            if (available > left) available = left;
            available &= ~((uint64_t)sector_bytes - 1u);
            bytes = available < transfer ? (uint32_t)available : transfer;
            if (store(target, at, input, bytes))
                return -1;
        }
        input += bytes;
        at += bytes;
        left -= bytes;
    }
    return count;
}

static s64 write_device(struct ntfs_device *device, const void *data, s64 bytes)
{
    NtfsFormatDevice *target = device->d_private;
    const s64 written = pwrite_device(device, data, bytes, (s64)target->position);
    if (written > 0)
        target->position += (uint64_t)written;
    return written;
}

static struct ntfs_device_operations operations = {
    .close = close_device,
    .seek = seek_device,
    .write = write_device,
    .pwrite = pwrite_device,
    .sync = sync_device,
};

struct ntfs_device *ntfs_format_device(NtfsFormatDevice *target)
{
    struct ntfs_device *device = ntfs_device_alloc("Common V2 partition", 0,
                                                  &operations, target);
    if (device)
        NDevSetOpen(device);
    return device;
}

int ntfs_format_finish(NtfsFormatDevice *target)
{
    const IfsFsHostRuntimeV2 *host = target->host;
    const uint32_t sector = host->logical_sector_bytes;
    if (!target->have_primary || !target->have_backup ||
        memcmp(target->primary, target->backup, sector))
        return IFS_FS_CORRUPT;
    if (host->cancelled && host->cancelled(host->context))
        return IFS_FS_IO_ERROR;
    /* The format engine already flushed the new metadata. Publish the backup
     * and primary in order, using real barriers on the same held target. */
    if (store(target, host->volume_bytes - sector,
              target->backup, sector) ||
        host->flush(host->context) ||
        store(target, 0u, target->primary, sector) ||
        host->flush(host->context))
        return IFS_FS_IO_ERROR;
    return IFS_FS_OK;
}
