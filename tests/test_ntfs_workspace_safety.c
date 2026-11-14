/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exercise NTFS workspace I/O only on a regular disposable image. The
 * replacement host functions below refuse anything except the test fixture. */
#include "ntfs_native.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "workspace test line %d: %s\n", __LINE__, #x); goto done; } } while (0)
enum { SECTOR = 512, IMAGE_SIZE = 8 * SECTOR };
static const char *fixture_path;
typedef struct {
    int fd;
    unsigned writes, flushes;
    bool fail_write, fail_flush, cancelled;
} HostFixture;
static int host_read(void *context, uint64_t offset, void *buffer, uint32_t bytes)
{
    HostFixture *h = context;
    return offset <= IMAGE_SIZE && bytes <= IMAGE_SIZE - offset &&
        pread(h->fd, buffer, bytes, (off_t)offset) == (ssize_t)bytes ? 0 : -1;
}
static int host_write(void *context, uint64_t offset, const void *buffer, uint32_t bytes)
{
    HostFixture *h = context;
    ++h->writes;
    return !h->fail_write && offset <= IMAGE_SIZE && bytes <= IMAGE_SIZE - offset &&
        pwrite(h->fd, buffer, bytes, (off_t)offset) == (ssize_t)bytes ? 0 : -1;
}
static int host_flush(void *context)
{
    HostFixture *h = context;
    ++h->flushes;
    return h->fail_flush ? -1 : fsync(h->fd);
}
static int host_cancelled(void *context) { return ((HostFixture *)context)->cancelled; }
static void *host_allocate(void *context, size_t bytes) { (void)context; return malloc(bytes); }
static void host_release(void *context, void *pointer) { (void)context; free(pointer); }

