/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <infiltratr/filesystem_module_v2.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int fd;
    uint32_t sector;
    int fail;
} Image;

static int read_at(void *context, uint64_t offset, void *out, uint32_t length)
{
    Image *image = context;
    if (image->fail || length != image->sector || offset % image->sector)
        return -1;
    return pread(image->fd, out, length, (off_t)offset) == (ssize_t)length ? 0 : -1;
}
static void *allocate(void *context, size_t size) { (void)context; return malloc(size); }
static void release(void *context, void *ptr) { (void)context; free(ptr); }

#define REQUIRE(cond) do { if (!(cond)) { fprintf(stderr, "%d: %s\n", __LINE__, #cond); return 1; } } while (0)

int main(int argc, char **argv)
{
    REQUIRE(argc == 5);
    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    REQUIRE(module);
    void *symbol = dlsym(module, IFS_FS_COMMON_EXPORT_V2);
    IfsFsModuleApiEntryV2 entry;
    REQUIRE(symbol && sizeof(entry) == sizeof(symbol));
    memcpy(&entry, &symbol, sizeof(entry));
    const IfsFsModuleApiV2 *api = entry();
    REQUIRE(ifs_fs_api_v2_valid(api, "ntfs"));
    const char *suffix = strrchr(argv[1], '.');
    uint32_t profile = suffix && !strcmp(suffix, ".elf") ?
        IFS_FS_PROFILE_FREESTANDING | IFS_FS_PROFILE_FIRMWARE : IFS_FS_PROFILE_HOSTED;
    REQUIRE(api->runtime_profiles == profile);
    const IfsFsCoreOpsV2 *core = (const void *)ifs_fs_group_v2(api, IFS_FS_GROUP_CORE);
    REQUIRE(core && core->probe);
    if (profile & IFS_FS_PROFILE_FIRMWARE)
        REQUIRE(core->header.capabilities==IFS_FS_CORE_PROBE && !core->mount && !core->read);
    Image image = { .fd = open(argv[2], O_RDONLY), .sector = (uint32_t)strtoul(argv[3], NULL, 10) };
    struct stat status;
    REQUIRE(image.fd >= 0 && !fstat(image.fd, &status));
    IfsFsHostRuntimeV2 host = {
        .abi_version = IFS_FS_HOST_ABI_V2, .structure_bytes = IFS_FS_HOST_V2_BASE_BYTES,
        .capabilities = IFS_FS_HOST_READ_ONLY, .volume_bytes = (uint64_t)status.st_size,
        .logical_sector_bytes = image.sector, .physical_sector_bytes = image.sector,
        .maximum_transfer_bytes = image.sector, .context = &image,
        .read_at = read_at, .allocate = allocate, .release = release
    };
    uint32_t version = 0;
    int expected = !strcmp(argv[4], "clean") ? IFS_FS_OK : IFS_FS_CORRUPT;
    REQUIRE(core->probe(&host,&version)==expected);
    REQUIRE(version==(expected==IFS_FS_OK ? 1u : 0u));
    image.fail = 1;
    REQUIRE(core->probe(&host, &version) == IFS_FS_IO_ERROR && !version);
    REQUIRE(!close(image.fd) && !dlclose(module));
    return 0;
}
