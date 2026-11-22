# NTFS format engine

The formatter derives from NTFS-3G source revision
[78414d93613532fd82f3a82aba5d4a1c32898781](https://github.com/tuxera/ntfs-3g/tree/78414d93613532fd82f3a82aba5d4a1c32898781/ntfsprogs).
Original upstream copyright and licence notices remain applicable.

The adapted formatter is part of the single Common V2 `NTFS.so` provider.
Supporting NTFS helper source is pinned at that exact revision, embedded in the
source distribution and native `.run`, then compiled as position-independent
code into the filesystem module. Neither the external NTFS-3G shared library
nor an external formatter program is needed at runtime.

The Common V2 consumer owns the target partition and passes exclusive, bounded
read/write and flush callbacks. Formatting never launches a filesystem program,
invokes FUSE, or mounts an external filesystem. Separate regressions create and
verify genuine 512-, 1024-, 2048- and 4096-byte logical-sector NTFS volumes.
Tests that populate disposable images use a source-built C fixture helper,
not the external `ntfscp` program.
