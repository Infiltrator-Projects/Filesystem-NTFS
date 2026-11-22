#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Create genuine kernel and static-busybox initramfs images for read tests.
# Never download a substitute filesystem driver or modify a real disk.
set -euo pipefail
out=$(realpath -m "${1:?fixture directory}")
mkdir -p "$out/download" "$out/init/bin"
if [[ $# == 2 ]]; then
    cp -- "$2" "$out/vmlinuz"
else
    image=$(apt-cache depends linux-image-generic |
        awk '/Depends: linux-image-[0-9]/ { name=$2 } END { print name }')
    test -n "$image"
    (cd "$out/download" && apt-get download "linux-image-unsigned-${image#linux-image-}")
    dpkg-deb -x "$out/download"/linux-image-unsigned-*.deb "$out/download/kernel"
    cp -- "$out/download/kernel/boot/vmlinuz-${image#linux-image-}" "$out/vmlinuz"
fi
cp -- "$(command -v busybox)" "$out/init/bin/busybox"
if readelf -lW "$out/init/bin/busybox" | grep -q INTERP; then
    echo "The initramfs needs statically linked busybox" >&2
    exit 1
fi
cat > "$out/init/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox echo "Infiltrator Common V2 filesystem test"
exec /bin/busybox sh
INIT
chmod 0755 "$out/init/init"
(cd "$out/init" && find . -print0 | LC_ALL=C sort -z |
 cpio --quiet --null -o -H newc --owner=0:0) | gzip -n > "$out/initramfs"
test -s "$out/vmlinuz" && test -s "$out/initramfs"
gzip -t "$out/initramfs"
# The generated archive must actually contain a boot-time /init program.
gzip -cd "$out/initramfs" | cpio -t --quiet | grep -Fx 'init' >/dev/null
