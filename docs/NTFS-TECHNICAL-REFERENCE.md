# NTFS: technical reference

## History, revision and design

NTFS arrived with Windows NT 3.1 in 1993. NTFS 3.0 and 3.1 were introduced with Windows 2000 and Windows XP respectively; operating-system release numbers are not filesystem revision numbers. This reference uses the Linux v6.12 ntfs3 format definitions as implementation evidence and Microsoft documentation for filesystem semantics. An NTFS volume is an attribute-based record store: the Master File Table maps record identities, and attributes may store bytes within records or in separate cluster runs. Directory indexes, metadata allocation and transaction recovery must be interpreted together. A reader that extracts the unnamed data stream is not necessarily a full NTFS implementation.

NTFS was designed for a security-oriented multiuser operating system, with a cost in metadata complexity relative to FAT. Later Windows features such as deduplication and reparse services may depend on additional OS semantics.

## Boot sector and address units

The boot record contains OEM identifier NTFS followed by four spaces at offset 3, little-endian sector bytes at 11, sectors-per-cluster encoding at 13, media byte 21, track/head geometry 24/26, hidden-sector field 28. Total volume sectors is 64-bit at 40, MFT starting LCN at 48, MFT mirror LCN at 56. Signed record-size and index-size encodings at 64 and 68, serial 72, checksum field 80, boot signature 55 AA at 510. For ordinary positive sectors-per-cluster values, cluster bytes C equal sector bytes times sectors per cluster. Large-cluster encodings need native validation. A positive signed record-size value is a cluster count; a negative value -k specifies 2^k bytes. For C=4096 and record-size -10, a FILE record is 1024 bytes. Validate all multiplications and the backup boot sector against the actual device.

## Master File Table bootstrap

The boot record locates the beginning of $MFT, not every MFT record. Read its initial FILE record, apply update-sequence protection, then resolve the unnamed nonresident $DATA mapping of record zero. For record number r and record size R, the byte offset in the MFT stream is r*R, which must be translated through the MFT's own runlist. The mirror is limited to initial critical records, not a full backup of all files. Reserved records include 0 $MFT, 1 $MFTMirr, 2 $LogFile, 3 $Volume, 4 $AttrDef, 5 root, 6 $Bitmap, 7 $Boot, 8 $BadClus, 9 $Secure, 10 $UpCase and 11 $Extend for the modern layout. Use validated record identities and revision-specific rules when interpreting the reserved namespace.

## Multi-sector record protection

FILE, INDX and log record families use a common protected header: four-byte signature at 0, 16-bit update-sequence array offset at 4, array word count at 6, 64-bit log sequence at 8. The first array word is the update-sequence marker; subsequent words preserve original trailing words from protected sectors. The pinned ntfs3 implementation uses 512-byte protection intervals, including on larger logical-sector media. For a 1024-byte FILE record, the ordinary count is 3; verify marker at bytes 510 and 1022, then replace those words with array entries 1 and 2 in the working copy. Validate alignment, bounds, count and every marker before accepting metadata. A failed fixup is corruption evidence, not a request to repair the disk. This mechanism detects certain torn writes but is not a general data checksum.

## FILE record fields and identity

After the common header, sequence number at 16, link count 18, first attribute offset 20, flags 22, bytes in use 24, allocated record bytes 28, base-record reference 32, next attribute ID 40, reserved field 42 and modern record number field 44. Flags bit 0 means in use, bit 1 directory. An eight-byte reference has low 32 bits of record number at 0, next 16 bits at 4 and sequence at 6. For NTFS 3.x a nonzero base reference identifies an extension record; it is not the file's containing directory. Compare references against current sequence and allocation state. A stale reference to a reused record is not valid ownership. Enforce attribute offset, record used size, and extension-to-base coherence.

## Typed attribute records

