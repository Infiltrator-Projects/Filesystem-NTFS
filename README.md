# Filesystem-NTFS
Independent filesystem implementation for InfiltratorOS, using the shared Common ABI.

## Installer files

- `/System/Filesystems/NTFS.so`
- `/System/Resources/Documentation/Filesystem-NTFS.pdf`
- `/System/Kernel/Filesystems/ntfs.ko`

The published Debian package installs these three regular files. A manual native RUN compiles the shared object and Linux adapter locally, generates the PDF, and replaces all three files. It uses the running kernel headers by default. Use `--kernel-build DIRECTORY` for a prepared kernel build with `Module.symvers`. The kernel module is stored for manual use.

InfiltratorOS calls `--system-package-mode` and decides which outputs need updating. Its worker builds a missing or older SO, repairs the PDF independently, and stores the matching released kernel module when available. Use `--build-only --output DIRECTORY` to build a Debian package.

## Common V2 runtime (1.0.11)

The installed NTFS.so exports `infiltrator_filesystem_api_v2` and no
legacy NTFS V1 entrypoint. Common V2 provides bounded host-I/O media
probing, read-only mount/unmount, and volume allocation query only when
the canonical MFT and bitmap validate. The existing NTFS engine and
recovery implementation remain source-owned but their legacy path/CLI
and fixture entrypoints are not exposed through the production V2 ABI.
File I/O, mutation, defragmentation, and recovery are not yet qualified
through Common V2 and must not be represented as available operations.
