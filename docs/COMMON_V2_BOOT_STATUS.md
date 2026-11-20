# NTFS Common V2 BOOT qualification

The production NTFS.so exposes only Common V2. The hosted profile provides real
read-only CORE.PROBE, MOUNT/UNMOUNT, STAT, LIST and READ over partition-relative
bounded Common V2 block I/O. The NTFS engine interprets its own $MFT and
directory indexes; no Linux VFS or FUSE path reader is substituted. Real-media
and malformed-image regression tests exercise these operations and verify source
nonmutation. Named streams, reparse points, compressed/encrypted files and
unqualified write operations are not advertised as boot-safe.

Historical NTFS V1 workers, fixtures and callback bindings remain in source
solely for non-installed regression targets. The production shared object must
neither export nor link the V1 module or its service trampoline. Future Debian
and native RUN payloads must contain NTFS.so and its PDF, never a separate
filesystem-specific Linux .ko.

**NOT BOOT-QUALIFIED.** The current hosted NTFS implementation depends on
libntfs-3g and glibc. There is no qualified no-import freestanding implementation
embedded in .infiltrator_boot. The unmodified BOOT validator must continue to
reject this library. Do not publish a firmware-qualified release until an
independent source-owned freestanding build and OVMF kernel/initramfs reads
pass with valid and corrupted media and byte-for-byte nonmutation checks.
