#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
test_app=$(realpath "${1:?Common V2 test executable required}")
seed_tool=$(realpath "${2:?source-built NTFS fixture seeder required}")
format_tool=$(realpath "${3:?Common V2 formatter test executable required}")
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
image="$work/native.img"
bash "$script_dir/boot-file-fixtures.sh" "$work/bootfiles"
payload="$work/bootfiles/vmlinuz"
initramfs="$work/bootfiles/initramfs"
"$format_tool" "$image"
# Real NTFS $MFT, allocation runlists and root directory index are used.
test -s "$payload" && test -s "$initramfs"
"$seed_tool" "$image" "$payload" kernel.img
"$seed_tool" "$image" "$initramfs" initramfs.img
sha256sum "$image" > "$work/source.sha"
"$test_app" "$image" "$payload" --read
(cd "$work" && sha256sum -c source.sha)
cp "$image" "$work/bad.img"
printf 'BADNTFS!' | dd of="$work/bad.img" bs=1 seek=3 conv=notrunc status=none
"$test_app" "$work/bad.img" "$payload" --reject
(cd "$work" && sha256sum -c source.sha)
echo NTFS_V2_REAL_MEDIA_AND_CORRUPTION_PASS
