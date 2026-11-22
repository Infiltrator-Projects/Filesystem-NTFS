# Filesystem-NTFS
Independent filesystem implementation for InfiltratorOS, using the shared Common ABI.

## Installer files

- `/System/Filesystems/NTFS.so`
- `/System/Resources/Documentation/Filesystem-NTFS.pdf`

The Debian package and source-embedded native installer deliver the Common V2 module and its manual. Only a single `NTFS.so` filesystem provider is shipped; no Linux kernel filesystem module or separate FUSE provider is supplied.

InfiltratorOS calls `--system-package-mode` to build and install the native
shared object and PDF. Use `--build-only --output DIRECTORY` to build a
Debian package.

## Common V2

NTFS.so provides probing, read-only mount and verified MFT and bitmap volume
accounting. Its Common V2 formatter creates NTFS 3.1 data volumes using the
variant returned by its probe (`1`). It accepts genuine 512-, 1024-, 2048- and 4096-byte logical sectors (including 512e and 4Kn), validated by separate formatting, probing and mount tests. UTF-8 volume labels contain up to 128 UTF-16 code units. Formatting creates new filesystem structures and leaves unallocated sectors untouched.

Formatting runs inside NTFS.so and writes through the exclusive Common V2
host. The [format engine](formatter/README.md) integrates adapted NTFS-3G helper source at the pinned upstream revision `78414d93613532fd82f3a82aba5d4a1c32898781`, compiled as position-independent objects into `NTFS.so`. The module does not link to a separately installed NTFS-3G or FUSE shared library. Its native source-embedded `.run` carries the required formatter source. Real-media tests create images through Common V2 and seed files using a C test fixture linked to this same embedded source; no `mkntfs`, `ntfscp`, `ntfslabel`, `ntfsfix` or `ntfsresize` executable is invoked. Original code provenance and attribution remain applicable. Common V2 also exposes read-only physical
extent mapping for regular files. Sparse regions are gaps in the logical
offsets. Resident data has no separate physical data extent. Compressed,
encrypted and reparse-point files are outside the map contract. General
namespace access supports hosted read-only stat, directory enumeration and file reads. Relocation and recovery through Common V2 remain pending.

The hosted and embedded freestanding profiles share the on-disk NTFS boot geometry probe. Read-only hosts can use 512-byte or 4 KiB transfer sectors independently of the formatted NTFS sector size. Native MFT torn-write protection validates the on-disk update-sequence count:
512-byte subrecords remain valid inside genuine 4Kn volumes. The freestanding
firmware reader supports validated read-only probe, mount, stat, list and read
operations; physical BOOT/Common V2 integration has not yet been qualified.
