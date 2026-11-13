#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
image="$work/ntfs.img"
export LC_ALL=C.UTF-8
"$1" "$image"
before="$(sha256sum "$image")"
test "$("$2" "$image")" = 'Café 🚀'
"$3" -n "$image"
"$4" --info --force "$image"
test "$(sha256sum "$image")" = "$before"
dd if=/dev/zero of="$work/payload" bs=4096 count=5 status=none
printf 'NTFS Common V2 extent bytes\n' | dd of="$work/payload" conv=notrunc status=none
"$5" "$image" "$work/payload" '/Café.bin'
before="$(sha256sum "$image")"
"$1" "$image" "$work/payload"
test "$(sha256sum "$image")" = "$before"
