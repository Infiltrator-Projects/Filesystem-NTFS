#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Compile the pinned source-embedded NTFS helpers as PIC. No system
# libntfs-3g shared object or external formatter is used for NTFS.so.
set -eu
source_root=${1:?pinned NTFS helper source directory required}
build_root=${2:?NTFS helper build directory required}
expected=78414d93613532fd82f3a82aba5d4a1c32898781
test "$(cat "$source_root/SOURCE-REVISION")" = "$expected" || {
    echo "Incorrect NTFS helper source revision" >&2
    exit 1
}
archive="$build_root/libntfs-3g/.libs/libntfs-3g.a"
if [ -s "$archive" ]; then
    exit 0
fi
test -f "$source_root/configure.ac"
# Generated autotools files are a build product, not a runtime dependency.
if [ ! -x "$source_root/configure" ]; then
    (cd "$source_root" && sh ./autogen.sh)
fi
mkdir -p "$build_root"
(cd "$build_root" && CFLAGS="-O2 -fPIC" "$source_root/configure" \
    --disable-ntfs-3g --disable-ntfsprogs --disable-shared --enable-static \
    --with-fuse=none --disable-ldconfig --disable-nfconv)
make -C "$build_root/libntfs-3g" -j2
test -s "$archive" || {
    echo "Pinned NTFS helper PIC archive was not created" >&2
    exit 1
}
