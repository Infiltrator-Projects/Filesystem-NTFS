# NTFS format engine

The formatter derives from the NTFS-3G source at
[78414d93613532fd82f3a82aba5d4a1c32898781](https://github.com/tuxera/ntfs-3g/tree/78414d93613532fd82f3a82aba5d4a1c32898781/ntfsprogs).
The upstream authors and licence are included here. The engine runs inside
NTFS.so and links the distribution's libntfs-3g for NTFS structure helpers.
The Common V2 consumer owns device acquisition and passes bounded I/O and flush
callbacks. Formatting does not invoke a command or mount a filesystem.
