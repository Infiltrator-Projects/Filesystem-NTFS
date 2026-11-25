#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
image="$work/ntfs.img"
export LC_ALL=C.UTF-8
"$1" "$image"
"$1" "$work/ntfs-writable.img" --verify-writable
before="$(sha256sum "$image")"
# The native test verifies the Unicode label and MFT metadata without any
# external filesystem inspection or repair command.
"$1" "$image" --verify-native-label
test "$(sha256sum "$image")" = "$before"
dd if=/dev/zero of="$work/payload" bs=4096 count=5 status=none
printf 'NTFS Common V2 extent bytes\n' | dd of="$work/payload" conv=notrunc status=none
"$2" "$image" "$work/payload" '/Café.bin'
before="$(sha256sum "$image")"
"$1" "$image" "$work/payload"
test "$(sha256sum "$image")" = "$before"
