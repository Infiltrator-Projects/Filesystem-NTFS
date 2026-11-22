#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
probe=$(realpath "${1:?probe harness}")
hosted=$(realpath "${2:?namespace harness}")
module=$(realpath "${3:?installed NTFS.so}")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
objcopy --dump-section .infiltrator_boot=reader.elf "$module" copy.so
test -s reader.elf
test -z "$(nm -Du reader.elf)"
if readelf -dW reader.elf | grep '(NEEDED)' >/dev/null; then exit 1; fi
head -c 65536 /dev/urandom > source
for disk_sector in 512 4096; do
    truncate -s 64M good.img
    mkntfs -F -Q -s "$disk_sector" good.img >/dev/null
    ntfscp -f good.img source kernel.img
    ntfscp -f good.img source initramfs.img
    cp --sparse=always --reflink=auto good.img bad-mft.img
    cp --sparse=always --reflink=auto good.img bad-geometry.img
    python3 - <<'MEDIA'
with open('good.img','rb') as f:
    b=f.read(512)
sector=int.from_bytes(b[11:13],'little')
cluster=sector*b[13]
mft=int.from_bytes(b[48:56],'little')*cluster
with open('bad-mft.img','r+b') as f:
    f.seek(mft)
    r=f.read(max(4096,sector)); assert r[:4]==b'FILE'
    assert int.from_bytes(r[6:8],'little')==int.from_bytes(r[28:32],'little')//512+1
    f.seek(mft+510); f.write(bytes([r[510]^1]))
with open('bad-geometry.img','r+b') as f:
    f.seek(56); f.write(b[48:56])
MEDIA
    sha256sum good.img bad-mft.img bad-geometry.img > media.sha
    for host_sector in 512 4096; do
        for profile in "$module" "$work/reader.elf"; do
            "$probe" "$profile" good.img "$host_sector" clean
            "$probe" "$profile" bad-geometry.img "$host_sector" corrupt
        done
        "$hosted" good.img source --read "$host_sector"
        "$hosted" bad-mft.img source --bad-mft "$host_sector"
        "$hosted" good.img source --read "$host_sector" "$work/reader.elf"
        "$hosted" bad-mft.img source --bad-mft "$host_sector" "$work/reader.elf"
    done
    sha256sum -c media.sha
done
