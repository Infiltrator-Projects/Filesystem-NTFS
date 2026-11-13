/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <infiltratr/filesystem_module_v2.h>
#include <fcntl.h>
#include <openssl/sha.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ntfs_native.h"

extern const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void);

enum { SECTOR = 512, IMAGE_BYTES = 64 * 1024 * 1024 };

typedef struct {
    int fd;
    unsigned writes;
    unsigned flushes;
    unsigned fail_flush;
} Image;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "NTFS format: %s\n", #x); goto done; } } while (0)

static int bounds(uint64_t offset, uint32_t bytes)
{
    return bytes && bytes <= 4096u && !(bytes % SECTOR) &&
           !(offset % SECTOR) && offset <= IMAGE_BYTES &&
           bytes <= IMAGE_BYTES - offset;
}

static int read_at(void *context, uint64_t offset, void *data, uint32_t bytes)
{
    Image *image = context;
    return !bounds(offset, bytes) ||
           pread(image->fd, data, bytes, (off_t)offset) != (ssize_t)bytes;
}

static int write_at(void *context, uint64_t offset, const void *data, uint32_t bytes)
{
    Image *image = context;
    ++image->writes;
    return !bounds(offset, bytes) ||
           pwrite(image->fd, data, bytes, (off_t)offset) != (ssize_t)bytes;
}

static int flush(void *context)
{
    Image *image = context;
    ++image->flushes;
    return image->flushes == image->fail_flush || fsync(image->fd);
}

static void *allocate(void *context, size_t bytes)
{
    (void)context;
    return malloc(bytes);
}

static void release(void *context, void *data)
{
    (void)context;
    free(data);
}

static int invalid_signatures(Image *image)
{
    uint8_t primary[SECTOR], backup[SECTOR], zero[SECTOR] = {0};
    return !read_at(image, 0, primary, sizeof(primary)) &&
           !read_at(image, IMAGE_BYTES - SECTOR, backup, sizeof(backup)) &&
           !memcmp(primary, zero, SECTOR) && !memcmp(backup, zero, SECTOR);
}

static int check_metadata_writes(Image *image, const IfsFsHostRuntimeV2 *writer,
                                 const IfsFsHostRuntimeV2 *reader)
{
    NtfsVolume media = {.fd = -1};
    NtfsLayout layout = {0};
    char *error = NULL;
    int status = 1;
    CHECK(ntfs_open_host(reader, &media, &error) == 0);
    CHECK(ntfs_read_layout(&media, false, &layout, &error) == 0);
    const unsigned writes_before = image->writes;
    CHECK(ntfs_set_volume_dirty(&media, &layout, true, &error) != 0);
    CHECK(image->writes == writes_before);
    free(error);
    error = NULL;
    ntfs_layout_free(&layout);
    ntfs_close_volume(&media);

    CHECK(ntfs_open_host(writer, &media, &error) == 0);
    CHECK(ntfs_read_layout(&media, false, &layout, &error) == 0);
    /* Exercise the partial-sector bitmap path without changing allocation. */
    const uint64_t bitmap_offset = layout.bitmap_runs.items[0].lcn * media.cluster_size;
    uint8_t before[SECTOR], after[SECTOR];
    CHECK(!read_at(image, bitmap_offset, before, sizeof(before)));
    CHECK(ntfs_write_stream(&media, &layout.bitmap_runs, 1u, before + 1u, 1u, &error) == 0);
    CHECK(!read_at(image, bitmap_offset, after, sizeof(after)));
    CHECK(!memcmp(before, after, sizeof(before)) && image->writes == writes_before + 1u);
    const unsigned flushes_before = image->flushes;
    CHECK(ntfs_set_volume_dirty(&media, &layout, true, &error) == 0);
    CHECK(image->writes == writes_before + 3u &&
          image->flushes == flushes_before + 1u);
    ntfs_layout_free(&layout);
    CHECK(ntfs_read_layout(&media, false, &layout, &error) != 0);
    free(error);
    error = NULL;
    CHECK(ntfs_read_layout(&media, true, &layout, &error) == 0);

    image->fail_flush = image->flushes + 1u;
    CHECK(ntfs_set_volume_dirty(&media, &layout, false, &error) != 0);
    image->fail_flush = 0u;
    free(error);
    error = NULL;
    CHECK(ntfs_set_volume_dirty(&media, &layout, false, &error) == 0);
    ntfs_layout_free(&layout);
    CHECK(ntfs_read_layout(&media, false, &layout, &error) == 0);
    status = 0;
done:
    if (error) fprintf(stderr, "%s\n", error);
    free(error);
    ntfs_layout_free(&layout);
    ntfs_close_volume(&media);
    return status;
}

