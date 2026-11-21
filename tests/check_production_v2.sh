#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
module=${1:?}
test -s "$module"
nm -D --defined-only "$module" | grep -Eq '[[:space:]]infiltrator_filesystem_api_v2$'
if nm -D --defined-only "$module" |
    grep -E 'infiltrator_filesystem_.*_v1|ifs_bind_services|ifs_ntfs_worker_main'; then
    echo "Retired V1 API leaked into production NTFS.so" >&2
    exit 1
fi
if nm -a "$module" |
    grep -E 'infiltrator_filesystem_ntfs_v1|ifs_bind_services|ifs_ntfs_worker_main'; then
    echo "Retired V1 implementation included in production NTFS.so" >&2
    exit 1
fi
temp=$(mktemp -d)
trap 'rm -rf "$temp"' EXIT
objcopy --dump-section ".infiltrator_boot=$temp/ntfs.elf" "$module" "$temp/copy.so"
test -s "$temp/ntfs.elf"
test -z "$(nm -Du "$temp/ntfs.elf")"
if readelf -dW "$temp/ntfs.elf" | grep '(NEEDED)' >/dev/null; then exit 1; fi
echo NTFS_PRODUCTION_COMMON_V2_ONLY_PASS
