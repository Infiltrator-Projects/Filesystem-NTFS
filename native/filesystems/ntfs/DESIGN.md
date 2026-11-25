# NTFS: Native Filesystem Implementation and Ownership

## Current production architecture

Hosted Common V2 declares probe, mount, stat, list, read, format, volume query and label retrieval; the native extent mapping interface is read-only. Format accepts 512, 1024, 2048 and 4096-byte logical sectors and up to 128 UTF-16 label code units. The formatter integrates adapted NTFS-3G source into the same SO, not a separately installed FUSE library. Formatting is serialised against mounted readers. Firmware reader qualification is distinct from production BOOT acceptance. The canonical entry point is `infiltrator_filesystem_api_v2`. Consumers must negotiate the actual profile and operation masks and use bounded, partition-relative host I/O. A source-level callback does not by itself establish that every format variant or host profile is qualified.

The canonical package installs one Common V2 shared object and its technical PDF. The native RUN downloads and verifies the exact source revision. No independent filesystem-specific kernel or EFI driver or FUSE substitute is distributed. See the [current provider README](../../../README.md) for current scope and the linked qualification evidence.

## Historical source and qualification record

> The following material preserves inherited design, original-format semantics, source attribution and earlier qualification evidence. Earlier implementation status, separate driver layouts, packaging instructions and ABI statements are historical, not the present release contract. No new test result is claimed by this record.

## Original source baseline

The actual complete original Linux NTFS3 implementation is available for
direct modification in `native/filesystems/ntfs/linux/`, restored from
the pinned Linux NTFS3 source previously held in this repository.
It preserves the original upstream GPL-2.0 notices, authorship, on-disk
semantics and all 28 source/build files including its decompression library.

## Linux native module

The original code now builds as an independent out-of-tree `ntfs3.ko` using
its existing Kbuild translation units and `obj-m` module registration.
`data/native-filesystems.tsv` maps the one NTFS catalogue identity to
the actual Linux VFS module named `ntfs3`.

The original module passed Debian 13 Module.symvers-backed linking and
the independent Linux 6.12 QEMU runtime workflow
(https://github.com/Infiltrator-Projects/Filesystems/actions/runs/39931921927).
The test booted Linux with no NTFS3 driver built into the kernel image,
loaded the project's copied module, wrote and renamed files on media
formatted by the independent mkfs.ntfs, unmounted/remounted and verified
persistent contents and a multicluster file, then cross-checked file
contents with ntfscat outside the Linux guest.

The existing Filesystems native module installer manages kernel ABI, signing,
native selection, source freshness, failure rollback and removal. The
NTFS catalogue no longer depends on NTFS-3G as its runtime mount provider.

## Qualification limitations

This qualification covers the exercised Linux NTFS3 features and media;
it does not guarantee full NTFS feature compatibility, corruption recovery,
every kernel version, removable-media failure safety, Windows interoperability
or EFI/root-system booting. Expansion requires independent tests of
journal replay, compression, sparse files, alternate streams, ACLs,
Unicode, malformed metadata and clean crash recovery.

The corresponding read-only UEFI `.efi` driver remains future work.
Changing source layout or attribution is never a substitute for these tests.
