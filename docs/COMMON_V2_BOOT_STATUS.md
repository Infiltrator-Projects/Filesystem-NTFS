# NTFS Common V2 hosted and freestanding qualification

## Production boundary

NTFS.so exports only `infiltrator_filesystem_api_v2()`. Its hosted module
provides CORE PROBE/MOUNT/STAT/LIST/READ over Common V2 partition-relative
reads using the existing ntfs-3g MFT and index engine. The embedded
`.infiltrator_boot` ELF now builds the independent, freestanding
`native/ntfs_ro_v2.c` C reader, not a probe-only descriptor. That reader uses
Common V2 host I/O and allocation exclusively, without glibc, NTFS-3G, Linux
VFS, FUSE, runtime filesystem-specific .ko or Python.

Freestanding CORE implements PROBE, read-only MOUNT/UNMOUNT, STAT, LIST and
READ; VOLUME implements QUERY. The reader checks NTFS boot geometry, MFT
file-record USA fixups, attribute boundaries, generation numbers, index entries,
resident and nonresident streams, allocation runlists and the $Bitmap.
It refuses dirty volumes, malformed attributes, unsupported encryption or
compression, unqualified attribute-list extensions and reparse traversal.

## Tested contract

CI builds both firmware and hosted profiles and extracts the **actual**
firmware ELF from both the generic Debian and CPU-native RUN packaged modules.
Disposable independently formatted NTFS images exercise 512-byte and 4096-byte
sectors/host geometry, directory traversal, kernel and initramfs exact-byte
reads, file-not-found, invalid paths, stale mount handles, corrupt geometry,
MFT fixup corruption and dirty-volume exclusion. SHA-256 confirms the source
images are unchanged.

Historical V1 workers/fixtures are retained in test-only targets and must not
be linked or exported in installed NTFS.so. The release contains NTFS.so and
its manual, never a Linux kernel driver substitute.

## Unqualified integration and supported subset

**Not yet certified for use by the future Common V2 BOOT consumer.**
The freestanding payload is an import-free ELF intended for authenticated
loading by that consumer, but an actual BOOT V2 validator, OVMF startup and
kernel/initramfs handoff through BOOT remain untested. The portable firmware
reader implements a conservative NTFS subset; unsupported metadata must
fail closed. The hosted path still uses ntfs-3g while the freestanding profile
uses the native C reader; **sharing the complete native implementation across
both profiles remains an architectural convergence task**. Do not imply a
completed BOOT integration or identical execution profiles from green CI.
