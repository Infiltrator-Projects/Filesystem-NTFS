# Filesystem-NTFS
Independent filesystem implementation for InfiltratorOS, using the shared Common ABI.

## Installer files

- `/System/Filesystems/NTFS.so`
- `/System/Resources/Documentation/Filesystem-NTFS.pdf`
- `/System/Kernel/Filesystems/ntfs.ko`

The published Debian package installs these three regular files. A manual native RUN compiles the shared object and Linux adapter locally, generates the PDF, and replaces all three files. It uses the running kernel headers by default. Use `--kernel-build DIRECTORY` for a prepared kernel build with `Module.symvers`. The kernel module is stored for manual use.

InfiltratorOS calls `--system-package-mode` and decides which outputs need updating. Its worker builds a missing or older SO, repairs the PDF independently, and stores the matching released kernel module when available. Use `--build-only --output DIRECTORY` to build a Debian package.

## Common V2 migration (1.0.12)

NTFS.so exports canonical Common V2 bounded-host probing, read-only
mount/unmount and verified MFT/bitmap volume accounting. The existing V1
worker/fixture entrypoint remains exported temporarily because the
Defragmenter NTFS loader still uses it. Removing V1 before its consumer
is migrated would silently break existing analysis and relayout services.
Only validated V2 groups are advertised; V2 file access, mutation, defrag
and recovery require a separately qualified consumer cutover.
