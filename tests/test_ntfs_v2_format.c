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
    uint32_t sector_bytes;
} Image;

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "NTFS format: %s\n", #x); goto done; } } while (0)

static int bounds(const Image *image, uint64_t offset, uint32_t bytes)
{
    const uint32_t sector = image->sector_bytes ? image->sector_bytes : SECTOR;
    return bytes && bytes <= 4096u && !(bytes % sector) &&
           !(offset % sector) && offset <= IMAGE_BYTES &&
           bytes <= IMAGE_BYTES - offset;
}

static int read_at(void *context, uint64_t offset, void *data, uint32_t bytes)
{
    Image *image = context;
    return !bounds(image, offset, bytes) ||
           pread(image->fd, data, bytes, (off_t)offset) != (ssize_t)bytes;
}

static int write_at(void *context, uint64_t offset, const void *data, uint32_t bytes)
{
    Image *image = context;
    ++image->writes;
    return !bounds(image, offset, bytes) ||
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

static void failing_plan_value(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    if (argc != 1 || sqlite3_value_int64(argv[0]) == 4)
        sqlite3_result_error(context, "injected plan read failure", -1);
    else
        sqlite3_result_int64(context, sqlite3_value_int64(argv[0]));
}

static int check_workspace_map(void)
{
    sqlite3 *db = NULL;
    sqlite3_stmt *statement = NULL;
    char *error = NULL;
    int status = 1;
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "CREATE TABLE blocks(old INTEGER PRIMARY KEY,target INTEGER);"
                           "INSERT INTO blocks VALUES(1,2),(3,4)",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ntfs_prepare_workspace_map(db, 100u, 2u, &error) == 0);
    CHECK(sqlite3_prepare_v2(db, "SELECT slot FROM workspace ORDER BY old", -1,
                             &statement, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int64(statement, 0) == 100);
    CHECK(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int64(statement, 0) == 101);
    CHECK(sqlite3_step(statement) == SQLITE_DONE);
    sqlite3_finalize(statement); statement = NULL;
    CHECK(sqlite3_exec(db, "CREATE TRIGGER deny_workspace_delete BEFORE DELETE ON workspace "
                           "BEGIN SELECT RAISE(ABORT,'injected delete failure'); END;",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ntfs_prepare_workspace_map(db, 100u, 2u, &error) != 0 && error);
    CHECK(sqlite3_get_autocommit(db) != 0);
    free(error); error = NULL;
    CHECK(sqlite3_exec(db, "DROP TRIGGER deny_workspace_delete", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ntfs_prepare_workspace_map(db, (uint64_t)INT64_MAX, 2u, &error) != 0 && error);
    free(error); error = NULL;
    CHECK(sqlite3_exec(db, "UPDATE blocks SET old=-1 WHERE old=1", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ntfs_prepare_workspace_map(db, 100u, 2u, &error) != 0 && error);
    free(error); error = NULL;
    CHECK(sqlite3_create_function(db, "fail_plan", 1, SQLITE_UTF8, NULL,
                                  failing_plan_value, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "DROP TABLE blocks;"
                           "CREATE TABLE raw_blocks(old INTEGER PRIMARY KEY,target INTEGER);"
                           "INSERT INTO raw_blocks VALUES(1,2),(3,4);"
                           "CREATE VIEW blocks AS SELECT old,fail_plan(target) AS target FROM raw_blocks;",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ntfs_prepare_workspace_map(db, 100u, 2u, &error) != 0 && error);
    CHECK(strstr(error, "reading NTFS terminal workspace map") != NULL);
    free(error); error = NULL;
    /* Failed replacement rolled back to the previously complete workspace. */
    CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM workspace", -1,
                             &statement, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 2);
    status = 0;
done:
    if (error) fprintf(stderr, "%s\n", error);
    free(error);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return status;
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
    uint8_t *mirror_saved = NULL, *mirror_work = NULL, *mirror_fixed = NULL;
    NtfsAttributeVec mirror_attrs = {0};
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
    /* A damaged mirror must fail before either on-disk copy is changed. The
     * image belongs to this test; setup writes bypass the host write counter. */
    const uint64_t mirror_offset = media.mftmirr_lcn * media.cluster_size +
                                   3ULL * media.record_size;
    mirror_saved = malloc(media.record_size);
    mirror_work = malloc(media.record_size);
    mirror_fixed = malloc(media.record_size);
    CHECK(mirror_saved && mirror_work && mirror_fixed);
    CHECK(pread(image->fd, mirror_saved, media.record_size, (off_t)mirror_offset) ==
          (ssize_t)media.record_size);
    memcpy(mirror_work, mirror_saved, media.record_size);
    mirror_work[6] = mirror_work[7] = 0; /* invalid update-sequence count */
    CHECK(pwrite(image->fd, mirror_work, media.record_size, (off_t)mirror_offset) ==
          (ssize_t)media.record_size);
    CHECK(ntfs_set_volume_dirty(&media, &layout, true, &error) != 0 && error);
    CHECK(image->writes == writes_before);
    free(error); error = NULL;
    CHECK(ntfs_apply_fixups(mirror_saved, media.record_size, media.bytes_per_sector,
                            mirror_fixed, &error) == 0);
    CHECK(ntfs_parse_attributes(mirror_fixed, media.record_size, &mirror_attrs, &error) == 0);
    size_t volume_attr = 0;
    for (size_t i = 0; i < mirror_attrs.count; ++i)
        if (mirror_attrs.items[i].type == NTFS_ATTR_VOLUME_INFORMATION &&
            !mirror_attrs.items[i].nonresident) {
            volume_attr = mirror_attrs.items[i].offset;
            break;
        }
    CHECK(volume_attr != 0);
    ntfs_put_u16(mirror_fixed, volume_attr + 20U, 0xfff0U);
    CHECK(ntfs_prepare_fixups(mirror_fixed, media.record_size, media.bytes_per_sector,
                              mirror_work, &error) == 0);
    CHECK(pwrite(image->fd, mirror_work, media.record_size, (off_t)mirror_offset) ==
          (ssize_t)media.record_size);
    CHECK(ntfs_set_volume_dirty(&media, &layout, true, &error) != 0 && error);
    CHECK(image->writes == writes_before);
    free(error); error = NULL;
    CHECK(pwrite(image->fd, mirror_saved, media.record_size, (off_t)mirror_offset) ==
          (ssize_t)media.record_size);
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
    ntfs_attributes_free(&mirror_attrs);
    free(mirror_saved);
    free(mirror_work);
    free(mirror_fixed);
    ntfs_layout_free(&layout);
    ntfs_close_volume(&media);
    return status;
}

/* Exercise the actual NTFS metadata-plan implementation against the same
 * bounded host lease used by Common V2. No real block device is involved. */
static int check_host_metadata_plan(Image *image, const IfsFsHostRuntimeV2 *writer,
                                    const IfsFsHostRuntimeV2 *reader)
{
    NtfsVolume writable = {.fd = -1}, readonly = {.fd = -1};
    NtfsLayout layout = {0};
    sqlite3 *plan = NULL;
    sqlite3_stmt *statement = NULL;
    char *error = NULL;
    int status = 1;
    CHECK(ntfs_open_host(reader, &readonly, &error) == 0);
    CHECK(ntfs_open_host(writer, &writable, &error) == 0);
    CHECK(ntfs_read_layout(&writable, false, &layout, &error) == 0);
    CHECK(sqlite3_open(":memory:", &plan) == SQLITE_OK);
    CHECK(sqlite3_exec(plan,
        "CREATE TABLE streams(record INTEGER,attr INTEGER,clusters INTEGER,target INTEGER);"
        "CREATE TABLE metadata(key TEXT PRIMARY KEY,value BLOB);",
        NULL, NULL, NULL) == SQLITE_OK);
    const unsigned before = image->writes;
    CHECK(ntfs_apply_host_metadata(&readonly, plan, false, &error) != 0 && error);
    CHECK(image->writes == before);
    free(error); error = NULL;
    IfsFsHostRuntimeV2 unleased = *writer;
    unleased.capabilities &= ~IFS_FS_HOST_EXCLUSIVE;
    NtfsVolume forged = writable;
    forged.host = &unleased;
    CHECK(ntfs_apply_host_metadata(&forged, plan, false, &error) != 0 && error);
    CHECK(image->writes == before);
    free(error); error = NULL;
    /* A missing or malformed bitmap must fail before the first disk write. */
    CHECK(ntfs_apply_host_metadata(&writable, plan, false, &error) != 0 && error);
    CHECK(image->writes == before);
    free(error); error = NULL;
    CHECK(sqlite3_exec(plan, "INSERT INTO metadata VALUES('bitmap',x'00')",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ntfs_apply_host_metadata(&writable, plan, false, &error) != 0 && error);
    CHECK(image->writes == before);
    free(error); error = NULL;
    CHECK(layout.bitmap_bytes <= INT32_MAX);
    CHECK(sqlite3_prepare_v2(plan, "UPDATE metadata SET value=? WHERE key='bitmap'", -1,
                             &statement, NULL) == SQLITE_OK);
    CHECK(sqlite3_bind_blob(statement, 1, layout.bitmap, (int)layout.bitmap_bytes,
                            SQLITE_TRANSIENT) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_DONE);
    sqlite3_finalize(statement); statement = NULL;
    CHECK(sqlite3_exec(plan, "INSERT INTO streams VALUES(24,0,1,-1)",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ntfs_apply_host_metadata(&writable, plan, false, &error) != 0 && error);
    CHECK(image->writes == before);
    free(error); error = NULL;
    CHECK(sqlite3_exec(plan, "DELETE FROM streams", NULL, NULL, NULL) == SQLITE_OK);
    const unsigned flushes = image->flushes;
    CHECK(ntfs_apply_host_metadata(&writable, plan, false, &error) == 0);
    CHECK(image->writes > before && image->flushes == flushes + 1u);
    image->fail_flush = image->flushes + 1u;
    CHECK(ntfs_apply_host_metadata(&writable, plan, false, &error) != 0 && error);
    image->fail_flush = 0u;
    free(error); error = NULL;
    CHECK(ntfs_apply_host_metadata(&writable, plan, false, &error) == 0);
    status = 0;
done:
    image->fail_flush = 0u;
    if (error) fprintf(stderr, "%s\n", error);
    free(error);
    sqlite3_finalize(statement);
    sqlite3_close(plan);
    ntfs_layout_free(&layout);
    ntfs_close_volume(&readonly);
    ntfs_close_volume(&writable);
    return status;
}

/* Cross-check the UTF-16 volume label through this project's independent
 * native MFT parser, not an external ntfslabel command or test subprocess.
 * This function has read-only host permissions and must never mutate media. */
static int check_native_volume_label(const char *path)
{
    Image image = {.fd = open(path, O_RDONLY | O_NOFOLLOW), .sector_bytes = 512u};
    NtfsVolume volume = {.fd = -1};
    NtfsLayout layout = {0};
    NtfsAttributeVec attributes = {0};
    uint8_t *raw = NULL, *fixed = NULL;
    char *error = NULL;
    int result = 1;
    const uint8_t expected[] = {
        'C', 0, 'a', 0, 'f', 0, 0xe9, 0, ' ', 0,
        0x3d, 0xd8, 0x80, 0xde
    };
    CHECK(image.fd >= 0);
    const IfsFsHostRuntimeV2 host = {
        .abi_version = IFS_FS_HOST_ABI_V2, .structure_bytes = sizeof(host),
        .capabilities = IFS_FS_HOST_READ_ONLY, .volume_bytes = IMAGE_BYTES,
        .logical_sector_bytes = 512u, .physical_sector_bytes = 4096u,
        .maximum_transfer_bytes = 4096u, .context = &image,
        .read_at = read_at, .allocate = allocate, .release = release,
    };
    CHECK(ntfs_open_host(&host, &volume, &error) == 0);
    CHECK(ntfs_read_layout(&volume, false, &layout, &error) == 0);
    CHECK(ntfs_read_record(&volume, &layout.mft_runs, 3u, &raw, &fixed, &error) == 0);
    CHECK(ntfs_parse_attributes(fixed, volume.record_size, &attributes, &error) == 0);
    unsigned matched = 0;
    for (size_t i = 0; i < attributes.count; ++i) {
        const NtfsAttribute *a = &attributes.items[i];
        if (a->type != UINT32_C(0x60)) continue;
        CHECK(!a->nonresident && a->length >= 24u);
        const uint32_t value_len = ntfs_u32(fixed, a->offset + 16u);
        const uint32_t value_off = ntfs_u16(fixed, a->offset + 20u);
        CHECK(value_off >= 24u && value_off <= a->length);
        CHECK(value_len == sizeof(expected) && value_len <= a->length - value_off);
        CHECK(memcmp(fixed + a->offset + value_off, expected, sizeof(expected)) == 0);
        ++matched;
    }
    CHECK(matched == 1u);
    result = 0;
done:
    if (error) fprintf(stderr, "%s\n", error);
    free(error);
    ntfs_attributes_free(&attributes);
    free(raw);
    free(fixed);
    ntfs_layout_free(&layout);
    ntfs_close_volume(&volume);
    if (image.fd >= 0 && close(image.fd)) result = 1;
    return result;
}

static int check_map(const char *path, const char *payload)
{
    Image image = {.fd = open(path, O_RDONLY | O_NOFOLLOW)};
    int source = open(payload, O_RDONLY | O_NOFOLLOW);
    int status = 1;
    void *mounted = NULL;
    IfsFsHostRuntimeV2 *short_host = NULL;
    NtfsVolume media = {.fd = -1};
    NtfsLayout layout = {0};
    NtfsCatalogue catalogue = {0};
    sqlite3 *plan = NULL;
    sqlite3_stmt *record = NULL;
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
    /* An earlier V2 host ends immediately before the optional entropy
     * extension. Allocate only that many bytes so ASan detects an overread. */
    volatile size_t short_bytes = IFS_FS_HOST_V2_BASE_BYTES;
    short_host = malloc(short_bytes);
    CHECK(short_host);
    memcpy(short_host, &host, IFS_FS_HOST_V2_BASE_BYTES);
    short_host->structure_bytes = IFS_FS_HOST_V2_BASE_BYTES;
    CHECK(core->mount(short_host, &mounted, &info) == IFS_FS_OK && mounted);
    CHECK(core->unmount(mounted) == IFS_FS_OK);
    mounted = NULL;
    free(short_host);
    short_host = NULL;
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

    /* Use the real catalogue to bind the plan to the provider's MFT
     * identity and verify it through the same open read-only media host. */
    CHECK(ntfs_read_layout(&media, false, &layout, &error) == 0);
    CHECK(ntfs_scan_catalogue(&media, &layout, &catalogue, &error) == 0);
    const NtfsStream *file = NULL;
    for (size_t i = 0; i < catalogue.count; ++i) {
        const NtfsStream *candidate = &catalogue.items[i];
        if (candidate->record_number >= NTFS_FIRST_USER_RECORD &&
            candidate->attribute_type == NTFS_ATTR_DATA &&
            !candidate->attribute_name[0] &&
            candidate->runs.count == 1u && run_count == 1u &&
            candidate->runs.items[0].lcn == runs[0].lcn &&
            candidate->runs.items[0].length == runs[0].length) {
            file = candidate;
            break;
        }
    }
    CHECK(file);
    CHECK(sqlite3_open(":memory:", &plan) == SQLITE_OK);
    CHECK(sqlite3_exec(plan,
        "CREATE TABLE streams(record,attr,clusters,target,reserve,sha);"
        "CREATE TABLE fixed_primary(record,attr,clusters,start,reserve,sha);",
        NULL, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(plan, "INSERT INTO streams VALUES (?,?,?,?,0,?)",
                             -1, &record, NULL) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(record, 1, (sqlite3_int64)file->record_number) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(record, 2, file->attribute_offset) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(record, 3, (sqlite3_int64)runs[0].length) == SQLITE_OK);
    CHECK(sqlite3_bind_int64(record, 4, (sqlite3_int64)runs[0].lcn) == SQLITE_OK);
    CHECK(sqlite3_bind_zeroblob(record, 5, SHA256_DIGEST_LENGTH) == SQLITE_OK);
    CHECK(sqlite3_step(record) == SQLITE_DONE);
    sqlite3_finalize(record);
    record = NULL;
    CHECK(ntfs_verify_volume(&media, plan, false, false, &error) != 0 && error);
    CHECK(strstr(error, "payload checksum changed") != NULL);
    free(error);
    error = NULL;
    CHECK(sqlite3_prepare_v2(plan, "UPDATE streams SET sha=?", -1, &record, NULL) == SQLITE_OK);
    CHECK(sqlite3_bind_blob(record, 1, expected_digest, sizeof(expected_digest), SQLITE_STATIC) == SQLITE_OK);
    CHECK(sqlite3_step(record) == SQLITE_DONE);
    CHECK(ntfs_verify_volume(&media, plan, false, false, &error) == 0);
    status = 0;
done:
    if (error) fprintf(stderr, "%s\n", error);
    free(error);
    sqlite3_finalize(record);
    sqlite3_close(plan);
    ntfs_catalogue_free(&catalogue);
    ntfs_layout_free(&layout);
    ntfs_close_volume(&media);
    if (mounted && core->unmount(mounted) != IFS_FS_OK) status = 1;
    free(short_host);
    if (source >= 0) close(source);
    if (image.fd >= 0) close(image.fd);
    return status;
}

/* Independent real-sector volume: Common V2 formats the media and the
 * native reader must mount the genuine logical-sector geometry. Separate
 * 512-byte and 4Kn regressions remain mandatory. */
static int check_sector_format(const char *path, uint32_t sector_bytes)
{
    Image image = {.fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0600),
                   .sector_bytes = sector_bytes};
    if (image.fd < 0) return 2;
    int result = 1;
    const IfsFsModuleApiV2 *api = infiltrator_filesystem_api_v2();
    CHECK(ifs_fs_api_v2_valid(api, "ntfs"));
    const IfsFsCoreOpsV2 *core = (const void *)ifs_fs_group_v2(api, IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume = (const void *)ifs_fs_group_v2(api, IFS_FS_GROUP_VOLUME);
    CHECK(core && core->probe && core->mount && core->unmount && volume && volume->format);
    CHECK(ftruncate(image.fd, IMAGE_BYTES) == 0);
    IfsFsHostRuntimeV2 host = {
        .abi_version = IFS_FS_HOST_ABI_V2, .structure_bytes = sizeof(host),
        .capabilities = IFS_FS_HOST_EXCLUSIVE | IFS_FS_HOST_DURABLE,
        .volume_bytes = IMAGE_BYTES, .logical_sector_bytes = sector_bytes,
        .physical_sector_bytes = 4096u, .maximum_transfer_bytes = 4096u,
        .context = &image, .read_at = read_at, .write_at = write_at,
        .flush = flush, .allocate = allocate, .release = release,
    };
    CHECK(volume->format(&host, 1u, "Native geometry", 0u) == IFS_FS_OK);
    CHECK(image.writes > 0u && image.flushes > 0u);
    uint8_t primary[4096], backup[4096];
    CHECK(read_at(&image, 0, primary, sector_bytes) == 0);
    CHECK(read_at(&image, IMAGE_BYTES - sector_bytes, backup, sector_bytes) == 0);
    CHECK(memcmp(primary, backup, sector_bytes) == 0);
    CHECK(primary[510] == 0x55u && primary[511] == 0xaau);
    CHECK(primary[11] == (uint8_t)sector_bytes && primary[12] == (uint8_t)(sector_bytes >> 8));
    IfsFsHostRuntimeV2 reader = host;
    reader.capabilities = IFS_FS_HOST_READ_ONLY;
    reader.write_at = NULL;
    reader.flush = NULL;
    uint32_t variant = 0;
    CHECK(core->probe(&reader, &variant) == IFS_FS_OK && variant == 1u);
    IfsFsVolumeInfoV2 info = {.structure_bytes = sizeof(info)};
    void *mounted = NULL;
    const int mount_result = core->mount(&reader, &mounted, &info);
    if (mount_result != IFS_FS_OK) {
        NtfsVolume media = {.fd = -1};
        NtfsLayout layout = {0};
        char *error = NULL;
        int opened = ntfs_open_host(&reader, &media, &error);
        int laid_out = opened ? -1 : ntfs_read_layout(&media, false, &layout, &error);
        fprintf(stderr, "sector=%u mount=%d, native_open=%d, native_layout=%d, reason=%s\n",
                sector_bytes, mount_result, opened, laid_out, error ? error : "(none)");
        free(error);
        ntfs_layout_free(&layout);
        ntfs_close_volume(&media);
    }
    CHECK(mount_result == IFS_FS_OK);
    CHECK(info.variant == 1u && info.block_bytes == 4096u &&
          info.total_bytes == IMAGE_BYTES - sector_bytes);
    CHECK(core->unmount(mounted) == IFS_FS_OK);
    result = 0;
done:
    if (close(image.fd)) result = 1;
    if (result) unlink(path);
    return result;
}

/* A separate image preserves the original formatter's Unicode-label and
 * metadata fixtures. Exercise native writable CORE without a seed program. */
static int check_writable(const char *path)
{
    Image image={.fd=open(path,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW,0600)};
    int status=1;
    void *mounted=NULL;
    const IfsFsModuleApiV2 *api=infiltrator_filesystem_api_v2();
    const IfsFsCoreOpsV2 *core=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    const IfsFsVolumeOpsV2 *volume=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    const IfsFsDefragOpsV2 *defrag=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_DEFRAG);
    CHECK(image.fd>=0 && ftruncate(image.fd,IMAGE_BYTES)==0);
    CHECK(core && core->create && core->write && core->remove && defrag && defrag->map);
    IfsFsHostRuntimeV2 host={
        .abi_version=IFS_FS_HOST_ABI_V2,.structure_bytes=sizeof(host),
        .capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE,
        .volume_bytes=IMAGE_BYTES,.logical_sector_bytes=SECTOR,
        .physical_sector_bytes=4096u,.maximum_transfer_bytes=4096u,
        .context=&image,.read_at=read_at,.write_at=write_at,.flush=flush,
        .allocate=allocate,.release=release
    };
    IfsFsHostRuntimeV2 reader=host;
    reader.capabilities=IFS_FS_HOST_READ_ONLY;reader.write_at=NULL;reader.flush=NULL;
    CHECK(volume->format(&host,1u,"V2 writable test",0u)==IFS_FS_OK);
    IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    CHECK(core->mount(&host,&mounted,&info)==IFS_FS_OK);
    void *peer=NULL;
    CHECK(core->mount(&reader,&peer,&info)==IFS_FS_BUSY && !peer);
    CHECK(core->create(mounted,"/V2-A.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    CHECK(core->create(mounted,"/V2-B.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    /* NTFS stores a small unnamed stream inside its MFT record.
     * Check every physical byte, including any USA-redirected tail. */
    uint8_t small[300];
    for(size_t i=0;i<sizeof(small);++i)small[i]=(uint8_t)(i*23u+9u);
    CHECK(core->create(mounted,"/RESIDENT.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    CHECK(core->write(mounted,"/RESIDENT.BIN",0,small,sizeof(small))==IFS_FS_OK);
    /* Exercise the provider's native directory indices and nested writes;
     * invalid components must not turn into host paths or metadata writes. */
    CHECK(core->create(mounted,"/V2-DIR",IFS_FS_ENTRY_DIRECTORY)==IFS_FS_OK);
    CHECK(core->create(mounted,"/V2-DIR/SUB",IFS_FS_ENTRY_DIRECTORY)==IFS_FS_OK);
    CHECK(core->create(mounted,"/V2-DIR/SUB/DATA.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    CHECK(core->write(mounted,"/V2-DIR/SUB/DATA.BIN",0,small,sizeof(small))==IFS_FS_OK);
    CHECK(core->create(mounted,"/V2-DIR/../ESCAPE",IFS_FS_ENTRY_FILE)!=IFS_FS_OK);
    CHECK(core->create(mounted,"/V2-DIR//ESCAPE",IFS_FS_ENTRY_FILE)!=IFS_FS_OK);
    CHECK(core->create(mounted,"/MISSING/DATA.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_NOT_FOUND);
    CHECK(core->write(mounted,"/V2-DIR/SUB",0,small,sizeof(small))!=IFS_FS_OK);
    /* Allocate a disposable ordinary file, then remove it using the same
     * provider as production. Verify the parent guard and read-only lease
     * separately, and confirm actual allocation accounting after remount. */
    uint8_t payload[65536],actual[65536];
    memset(payload,0x7c,sizeof(payload));
    CHECK(core->create(mounted,"/V2-DIR/SUB/REMOVE.BIN",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    for(unsigned chunk=0;chunk<16u;++chunk)
        CHECK(core->write(mounted,"/V2-DIR/SUB/REMOVE.BIN",
                         (uint64_t)chunk*sizeof(payload),payload,sizeof(payload))==IFS_FS_OK);
    CHECK(volume->query(mounted,&info)==IFS_FS_OK);
    const uint64_t allocated_free=info.free_bytes;
    CHECK(core->remove(mounted,"/V2-DIR/SUB")==IFS_FS_BUSY);
    CHECK(core->remove(mounted,"/V2-DIR/SUB/REMOVE.BIN")==IFS_FS_OK);
    CHECK(core->remove(mounted,"/V2-DIR/SUB/REMOVE.BIN")==IFS_FS_NOT_FOUND);
    CHECK(volume->query(mounted,&info)==IFS_FS_OK &&
          info.free_bytes>=allocated_free+16u*sizeof(payload));
    CHECK(core->create(mounted,"/V2-DIR/EMPTY",IFS_FS_ENTRY_DIRECTORY)==IFS_FS_OK);
    CHECK(core->remove(mounted,"/V2-DIR/EMPTY")==IFS_FS_OK);
    for(unsigned chunk=0;chunk<32u;++chunk) {
        for(size_t i=0;i<sizeof(payload);++i)
            payload[i]=(uint8_t)(i*17u+chunk);
        CHECK(core->write(mounted,"/V2-A.BIN",(uint64_t)chunk*sizeof(payload),
                          payload,sizeof(payload))==IFS_FS_OK);
        for(size_t i=0;i<sizeof(payload);++i)payload[i]^=0x5au;
        CHECK(core->write(mounted,"/V2-B.BIN",(uint64_t)chunk*sizeof(payload),
                          payload,sizeof(payload))==IFS_FS_OK);
    }
    CHECK(core->unmount(mounted)==IFS_FS_OK);mounted=NULL;
    CHECK(core->mount(&reader,&mounted,&info)==IFS_FS_OK);
    for(unsigned chunk=0;chunk<32u;++chunk) {
        for(size_t i=0;i<sizeof(payload);++i)payload[i]=(uint8_t)(i*17u+chunk);
        CHECK(core->read(mounted,"/V2-A.BIN",(uint64_t)chunk*sizeof(payload),
                         actual,sizeof(actual))==IFS_FS_OK);
        CHECK(memcmp(payload,actual,sizeof(payload))==0);
        for(size_t i=0;i<sizeof(payload);++i)payload[i]^=0x5au;
        CHECK(core->read(mounted,"/V2-B.BIN",(uint64_t)chunk*sizeof(payload),
                         actual,sizeof(actual))==IFS_FS_OK);
        CHECK(memcmp(payload,actual,sizeof(payload))==0);
    }
    IfsFsExtentV2 ranges[8];uint32_t mapped=0;
    CHECK(defrag->map(mounted,"/V2-A.BIN",0,ranges,8,&mapped)==IFS_FS_OK && mapped);
    CHECK(core->read(mounted,"/RESIDENT.BIN",0,actual,sizeof(small))==IFS_FS_OK);
    CHECK(!memcmp(small,actual,sizeof(small)));
    char nested_name[256];
    IfsFsEntryV2 nested={.structure_bytes=sizeof(nested),.name=nested_name,
                         .name_capacity=sizeof(nested_name)};
    CHECK(core->stat(mounted,"/V2-DIR/SUB",&nested)==IFS_FS_OK &&
          nested.type==IFS_FS_ENTRY_DIRECTORY);
    CHECK(core->stat(mounted,"/V2-DIR/SUB/DATA.BIN",&nested)==IFS_FS_OK &&
          nested.type==IFS_FS_ENTRY_FILE && nested.size_bytes==sizeof(small));
    CHECK(core->read(mounted,"/V2-DIR/SUB/DATA.BIN",0,actual,sizeof(small))==IFS_FS_OK);
    CHECK(!memcmp(small,actual,sizeof(small)));
    CHECK(core->stat(mounted,"/ESCAPE",&nested)==IFS_FS_NOT_FOUND);
    CHECK(core->stat(mounted,"/V2-DIR/SUB/REMOVE.BIN",&nested)==IFS_FS_NOT_FOUND);
    CHECK(core->stat(mounted,"/V2-DIR/EMPTY",&nested)==IFS_FS_NOT_FOUND);
    uint64_t covered=0u;
    for(uint64_t page=0u;page<=sizeof(small);++page) {
        mapped=99u;
        int rc=defrag->map(mounted,"/RESIDENT.BIN",page,ranges,1u,&mapped);
        if(rc==IFS_FS_END){CHECK(!mapped && covered==sizeof(small));break;}
        CHECK(rc==IFS_FS_OK && mapped==1u && ranges[0].logical_offset==covered);
        CHECK(ranges[0].length && covered<=sizeof(small) &&
              ranges[0].length<=sizeof(small)-covered);
        CHECK(!ranges[0].flags && !ranges[0].reserved);
        CHECK(pread(image.fd,actual,(size_t)ranges[0].length,
                    (off_t)ranges[0].physical_offset)==(ssize_t)ranges[0].length);
        CHECK(!memcmp(actual,small+covered,(size_t)ranges[0].length));
        covered+=ranges[0].length;
    }
    CHECK(covered==sizeof(small));
    const unsigned before=image.writes;
    CHECK(core->write(mounted,"/V2-A.BIN",0,payload,sizeof(payload))==IFS_FS_ACCESS);
    CHECK(core->create(mounted,"/REFUSED",IFS_FS_ENTRY_FILE)==IFS_FS_ACCESS);
    CHECK(core->remove(mounted,"/V2-DIR/SUB/DATA.BIN")==IFS_FS_ACCESS);
    CHECK(image.writes==before);
    CHECK(core->unmount(mounted)==IFS_FS_OK);mounted=NULL;
    status=0;
done:
    if(mounted && core && core->unmount)(void)core->unmount(mounted);
    if(image.fd>=0 && close(image.fd))status=1;
    if(status)unlink(path);
    return status;
}
int main(int argc, char **argv)
{
    if(argc==3 && !strcmp(argv[2],"--verify-writable"))
        return check_writable(argv[1]);
    if (argc == 3 && !strcmp(argv[2], "--verify-native-label"))
        return check_native_volume_label(argv[1]);
    if (argc == 3 &&
        (!strcmp(argv[2], "--sector-1024") ||
         !strcmp(argv[2], "--sector-2048") ||
         !strcmp(argv[2], "--sector-4096"))) {
        const uint32_t sector_bytes = (uint32_t)strtoul(argv[2] + 9, NULL, 10);
        return check_sector_format(argv[1], sector_bytes);
    }
    if (argc == 3) return check_map(argv[1], argv[2]);
    if (argc != 2) return 2;
    if (check_workspace_map() != 0) return 1;
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
    host.logical_sector_bytes = 8192u;
    const int geometry_error = volume->format(&host, 1u, NULL, 0u);
    CHECK(geometry_error == IFS_FS_INVALID || geometry_error == IFS_FS_UNSUPPORTED);
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
    /* A format attempt must not invalidate a live read-only session,
     * even if the caller supplies a second, nominally exclusive host. */
    const unsigned mounted_writes=image.writes, mounted_flushes=image.flushes;
    CHECK(volume->format(&host,1u,"Must not overwrite",0u)==IFS_FS_BUSY);
    CHECK(image.writes==mounted_writes && image.flushes==mounted_flushes);
    char read_label[768],tiny_label[2]={'X','X'};
    uint32_t label_bytes=UINT32_MAX;
    CHECK(volume->label && !volume->set_label &&
          volume->label(mounted,read_label,sizeof(read_label),&label_bytes)==IFS_FS_OK &&
          !strcmp(read_label,"Caf\xC3\xA9 \xF0\x9F\x9A\x80") && label_bytes==10u);
    CHECK(volume->label(mounted,tiny_label,sizeof(tiny_label),&label_bytes)==IFS_FS_NO_SPACE &&
          !label_bytes && tiny_label[0]=='X' && tiny_label[1]=='X');
    CHECK(info.variant == 1u && info.block_bytes == 4096u &&
          info.total_bytes == IMAGE_BYTES - SECTOR &&
          info.free_bytes > 0 && info.free_bytes < info.total_bytes);
    CHECK(core->unmount(mounted) == IFS_FS_OK);
    uint8_t primary[SECTOR], backup[SECTOR];
    CHECK(!read_at(&image, 0, primary, sizeof(primary)));
    CHECK(!read_at(&image, IMAGE_BYTES - SECTOR, backup, sizeof(backup)));
    CHECK(!memcmp(primary, backup, sizeof(primary)));
    CHECK(check_metadata_writes(&image, &host, &reader) == 0);
    CHECK(check_host_metadata_plan(&image, &host, &reader) == 0);
    status = 0;
done:
    if (close(image.fd)) status = 1;
    if (status) unlink(argv[1]);
    return status;
}
