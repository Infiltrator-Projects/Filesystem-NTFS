# NTFS migration — 2026-10-30

Source import only. Existing Defragmenter NTFS native analysis/relayout code copied **unchanged** from Defragmenter commit 30af892d8897d3aeaeefeac1bfcdb9620a0a5c72 to `migration/defragmenter/native/`. Canonical NTFS on-media design is copied from Filesystems commit ca2fa1210c3c1f2b6f52cd15b3da6dfa83d453ad.

The full NTFS3 Linux VFS implementation remains in Filesystems/native/filesystems/ntfs/linux. Worker sources rely on Defragmenter's platform and orchestration services, so this repository does **not** yet produce NTFS.so. Do not delete original sources or move consumers before reconciliation, a Common ABI adapter, independent media qualification and generic BOOT/kernel integration.
