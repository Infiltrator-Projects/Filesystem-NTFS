# Filesystem-NTFS
Independent filesystem implementation for InfiltratorOS, using the shared Common ABI.

## Installer files

- `/System/Filesystems/NTFS.so`
- `/System/Resources/Documentation/Filesystem-NTFS.pdf`

The Debian and native installer candidates stage the Common V2 module and its manual. Release 1.0.18 also shipped a separate Linux module. New publication is gated on the firmware reader and qualification.

InfiltratorOS calls `--system-package-mode` to build and install the native
shared object and PDF. Use `--build-only --output DIRECTORY` to build a
Debian package.

## Common V2

NTFS.so provides probing, read-only mount and verified MFT and bitmap volume
accounting. Its Common V2 formatter creates NTFS 3.1 data volumes using the
variant returned by its probe (`1`). It supports both 512-byte logical sectors (including 512e) and genuine
4096-byte logical sectors (4Kn), with UTF-8 volume labels up to 128 UTF-16
code units. It creates
new filesystem structures and leaves unallocated sectors untouched.

Formatting runs inside NTFS.so and writes through the exclusive Common V2
host. The [format engine](formatter/README.md) uses libntfs-3g helpers and
requires no FUSE mount or formatter subprocess. Its separate 512-byte and 4Kn
regressions create their volume images through the production Common V2 format
entrypoint; no external mkntfs command is used. External NTFS-3G utilities
remain in distinct cross-check and file-population tests. The native build extracts its
NTFS development and test files into temporary storage and installs only the
FUSE-free NTFS runtime library. Common V2 also exposes read-only physical
extent mapping for regular files. Sparse regions are gaps in the logical
offsets. Resident data has no separate physical data extent. Compressed,
encrypted and reparse-point files are outside the map contract. General
namespace access supports hosted read-only stat, directory enumeration and file reads. Relocation and recovery through Common V2 remain pending.

The hosted and embedded freestanding profiles share the on-disk NTFS boot geometry probe. Read-only hosts can use 512-byte or 4 KiB transfer sectors independently of the formatted NTFS sector size. Native MFT torn-write protection validates the on-disk update-sequence count:
512-byte subrecords remain valid inside genuine 4Kn volumes. The freestanding
firmware reader supports validated read-only probe, mount, stat, list and read
operations; physical BOOT/Common V2 integration has not yet been qualified.