void ntfs_set_error(char **error, const char *format, ...)
{
    if (!error || *error) return;
    va_list args;
    va_start(args, format);
    if (vasprintf(error, format, args) < 0) *error = NULL;
    va_end(args);
}
void *ld_xmalloc(size_t bytes)
{
    void *p = malloc(bytes ? bytes : 1u);
    if (!p) abort();
    return p;
}
bool ld_stop_requested(void) { return false; }
ssize_t ld_pread_full(int fd, void *buffer, size_t bytes, uint64_t offset)
{
    return pread(fd, buffer, bytes, (off_t)offset);
}
ssize_t ld_pwrite_full(int fd, const void *buffer, size_t bytes, uint64_t offset)
{
    return pwrite(fd, buffer, bytes, (off_t)offset);
}
int ld_device_open_verified_fd(const char *path, bool writable,
                               const char *identity, uint64_t bytes)
{
    struct stat st;
    if (!path || !fixture_path || strcmp(path, fixture_path) || !identity ||
        strcmp(identity, "fixture") || bytes != IMAGE_SIZE) { errno = EINVAL; return -1; }
    int fd = open(path, (writable ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != IMAGE_SIZE) {
        close(fd); errno = EINVAL; return -1;
    }
    return fd;
}
static void fail_on_second_slot(sqlite3_context *context, int argc, sqlite3_value **argv)
{
    if (argc != 1 || sqlite3_value_int64(argv[0]) == 7)
        sqlite3_result_error(context, "injected workspace read failure", -1);
    else
        sqlite3_result_int64(context, sqlite3_value_int64(argv[0]));
}
int main(void)
{
    char path[] = "/tmp/ntfs-workspace-safety.XXXXXX";
    int fd = mkstemp(path), status = 1;
    sqlite3 *db = NULL;
    char *error = NULL;
    unsigned char original[IMAGE_SIZE], observed[IMAGE_SIZE];
    fixture_path = path;
    CHECK(fd >= 0);
    for (size_t i = 0; i < sizeof(original); ++i) original[i] = (unsigned char)(i / SECTOR);
    CHECK(write(fd, original, sizeof(original)) == sizeof(original));
    CHECK(sqlite3_open(":memory:", &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "CREATE TABLE workspace(old INTEGER PRIMARY KEY,target INTEGER,slot INTEGER,sha BLOB);"
                           "INSERT INTO workspace VALUES(1,3,6,NULL)", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ntfs_stage_workspace(path, db, SECTOR, "fixture", IMAGE_SIZE, &error) == 0);
    CHECK(ntfs_verify_workspace(path, db, SECTOR, "fixture", IMAGE_SIZE, &error) == 0);
    CHECK(ntfs_place_workspace(path, db, SECTOR, "fixture", IMAGE_SIZE, false, &error) == 0);
    CHECK(pread(fd, observed, sizeof(observed), 0) == sizeof(observed));
    CHECK(!memcmp(observed + 3 * SECTOR, original + SECTOR, SECTOR));
    CHECK(!memcmp(observed + 6 * SECTOR, original + SECTOR, SECTOR));
    memset(observed + SECTOR, 0xa5, SECTOR);
    CHECK(pwrite(fd, observed + SECTOR, SECTOR, SECTOR) == SECTOR);
    CHECK(ntfs_restore_workspace(path, db, SECTOR, "fixture", IMAGE_SIZE, &error) == 0);
    CHECK(pread(fd, observed, sizeof(observed), 0) == sizeof(observed));
    CHECK(!memcmp(observed + SECTOR, original + SECTOR, SECTOR));
    memcpy(original, observed, sizeof(original));

    /* Exercise the same workspace stages through the actual V2 host lease.
     * No raw device path is passed into these operations. */
    HostFixture fixture = {.fd = fd};
    IfsFsHostRuntimeV2 host = {
        .abi_version = IFS_FS_HOST_ABI_V2, .structure_bytes = sizeof(host),
        .capabilities = IFS_FS_HOST_EXCLUSIVE | IFS_FS_HOST_DURABLE | IFS_FS_HOST_CANCEL,
        .volume_bytes = IMAGE_SIZE, .logical_sector_bytes = SECTOR,
        .physical_sector_bytes = SECTOR, .maximum_transfer_bytes = SECTOR,
        .context = &fixture, .read_at = host_read, .write_at = host_write,
        .flush = host_flush, .allocate = host_allocate, .release = host_release,
        .cancelled = host_cancelled
    };
    IfsFsHostRuntimeV2 reader = host;
    reader.capabilities = IFS_FS_HOST_READ_ONLY | IFS_FS_HOST_CANCEL;
    reader.write_at = NULL; reader.flush = NULL;
    CHECK(ntfs_stage_host_workspace(&reader, db, SECTOR, &error) != 0 && error);
    free(error); error = NULL;
    CHECK(fixture.writes == 0);
    CHECK(ntfs_stage_host_workspace(&host, db, SECTOR, &error) == 0);
    CHECK(ntfs_verify_host_workspace(&host, db, SECTOR, &error) == 0);
    CHECK(ntfs_place_host_workspace(&host, db, SECTOR, false, &error) == 0);
    CHECK(pread(fd, observed, sizeof(observed), 0) == sizeof(observed));
    CHECK(!memcmp(observed + 3 * SECTOR, original + SECTOR, SECTOR));
    memset(observed + SECTOR, 0x5a, SECTOR);
    CHECK(pwrite(fd, observed + SECTOR, SECTOR, SECTOR) == SECTOR);
    CHECK(ntfs_restore_host_workspace(&host, db, SECTOR, &error) == 0);
    CHECK(pread(fd, observed, sizeof(observed), 0) == sizeof(observed));
    CHECK(!memcmp(observed + SECTOR, original + SECTOR, SECTOR));
    CHECK(fixture.writes >= 3 && fixture.flushes >= 3);
    memcpy(original, observed, sizeof(original));

    /* A failed durable flush or write must not commit workspace digests. */
    CHECK(sqlite3_exec(db, "UPDATE workspace SET sha=NULL", NULL, NULL, NULL) == SQLITE_OK);
    fixture.fail_flush = true;
    CHECK(ntfs_stage_host_workspace(&host, db, SECTOR, &error) != 0 && error);
    free(error); error = NULL; fixture.fail_flush = false;
    sqlite3_stmt *digest = NULL;
    CHECK(sqlite3_prepare_v2(db, "SELECT sha IS NULL FROM workspace", -1, &digest, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(digest) == SQLITE_ROW && sqlite3_column_int(digest, 0) == 1);
    sqlite3_finalize(digest); digest = NULL;
    fixture.fail_write = true;
    CHECK(ntfs_stage_host_workspace(&host, db, SECTOR, &error) != 0 && error);
    free(error); error = NULL; fixture.fail_write = false;
    CHECK(sqlite3_prepare_v2(db, "SELECT sha IS NULL FROM workspace", -1, &digest, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(digest) == SQLITE_ROW && sqlite3_column_int(digest, 0) == 1);
    sqlite3_finalize(digest); digest = NULL;
    fixture.cancelled = true;
    CHECK(ntfs_stage_host_workspace(&host, db, SECTOR, &error) == -2);
    fixture.cancelled = false;
    CHECK(pread(fd, observed, sizeof(observed), 0) == sizeof(observed));
    memcpy(original, observed, sizeof(original));

    /* A target past the end or a wrapped negative slot must fail without
     * modifying any byte of the image. */
    CHECK(sqlite3_exec(db, "UPDATE workspace SET target=8", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ntfs_place_workspace(path, db, SECTOR, "fixture", IMAGE_SIZE, false, &error) != 0 && error);
    free(error); error = NULL;
    CHECK(pread(fd, observed, sizeof(observed), 0) == sizeof(observed));
    CHECK(!memcmp(original, observed, sizeof(original)));
    CHECK(sqlite3_exec(db, "UPDATE workspace SET slot=-1", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ntfs_verify_workspace(path, db, SECTOR, "fixture", IMAGE_SIZE, &error) != 0 && error);
    free(error); error = NULL;
    CHECK(sqlite3_exec(db, "DELETE FROM workspace", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ntfs_place_workspace(path, db, SECTOR, "fixture", IMAGE_SIZE, false, &error) != 0 && error);
    free(error); error = NULL;
    CHECK(ntfs_place_workspace(path, db, 0u, "fixture", IMAGE_SIZE, false, &error) != 0 && error);
    free(error); error = NULL;
    CHECK(pread(fd, observed, sizeof(observed), 0) == sizeof(observed));
    CHECK(!memcmp(original, observed, sizeof(original)));

    /* One verified row followed by a database error is a failed verification. */
    CHECK(sqlite3_exec(db, "INSERT INTO workspace VALUES(1,3,6,NULL)", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ntfs_stage_workspace(path, db, SECTOR, "fixture", IMAGE_SIZE, &error) == 0);
    CHECK(sqlite3_create_function(db, "fail_slot", 1, SQLITE_UTF8, NULL,
                                  fail_on_second_slot, NULL, NULL) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "ALTER TABLE workspace RENAME TO workspace_data;"
                           "INSERT INTO workspace_data SELECT 2,4,7,sha FROM workspace_data WHERE old=1;"
                           "CREATE VIEW workspace AS SELECT old,target,fail_slot(slot) AS slot,sha FROM workspace_data;",
                       NULL, NULL, NULL) == SQLITE_OK);
    CHECK(ntfs_place_workspace(path, db, SECTOR, "fixture", IMAGE_SIZE, false, &error) != 0 && error);
    CHECK(strstr(error, "reading NTFS terminal workspace rows") != NULL);
    free(error); error = NULL;
    CHECK(ntfs_verify_workspace(path, db, SECTOR, "fixture", IMAGE_SIZE, &error) != 0 && error);
    CHECK(strstr(error, "reading NTFS workspace verification rows") != NULL);
    free(error); error = NULL;
    CHECK(ntfs_place_host_workspace(&host, db, SECTOR, false, &error) != 0 && error);
    CHECK(strstr(error, "reading NTFS terminal workspace rows") != NULL);
    free(error); error = NULL;
    CHECK(ntfs_verify_host_workspace(&host, db, SECTOR, &error) != 0 && error);
    CHECK(strstr(error, "reading NTFS workspace verification rows") != NULL);
    free(error); error = NULL;
    status = 0;
done:
    if (error) fprintf(stderr, "%s\n", error);
    free(error);
    sqlite3_close(db);
    if (fd >= 0) close(fd);
    unlink(path);
    return status;
}
