# NTFS source consolidation — 6 November 2026

The independent NTFS module repository now contains all 29 canonical source/design files under `native/filesystems/ntfs/` from central Filesystems main `a253fb724ce1fd760a361219281effb6abd71367`. Its existing matching design file was preserved. The complete original Linux NTFS3 VFS code was imported into its local `native/filesystems/ntfs/linux/` tree, preserving its separate code authorship and licenses. The existing Defragmenter-derived native NTFS worker and standalone module remain intact and continue to be built by this repository.

`standalone/kernel.json` builds `ntfs3.ko` from this repository's source, without retrieving a pinned older Filesystems source archive. The independent `.so` is still subject to its ABI and real-media qualification tests; copying the kernel driver does not itself mean the shared object implements full NTFS read/write.

Central source deletion requires an independent kernel build and runtime test plus detachment of the central test/package consumers. The Linux source must not be forked again in Filesystems after that cutover.

NTFS metadata-plan application can now use an existing exclusive, durable
Common V2 storage lease without reopening the target by pathname. Its
disposable-image tests cover rejection before writes and failed durability.
This primitive alone does not qualify V2 relocation or crash recovery; those
operations remain unavailable through the Common V2 defragmentation group
until their complete transaction and recovery paths have been migrated.
Workspace relocation also rejects out-of-bounds cluster addresses, incomplete
database reads and empty relocation sets; disposable-image tests exercise
staging, verification, placement, recovery and these failure cases.
