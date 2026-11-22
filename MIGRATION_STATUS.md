# NTFS Common V2 source consolidation — 22 November 2026

The production independent `/System/Filesystems/NTFS.so` exports the
canonical `infiltrator_filesystem_api_v2` and is the sole installed
filesystem module. The native hosted CORE and freestanding CORE are both
compiled from `native/ntfs_ro_v2.c`, and both use the source-owned,
read-only NTFS decoder; no separate NTFS-3G pathname or file-reader path
is permitted. Hosted DEFRAG.MAP now uses that same native MFT runlist
parser, rather than `module/ntfs_v2_map.c`. This retired, duplicate
decoder has been deleted. Hosted VOLUME.FORMAT remains the existing
source-owned formatter and is not advertised by the firmware reader.

The historical NTFS V1 adapters, workers, Linux NTFS3 source and
transactional defragmentation fixtures are not part of the installed
shared object. A checkout may retain historical source and test
fixtures; **they are not runtime ABI exports, installed kernel drivers
or fallbacks**. No `.ko` is distributed by the Common V2 package.
Common V2 DEFRAG.MAP provides real, bounded, read-only extent mapping,
not PLAN/EXECUTE/VERIFY/RECOVER; those write/recovery capabilities
remain unavailable until the format's native transactional engine is
migrated and interrupted writes are qualified.

The Common V2 formatter now creates genuine 512-byte and 4096-byte
logical-sector NTFS volumes through the same internal NTFS.so format entrypoint.
The independent native 4Kn regression verifies aligned 4096-byte host writes,
boot and backup sectors, native probing and mounting. The media suite exercises
both formatted sector geometries and rejects MFT/update-sequence corruption.
The metadata reader honours the validated NTFS USA protected-unit count,
including 512-byte MST subrecords within a 4096-byte logical sector. It does
not substitute 512-byte media for the separate 4Kn coverage. The external
mkntfs program is not used by these regressions; unrelated file-population
and cross-validation tests still use external NTFS-3G utilities.

The repository's disposable real-media suite compares complete kernel
and initramfs file bytes through hosted and extracted freestanding
profiles on both 512 B and 4 KiB sector media; checks allocation map
bounds, paging, corrupt MFT records, dirty journal rejection, stale
handles and nonmutation; and independently tests duplicate FILE
attributes and truncated runlist denial.

The **version in the source tree is 1.0.22**, following the previously
published 1.0.21. This source is not a verified release until the
exact-main GitHub Actions build, portable Debian package, aggressive
native RUN build and packaged-media tests all succeed and publish a
new immutable release. Neither profile is currently accepted by a
production BOOT Common V2 validator, because BOOT itself is not V2.
No BOOT source, ESP, firmware state or physical disks were altered.
