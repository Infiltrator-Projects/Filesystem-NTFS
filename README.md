# Filesystem-NTFS
Independent filesystem implementation for InfiltratorOS, using the shared Common ABI.

## Installer files

- `/System/Filesystems/NTFS.so`
- `/System/Resources/Documentation/Filesystem-NTFS.pdf`
- `/System/Kernel/Filesystems/ntfs.ko`

The published Debian package installs these three regular files. A manual native RUN compiles the shared object and Linux adapter locally, generates the PDF, and replaces all three files. It uses the running kernel headers by default. Use `--kernel-build DIRECTORY` for a prepared kernel build with `Module.symvers`. The kernel module is stored for manual use.

InfiltratorOS calls `--system-package-mode` to build and install the native
shared object and PDF. Use `--build-only --output DIRECTORY` to build a
Debian package.

## Common V2

NTFS.so provides probing, read-only mount and verified MFT and bitmap volume
accounting. Its Common V2 formatter creates NTFS 3.1 data volumes using the
variant returned by its probe (`1`). It supports 512-byte logical sectors,
including 512e, and UTF-8 volume labels up to 128 UTF-16 code units. It creates
new filesystem structures and leaves unallocated sectors untouched.

Formatting runs inside NTFS.so and writes through the exclusive Common V2
host. The [format engine](formatter/README.md) uses libntfs-3g helpers and
requires no FUSE mount or formatter subprocess. The native build extracts its
NTFS development and test files into temporary storage and installs only the
FUSE-free NTFS runtime library. Common V2 also exposes read-only physical
extent mapping for regular files. Sparse regions are gaps in the logical
offsets. Resident data has no separate physical data extent. Compressed,
encrypted and reparse-point files are outside the map contract. General
namespace access, relocation and recovery through Common V2 remain pending.