static int check_map(const char *path, const char *payload)
{
    Image image = {.fd = open(path, O_RDONLY | O_NOFOLLOW)};
    int source = open(payload, O_RDONLY | O_NOFOLLOW);
    int status = 1;
    void *mounted = NULL;
    NtfsVolume media = {.fd = -1};
    char *error = NULL;
    const IfsFsModuleApiV2 *api = infiltrator_filesystem_api_v2();
    const IfsFsCoreOpsV2 *core = (const void *)ifs_fs_group_v2(api, IFS_FS_GROUP_CORE);
    const IfsFsDefragOpsV2 *ops = (const void *)ifs_fs_group_v2(api, IFS_FS_GROUP_DEFRAG);
    CHECK(image.fd >= 0 && source >= 0);
    CHECK(core && ops && ops->header.capabilities == IFS_FS_DEFRAG_MAP);
    const IfsFsHostRuntimeV2 host = {
        .abi_version = IFS_FS_HOST_ABI_V2, .structure_bytes = sizeof(host),
        .capabilities = IFS_FS_HOST_READ_ONLY, .volume_bytes = IMAGE_BYTES,
        .logical_sector_bytes = SECTOR, .physical_sector_bytes = 4096,
        .maximum_transfer_bytes = 4096, .context = &image,
        .read_at = read_at, .allocate = allocate, .release = release,
    };
    IfsFsVolumeInfoV2 info = {.structure_bytes = sizeof(info)};
    CHECK(core->mount(&host, &mounted, &info) == IFS_FS_OK && mounted);
    IfsFsExtentV2 extent = {0};
    NtfsRun runs[5];
    size_t run_count = 0u;
    uint8_t payload_data[5u * 4096u];
    uint32_t written = 7;
    CHECK(ops->map(mounted, "/missing", 0, &extent, 1, &written) == IFS_FS_NOT_FOUND && !written);
    CHECK(ops->map(mounted, "/", 0, &extent, 1, &written) == IFS_FS_UNSUPPORTED && !written);
    CHECK(ops->map(mounted, "/../Cafe.bin", 0, &extent, 1, &written) == IFS_FS_INVALID && !written);
    uint64_t bytes = 0;
    for (uint64_t page = 0;; ++page) {
        const int result = ops->map(mounted, "/CAF\xC3\x89.BIN", page, &extent, 1, &written);
        if (result == IFS_FS_END) {
            CHECK(!written && bytes == 5u * 4096u);
            break;
        }
        CHECK(result == IFS_FS_OK && written == 1);
        CHECK(extent.logical_offset == bytes && extent.length > 0 &&
              extent.length <= 5u * 4096u - bytes &&
              extent.physical_offset % info.block_bytes == 0u);
        CHECK(run_count < 5u && extent.length % info.block_bytes == 0u);
        runs[run_count++] = (NtfsRun){
            .lcn = extent.physical_offset / info.block_bytes,
            .length = extent.length / info.block_bytes,
        };
        uint8_t actual[4096], expected[4096];
        for (uint64_t offset = 0; offset < extent.length; offset += sizeof(actual)) {
            const size_t count = extent.length - offset < sizeof(actual) ?
                                 (size_t)(extent.length - offset) : sizeof(actual);
            CHECK(pread(source, expected, count, (off_t)(bytes + offset)) == (ssize_t)count);
            CHECK(pread(image.fd, actual, count, (off_t)(extent.physical_offset + offset)) == (ssize_t)count);
            CHECK(!memcmp(actual, expected, count));
            memcpy(payload_data + bytes + offset, expected, count);
        }
        bytes += extent.length;
    }
    CHECK(ntfs_open_host(&host, &media, &error) == 0);
    const NtfsStream stream = {.runs = {.items = runs, .count = run_count}};
    uint8_t digest[SHA256_DIGEST_LENGTH], expected_digest[SHA256_DIGEST_LENGTH];
    CHECK(SHA256(payload_data, sizeof(payload_data), expected_digest));
    CHECK(ntfs_stream_digest(&media, &stream, digest, &error) == 0);
    CHECK(!memcmp(digest, expected_digest, sizeof(digest)));
    status = 0;
done:
    if (error) fprintf(stderr, "%s\n", error);
    free(error);
    ntfs_close_volume(&media);
    if (mounted && core->unmount(mounted) != IFS_FS_OK) status = 1;
    if (source >= 0) close(source);
    if (image.fd >= 0) close(image.fd);
    return status;
}