Each attribute begins with 32-bit type 0, total record length 4, resident/nonresident byte 8, name length in UTF-16 units 9, name offset 10, flags 12 and record-local ID 14. Types include 0x10 standard information, 0x20 attribute list, 0x30 file name, 0x40 object ID, 0x50 security descriptor, 0x60 volume name, 0x70 volume information, 0x80 data, 0x90 index root, 0xA0 index allocation, 0xB0 bitmap, 0xC0 reparse point, 0xD0 EA information and 0xE0 EA. Type 0xFFFFFFFF terminates the list. For resident attributes value length at 16, value offset 20 and indexed flag 22; minimum common resident header is 24 bytes. Validate name and value bounds separately, attribute progress and eight-byte alignment. Several named DATA streams can coexist; type alone is not stream identity.

## Nonresident attribute headers

The nonresident header has start VCN at 16, inclusive last VCN at 24, mapping-pairs offset 32, compression-unit exponent 34, allocated bytes 40, logical data bytes 48 and initialized bytes 56. A further allocated-physical-size field at 64 is present for applicable compressed/sparse first segments. Do not require it for ordinary 64-byte headers or extension segments. VCN denotes a virtual cluster relative to an attribute; LCN denotes a volume cluster. Enforce supported attribute flags and monotone VCN coverage. Size information in the first segment describes the stream; a later segment is not a separate file. Distinguish initialized length from EOF: bytes beyond initialized length but within EOF read as zeros. Sparse and compressed physical occupancy must not be inferred from logical allocation alone.

## Mapping pairs: signed delta example

A mapping-pair header byte gives length-field width in its low nibble and signed LCN-delta width in its high nibble. Zero terminates the list. Decode the unsigned run length and signed little-endian delta with checked sign extension. A zero delta-width denotes a sparse run; it does not change the prior physical LCN. Example byte sequence in hex: 11 03 28 11 02 FB 01 04 00. Starting at VCN zero, first three clusters map to LCN 40..42; next two to LCN 35..36 because FB represents -5; next four are sparse. With 4096-byte clusters, logical VCN 4 maps to LCN 36 at byte 147456, whereas VCN 6 reads as zeros. Check widths, nonzero lengths, cumulative VCN end and physical volume boundaries. Decode each attribute segment with its native starting delta state.

## Attribute lists and extension records

An attribute-list entry contains type 0, 16-bit entry length 4, name length 6, name offset 7, starting VCN 8, eight-byte file reference 16, attribute ID 24 and variable name at or after 26. It can refer to attributes in the base or extension FILE records. Follow the list's own resident/nonresident representation; match type, name, record sequence, ID and VCN coverage. Reject cycles, duplicate or overlapping segments and inconsistent base references. Bootstrapping a fragmented MFT may require progressive resolution of its own attribute list rather than assuming arbitrary record addresses can be read immediately. A partial record list is not a complete file mapping.

## Names, links and directory indexes

The FILE_NAME value begins with parent reference at 0, four 64-bit timestamps at 8,16,24,32, allocation and data sizes at 40/48, attributes at 56, EA/reparse information at 60, name length at 64, namespace at 65 and UTF-16LE name from 66. Namespaces are POSIX 0, Windows 1, DOS 2 and combined Windows/DOS 3. Multiple names and links can identify the same base record. Do not count a DOS alias as an independent file. Directory $I30 indexes use $INDEX_ROOT, optional $INDEX_ALLOCATION and corresponding $BITMAP. Use the volume's $UpCase table and native collation, not host locale or an assumed Unicode case-folding algorithm.

## Index root and INDX buffers

INDEX_ROOT value fields are indexed attribute type 0, collation rule 4, index block bytes 8, cluster/sector encoding 12 and index header at 16. An INDX buffer has the protected common header, logical index address at 16 and index header at 24. Index header fields are first-entry offset 0, used bytes 4, allocated bytes 8 and flags 12, relative to that header. Each index entry has an eight-byte reference or index-specific data locator, entry bytes at 8, key bytes 10, flags 12 and reserved 14. Flag 1 denotes a child pointer in the final eight bytes; flag 2 marks the final entry. Validate node bounds, child addresses and index bitmap, collation order and record sequences. The index address uses native index geometry; do not always multiply it by cluster bytes.

## Allocation and special data forms

