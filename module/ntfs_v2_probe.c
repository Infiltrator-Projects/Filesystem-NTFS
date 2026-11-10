/* SPDX-License-Identifier: GPL-3.0-or-later
 * Native Common V2 NTFS media probe. No V1 service binding and no fd/path I/O.
 * The host verifies raw partition ownership; this code validates its boot
 * geometry without claiming namespace or mutation operations.
 */
#include <infiltratr/filesystem_module_v2.h>
#include <stdint.h>
#include <string.h>
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint64_t le64(const uint8_t *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (i * 8u);
    return value;
}
static int native_probe(const IfsFsHostRuntimeV2 *h, uint32_t *variant) {
    uint8_t boot[512];
    if (!variant || !ifs_fs_host_v2_valid(h)) return IFS_FS_INVALID;
    if (h->volume_bytes < sizeof(boot) || h->maximum_transfer_bytes < sizeof(boot))
        return IFS_FS_NOT_FOUND;
    if (h->read_at(h->context, 0u, boot, sizeof(boot)) != 0)
        return IFS_FS_IO_ERROR;
    if (memcmp(boot + 3u, "NTFS    ", 8u) != 0 ||
        boot[510u] != 0x55u || boot[511u] != 0xaau)
        return IFS_FS_NOT_FOUND;
    const uint32_t sector = le16(boot + 11u);
    const uint32_t sectors_per_cluster = boot[13u];
    if (sector < 512u || sector > 4096u ||
        (sector & (sector - 1u)) != 0u ||
        sectors_per_cluster == 0u ||
        (sectors_per_cluster & (sectors_per_cluster - 1u)) != 0u ||
        sector < h->logical_sector_bytes ||
        sector > UINT32_MAX / sectors_per_cluster)
        return IFS_FS_CORRUPT;
    const uint32_t cluster = sector * sectors_per_cluster;
    if (cluster > 65536u) return IFS_FS_UNSUPPORTED;
    const uint64_t sectors = le64(boot + 40u);
    if (sectors == 0u || sectors > h->volume_bytes / sector ||
        sectors / sectors_per_cluster < 2u)
        return IFS_FS_CORRUPT;
    const uint64_t clusters = sectors / sectors_per_cluster;
    if (le64(boot + 48u) >= clusters || le64(boot + 56u) >= clusters)
        return IFS_FS_CORRUPT;
    const int8_t record_code = (int8_t)boot[64u];
    uint64_t record_bytes;
    if (record_code < 0) {
        if (record_code < -20) return IFS_FS_UNSUPPORTED;
        record_bytes = UINT64_C(1) << (unsigned)(-record_code);
    } else {
        record_bytes = (uint64_t)(uint8_t)record_code * cluster;
    }
    if (record_bytes < 512u || record_bytes > 1024u * 1024u ||
        record_bytes % sector != 0u)
        return IFS_FS_CORRUPT;
    *variant = 1u;
    return IFS_FS_OK;
}
static const IfsFsCoreOpsV1 core = {
    .header = { IFS_FS_GROUP_ABI_V1, sizeof(IfsFsCoreOpsV1),
                IFS_FS_GROUP_CORE, 0u, IFS_FS_CORE_PROBE },
    .probe = native_probe
};
static const IfsFsGroupHeaderV2 *query(uint32_t id, uint32_t lo, uint32_t hi) {
    return id == IFS_FS_GROUP_CORE &&
        lo <= IFS_FS_GROUP_ABI_V1 && hi >= IFS_FS_GROUP_ABI_V1
        ? &core.header : NULL;
}
static const IfsFsModuleApiV2 api = {
    IFS_FS_COMMON_ABI_V2, sizeof(IfsFsModuleApiV2), "ntfs",
    IFS_FS_PROFILE_HOSTED, IFS_FS_GROUP_BIT(IFS_FS_GROUP_CORE), NULL, query
};
__attribute__((visibility("default")))
const IfsFsModuleApiV2 *infiltrator_filesystem_api_v2(void) { return &api; }