int main(int argc, char **argv)
{
    if (argc == 3) return check_map(argv[1], argv[2]);
    if (argc != 2) return 2;
    Image image = {.fd = open(argv[1], O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600)};
    if (image.fd < 0) return 2;
    int status = 1;
    const IfsFsModuleApiV2 *api = infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api, "ntfs"));
    const IfsFsCoreOpsV2 *core = (const void *)ifs_fs_group_v2(api, IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume = (const void *)ifs_fs_group_v2(api, IFS_FS_GROUP_VOLUME);
    CHECK(core && core->probe && volume && volume->format);
    CHECK(ftruncate(image.fd, IMAGE_BYTES) == 0);
    IfsFsHostRuntimeV2 host = {
        .abi_version = IFS_FS_HOST_ABI_V2,
        .structure_bytes = sizeof(host),
        .capabilities = IFS_FS_HOST_EXCLUSIVE | IFS_FS_HOST_DURABLE,
        .volume_bytes = IMAGE_BYTES, .logical_sector_bytes = SECTOR,
        .physical_sector_bytes = 4096, .maximum_transfer_bytes = 4096,
        .context = &image, .read_at = read_at, .write_at = write_at,
        .flush = flush, .allocate = allocate, .release = release,
    };
    IfsFsHostRuntimeV2 reader = host;
    reader.capabilities = IFS_FS_HOST_READ_ONLY;
    reader.write_at = NULL;
    reader.flush = NULL;
    CHECK(volume->format(&reader, 1u, "Locked", 0u) == IFS_FS_ACCESS);
    CHECK(volume->format(&host, 0u, "Bad variant", 0u) == IFS_FS_UNSUPPORTED);
    CHECK(volume->format(&host, 1u, "Bad flags", 1u) == IFS_FS_UNSUPPORTED);
    CHECK(volume->format(&host, 1u, "\xC0\x80", 0u) == IFS_FS_INVALID);
    char long_label[130];
    memset(long_label, 'x', sizeof(long_label) - 1u);
    long_label[sizeof(long_label) - 1u] = '\0';
    CHECK(volume->format(&host, 1u, long_label, 0u) == IFS_FS_INVALID);
    host.logical_sector_bytes = 4096;
    CHECK(volume->format(&host, 1u, NULL, 0u) == IFS_FS_UNSUPPORTED);
    host.logical_sector_bytes = SECTOR;
    CHECK(image.writes == 0 && image.flushes == 0);

    CHECK(volume->format(&host, 1u, "Before retry", 0u) == IFS_FS_OK);
    image.writes = 0;
    image.flushes = 0;
    image.fail_flush = 2u;
    CHECK(volume->format(&host, 1u, "Uncommitted", 0u) == IFS_FS_IO_ERROR);
    CHECK(image.writes > 2 && invalid_signatures(&image));
    image.fail_flush = 0;
    image.flushes = 0;
    CHECK(volume->format(&host, 1u, "Caf\xC3\xA9 \xF0\x9F\x9A\x80", 0u) == IFS_FS_OK);
    CHECK(image.flushes >= 4);
    uint32_t variant = 0;
    CHECK(core->probe(&reader, &variant) == IFS_FS_OK && variant == 1u);
    IfsFsVolumeInfoV2 info = {.structure_bytes = sizeof(info)};
    void *mounted = NULL;
    CHECK(core->mount(&reader, &mounted, &info) == IFS_FS_OK);
    CHECK(info.variant == 1u && info.block_bytes == 4096u &&
          info.total_bytes == IMAGE_BYTES - SECTOR &&
          info.free_bytes > 0 && info.free_bytes < info.total_bytes);
    CHECK(core->unmount(mounted) == IFS_FS_OK);
    uint8_t primary[SECTOR], backup[SECTOR];
    CHECK(!read_at(&image, 0, primary, sizeof(primary)));
    CHECK(!read_at(&image, IMAGE_BYTES - SECTOR, backup, sizeof(backup)));
    CHECK(!memcmp(primary, backup, sizeof(primary)));
    CHECK(check_metadata_writes(&image, &host, &reader) == 0);
    status = 0;
done:
    if (close(image.fd)) status = 1;
    if (status) unlink(argv[1]);
    return status;
}
