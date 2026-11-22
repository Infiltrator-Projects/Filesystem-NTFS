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
    cp --sparse=always --reflink=auto good.img dirty-volume.img
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
# Locate the genuine resident $VOLUME_INFORMATION value in MFT record 3.
# Alter only its dirty flag, on an independent copy of the clean image.
encoded=int.from_bytes(b[64:65], 'little', signed=True)
record_size=(1<<(-encoded)) if encoded<0 else encoded*cluster
assert record_size>=512 and record_size<=65536
volume_record=mft+3*record_size
with open('dirty-volume.img','r+b') as f:
    f.seek(volume_record)
    record=f.read(record_size)
    assert record[:4]==b'FILE'
    at=int.from_bytes(record[20:22],'little')
    used=int.from_bytes(record[24:28],'little')
    assert 48<=at<used<=record_size
    changed=False
    while at+24<=used:
        kind=int.from_bytes(record[at:at+4],'little')
        if kind==0xffffffff: break
        length=int.from_bytes(record[at+4:at+8],'little')
        assert length>=24 and length%8==0 and at+length<=used
        if kind==0x70:
            assert record[at+8]==0
            value_len=int.from_bytes(record[at+16:at+20],'little')
            value_at=int.from_bytes(record[at+20:at+22],'little')
            assert value_len>=12 and value_at+12<=length
            flag_off=volume_record+at+value_at+10
            flags=int.from_bytes(record[at+value_at+10:at+value_at+12],'little')
            f.seek(flag_off)
            f.write((flags|1).to_bytes(2,'little'))
            changed=True
            break
        at+=length
    assert changed, "Required $VOLUME_INFORMATION not found"
MEDIA
    sha256sum good.img bad-mft.img bad-geometry.img dirty-volume.img > media.sha
    for host_sector in 512 4096; do
        for profile in "$module" "$work/reader.elf"; do
            "$probe" "$profile" good.img "$host_sector" clean
            "$probe" "$profile" bad-geometry.img "$host_sector" corrupt
        done
        "$hosted" good.img source --read "$host_sector"
        "$hosted" bad-mft.img source --bad-mft "$host_sector"
        "$hosted" good.img source --read "$host_sector" "$work/reader.elf"
        "$hosted" bad-mft.img source --bad-mft "$host_sector" "$work/reader.elf"
        "$hosted" dirty-volume.img source --dirty-volume "$host_sector" "$work/reader.elf"
    done
    sha256sum -c media.sha
done