$Bitmap tracks volume clusters and $MFT's own bitmap tracks record allocation. Set bits identify allocated items; bit order is least-significant first. For cluster 33000, byte 4125 bit 0 holds its allocation state. Resident data has no independent data cluster run; compressed data is organized into compression units with native encoded/sparse patterns; LZNT1 decoding is not interchangeable with WOF compression or EFS encryption. $BadClus reserves damaged cluster ranges. MFT reservation zones are allocator policy, not evidence that every cluster in the zone is allocated. A physical ownership scan must include every nonresident attribute stream and metadata file; an ordinary file-data extent list is insufficient for full volume defragmentation.

## Security, reparse and stream semantics

Modern NTFS centralizes security descriptors in $Secure, with descriptor storage and indexes; standard-information security identifiers refer to that metadata. Reparse attributes carry a tag and bounded data with tag-specific semantics, including some symbolic links, mount points and WOF-backed data. Preserve unknown reparse tags, named streams, object IDs, EA values and access controls during operations. Distinguish a native NTFS sparse attribute from higher-level deduplication or external backing. Windows timestamps count 100-nanosecond intervals since 1601-01-01 UTC. Treat duplicated FILE_NAME timestamps and sizes as index metadata, not universally authoritative current stream state. Cross-platform conversion cannot safely replace native security or reparse behavior with ordinary POSIX permission bits.

## Transaction recovery and durability

$LogFile uses restart pages (RSTR), record pages (RCRD), log sequence numbers and transaction records. Recovery establishes a coherent restart state, reconstructs committed operations and performs required redo/undo under revision-specific log semantics. The $UsnJrnl change journal is not the metadata transaction log and cannot substitute for replay. A clean-looking directory or valid fixup is insufficient evidence of recovery. The volume dirty flag and Windows hibernation or Fast Startup state require explicit handling. A writer must preserve MFT, allocation, attribute lists, indexes and log ordering; copy and verify relocation data before committing new mappings, and retire original ownership only after durability is proven. Exact native log operation codes, restart tables and replay semantics require further source-level and crash-fixture work.

## Implementation and qualification boundary

At historical source revision f87572403218 the hosted provider declared read-only extent mapping and native NTFS 3.1 data-volume format (adapted NTFS-3G source inside the same SO). Its documented profile accepted 512, 1024, 2048 and 4096-byte logical sectors, up to 128 UTF-16 label units, and update-sequence fixups on 512-byte subrecords even on genuine 4Kn media. Compressed, encrypted and reparse files were outside that physical-map contract. Formatting was serialized against readers; unallocated sectors were not erased. These are source-baseline facts, not a claim of current qualification.

Common V2 is the independent provider interface: https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md . Negotiate real operation masks and media profiles. Use bounded host I/O, exclusive unmounted ownership and durable flush for mutation; distinguish unsupported, corrupt, I/O and stale-handle results. Test Media and Defragmenter use the same engine. Hosted, freestanding, BOOT, kernel, recovery and package qualification are independent. Current capability and release status belongs to the repository README and exact-source qualification records; historical evidence is not a current status claim.

Historical source or test reference: https://github.com/Infiltrator-Projects/Filesystem-NTFS/actions/runs/45526878687 .

## Formatting and validation

A formatter must publish coherent primary and backup boot geometry, MFT and mirror, reserved system records, root index, allocation bitmaps, attribute definitions, log initialization, uppercase mapping and security metadata. Verify independently formatted Windows and NTFS-3G fixtures at admitted sector and cluster sizes, including 4Kn, fragmented MFTs, multiple streams, extension records, sparse runs, compression, hard links, non-ASCII names, index splits, stale sequences, fixup mismatches and interrupted mutations. Use native chkdsk and read-only reference comparisons on disposable copies; no external production formatter or filesystem driver fallback is implied. Sources: https://github.com/torvalds/linux/tree/v6.12/fs/ntfs3 (ntfs.h, run.c, fsntfs.c, record.c, index.c, fslog.c); https://learn.microsoft.com/en-us/windows/win32/fileio/master-file-table . 

The PDF preserves the earlier manual as a historical appendix.
