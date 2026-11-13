# Filesystem-NTFS
Independent filesystem implementation for InfiltratorOS, using the shared Common ABI.

## Installer files

- `/System/Filesystems/NTFS.so`
- `/System/Resources/Documentation/Filesystem-NTFS.pdf`
- `/System/Kernel/Filesystems/ntfs.ko`

The published Debian package installs these three regular files. A manual native RUN compiles the shared object and Linux adapter locally, generates the PDF, and replaces all three files. It uses the running kernel headers by default. Use `--kernel-build DIRECTORY` for a prepared kernel build with `Module.symvers`. The kernel module is stored for manual use.

InfiltratorOS calls `--system-package-mode` and decides which outputs need updating. Its worker builds a missing or older SO, repairs the PDF independently, and stores the matching released kernel module when available. Use `--build-only --output DIRECTORY` to build a Debian package.

## Common V2

NTFS.so provides probing, read-only mount and verified MFT and bitmap volume
accounting. Its Common V2 formatter creates NTFS 3.1 data volumes using the
variant returned by its probe (`1`). It supports 512-byte logical sectors,
including 512e, and UTF-8 volume labels up to 128 UTF-16 code units. It creates
new filesystem structures and leaves unallocated sectors untouched.

Formatting runs inside NTFS.so and writes through the exclusive Common V2
host. The [format engine](formatter/README.md) uses libntfs-3g helpers and
requires no FUSE mount or formatter subprocess. Common V2 namespace operations,
relocation and recovery remain pending. Defragmenter still uses the dedicated
NTFS relocation and fixture interfaces.
