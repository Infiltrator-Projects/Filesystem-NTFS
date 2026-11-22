#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
test_app=$(realpath "${1:?Common V2 test executable required}")
copy_tool=$(realpath "${2:?ntfscp required}")
mkfs_tool=$(realpath "${3:?mkntfs required}")
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
image="$work/native.img"
bash "$(dirname "$0")/boot-file-fixtures.sh" "$work/bootfiles"
payload="$work/bootfiles/vmlinuz"
initramfs="$work/bootfiles/initramfs"
truncate -s 64M "$image"
"$mkfs_tool" -F -Q -L V2_NTFS_TEST "$image" >/dev/null
# Real NTFS $MFT, allocation runlists and root directory index are used.
test -s "$payload" && test -s "$initramfs"
"$copy_tool" -f "$image" "$payload" kernel.img
"$copy_tool" -f "$image" "$initramfs" initramfs.img
"$copy_tool" -f "$image" "$payload" initramfs.img
sha256sum "$image" > "$work/source.sha"
"$test_app" "$image" "$payload" --read
(cd "$work" && sha256sum -c source.sha)
cp "$image" "$work/bad.img"
printf 'BADNTFS!' | dd of="$work/bad.img" bs=1 seek=3 conv=notrunc status=none
"$test_app" "$work/bad.img" "$payload" --reject
(cd "$work" && sha256sum -c source.sha)
echo NTFS_V2_REAL_MEDIA_AND_CORRUPTION_PASS
