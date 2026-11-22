# NTFS Common V2 native hosted and freestanding reader

## Runtime architecture (source revision after 1.0.20)

The one installed `NTFS.so` exports only
`infiltrator_filesystem_api_v2`. Hosted and firmware profiles compile
**the same** `native/ntfs_ro_v2.c` bounded native NTFS decoder. Both
profiles implement CORE PROBE, read-only MOUNT/UNMOUNT, STAT, LIST and
READ with real NTFS MFT records, USA fixups, INDEX_ROOT and
INDEX_ALLOCATION traversal, sector-safe host reads, allocation runlists,
clean-volume validation and invalid-handle rejection. Each profile owns
its own stable Common V2 session registry; both call the same on-media
decoder, not separately reimplemented path readers.

Hosted DEFRAG.MAP now calls `ntfs_ro_map_v2` on the *mounted native
reader*, and parses exactly the same MFT data runs as CORE.READ. Runlist
mapping validates the entire mapping-pairs chain before emitting physical
extents and supports bounded pagination and sparse-run omission. This
is a read-only filesystem allocation map, **not** a V2 defragmentation
mutation/recovery capability. Hosted VOLUME.FORMAT remains available;
it is distinct from read-only mounting and does not run in firmware.

The old `module/ntfs_v2_map.c` NTFS-3G namespace and extent-map bridge
has been removed from the installed target and the repository. There is
no Linux VFS, FUSE, NTFS-3G namespace reader, native `.ko` fallback or
retired filesystem ABI in the production CORE/MAP read path.
Freestanding reader is built with `-ffreestanding` and no imported
dynamic libraries; its exact ELF is embedded into the single installed
carrier in `.infiltrator_boot`.

## Exact executable qualification

Source and Common V2 interface validity alone are not a working boot
test. The repository tests the actual linked and packaged hosted and
freestanding providers on independently `mkntfs`-formatted, copied
512-byte and 4096-byte logical-sector disposable disk images.
Tests compare the full real Linux kernel and generated initramfs against
source bytes; check real root listing, NTFS path lookup, partial reads,
volume totals, stale handles, metadata corruption, the MFT fixup, and
clean versus dirty journal rejection; and confirm unchanged image SHA.
Hosted DEFRAG.MAP separately checks real physical extent bounds,
fragment ordering and next-page/missing-file results.

The `tests/test_ntfs_duplicate_attribute.c` test uses the exact
native reader source and rejects duplicate `$DATA` attributes, an
unterminated FILE chain and malformed subsequent attributes even when
the first matching record is valid. The source and V2 module need to
pass **new current-main** CI before any updated .deb/.run release can
be called qualified.

## Not yet a production BOOT integration

BOOT itself does not currently consume Common V2, so neither the
module nor its embedded firmware profile can be described as
currently BOOT-compatible. No current production BOOT V2 validator
or OVMF kernel handoff has accepted this exact module. The
freestanding reader supports a conservative NTFS feature subset,
and refuses dirty metadata, attribute-list extensions, unqualified
encryption/compression and reparse traversal. These exclusions are
intentional; do not replace the on-media decoder, weaken a validator
or invent a success.

Historical worker/transaction regression code is test-only and is
not linked or published as a V1 runtime. Actual V2 transactional
defragmentation and durable recovery are still unavailable.
