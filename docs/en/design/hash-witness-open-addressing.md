# Primitive Hash Witness and Open Addressing Design

The standard library's `std::hash::Hash` and `Equal` protocols are compiler-
owned for scalar keys. Fixed-width integers, bool, char, floating values, and
their `usize`/`isize` representations use canonical semantic identities; user
code cannot
declare a competing primitive conformance outside `std::hash`. Witness and
public-interface ordering is canonicalized before fingerprinting, so overload
order cannot change an artifact digest.

`std::hash::hash_value` and `equal_value` are ordinary generic Chtholly calls.
Float witnesses compare and hash deterministic IEEE bit patterns, so NaNs and
signed zero are stable across targets. The primitive implementations
use scalar bits and the instance seed. The `.cns` mix function uses the
compiler-owned `wrapping_mul` operation; constant evaluation and LLVM lowering
both retain the low 64 bits.
Pointer witnesses, when enabled for an exact raw-pointer type, hash the target
data-layout pointer bits and compare address identity only; pointee contents are
never read and const qualification does not alter the address value. Pointer
width is part of the target identity; address spaces remain an ABI-specific
extension until they are represented in the source type model.

The `std::collections` surface reserves an open-addressing representation with
empty/occupied/deleted metadata, quadratic probing, cached hash, per-instance
seed, and Option payloads. Mutating operations must invalidate borrowed views;
rehash is required to commit only after a complete candidate table is built.
The reusable native `OpenAddressingHashTable` test utility exercises these
invariants while source-level generic storage is closed in the next wave.

Runtime ABI v2 adds `chtholly_next_runtime_v2_allocate`, which reports invalid
alignment and out-of-memory without modifying caller-owned storage. Runtime
ABI v1 and Component ABI-1 are unchanged.
