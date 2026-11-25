# Filesystem-NTFS

## 1. Purpose and format scope

NTFS, including native NTFS 3.1 data-volume formatting. This repository owns the format-specific implementation of `NTFS.so` for InfiltratorOS.

## 2. On-disk architecture and invariants

Boot geometry, master file table, allocation bitmap, attributes and runlists define file and allocation ownership. Update-sequence fixups validate torn-write protection, including 512-byte subrecords within genuine 4Kn media. Resident data has no separate physical extent; sparse mappings contain logical gaps.

## 3. Common V2 implementation and capabilities

Hosted Common V2 declares probe, mount, stat, list, read, format, volume query and label retrieval; the native extent mapping interface is read-only. Format accepts 512, 1024, 2048 and 4096-byte logical sectors and up to 128 UTF-16 label code units. The formatter integrates adapted NTFS-3G source into the same SO, not a separately installed FUSE library. Formatting is serialised against mounted readers. Firmware reader qualification is distinct from production BOOT acceptance. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

## 4. Integrity, mutation, allocation and recovery

Compressed, encrypted and reparse-point files are outside the documented physical-map contract. Formatting leaves unallocated sectors untouched; full writable namespace, relocation, recovery and physical-media crash safety are separate qualifications. For destructive operations, require the applicable exclusive writable host, durability contract and format-specific validation. Never silently substitute a kernel or FUSE driver or treat a rejected or unavailable operation as success. Preserve original disk-format semantics; relocation and recovery are separate from ordinary file I/O.

## 5. Build, installation and release

The target installation is `/System/Filesystems/NTFS.so` and `/System/Resources/Documentation/Filesystem-NTFS.pdf`. The project release policy is a portable amd64 DEB and a CPU-native source-first RUN, with GitHub's automatic source archive; do not treat this policy statement as verification of the current installer payload. The release workflow is the authoritative executable build and package specification. Historical adapters and test artifacts are not additional production filesystem authorities.

## 6. Qualification and limitations

[Recorded qualification workflow](https://github.com/Infiltrator-Projects/Filesystem-NTFS/actions/runs/45526878687) · [Current workflows](https://github.com/Infiltrator-Projects/Filesystem-NTFS/actions?query=branch%3Amain). A successful workflow establishes the tested scope only, not complete original-format parity, physical-media crash safety, or production BOOT/kernel integration. Use disposable images for destructive qualification.

## 7. Technical references and provenance

- [Provider implementation](module/ntfs_v2_probe.c)
- [Native design or source provenance](native/filesystems/ntfs/DESIGN.md)
- [Migration and source history](MIGRATION_STATUS.md)
- [Original-format reference source](docs/NTFS.pages) and [PDF](docs/NTFS.pdf)
- [Project philosophy](docs/PHILOSOPHY.md)
- [Common ABI](https://github.com/Infiltrator-Projects/Libraries)

The original-format reference and historical migration notes describe different evidence from the current production provider. When descriptions disagree, the current exported descriptor, implementation, executable tests and exact-revision release evidence take precedence.
