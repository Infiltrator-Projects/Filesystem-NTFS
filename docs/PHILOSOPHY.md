# Engineering Philosophy

Filesystem-NTFS adopts the [Infiltrator Engineering Philosophy](https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/PHILOSOPHY.md) as its governing standard. These are engineering acceptance criteria, not a claim that all operations are implemented or qualified.

## Ownership and ABI

This independent provider owns its on-disk format semantics. Common owns the [V2 filesystem ABI and host services](https://github.com/Infiltrator-Projects/Libraries/blob/main/docs/FILESYSTEM_COMMON_ABI_V2.md); BOOT, Kernel and applications consume that contract. The production filesystem module must use Common V2 exclusively. Do not introduce FUSE, duplicate format policy in consumers, or advertise operations that have not been qualified. Hosted and freestanding execution profiles require independent validation.

## Integrity and failure

Validate media geometry, feature flags, offsets, lengths, arithmetic and allocation metadata before trusting them. Bound all I/O to the supplied volume. Document ownership, locking, on-disk invariants and unsupported feature combinations. Mutations require explicit preconditions, ordering, durability, interruption and recovery semantics; unsupported or unsafe operations must refuse without modifying media. Read-only operations must leave the image unchanged.

## Evidence and craftsmanship

Use representative real images and malformed fixtures, independent format references and failure-injection tests appropriate to each advertised operation. Qualify the exact packaged module and its ABI; distinguish probe, read, write, recovery, defragmentation and boot qualification. Preserve postgraduate-level reasoning in source comments and documentation. A passing compile or an ABI descriptor alone is not proof of data integrity or bootability.
