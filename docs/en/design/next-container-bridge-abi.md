# Generic Container Native Bridge ABI v1

The container bridge is a separate runtime boundary from runtime ABI v1 and
v2. Its link manifest is `chtholly_next_container_v1.links`; adding this
boundary does not change Component ABI-1 or the existing runtime symbols.

## Typed vtable

Each concrete `HashMap<K, V>` or `HashSet<T>` specialization receives a
SemIR-generated `SemConcreteContainerVTable` descriptor and a target-side
`chtholly_next_container_v1_vtable`. The header records the
ABI version, semantic epoch, target pointer width, key/value sizes and
alignments, canonical type fingerprints, and a layout fingerprint. Callback
addresses are process-local and are not part of the persistent fingerprint.

Callbacks receive object addresses and an opaque context:

```text
hash(const void*, seed, context) -> u64
equal(const void*, const void*, context) -> bool
move(destination, source, context) -> void
drop(object, context) -> void
borrow_invalidate(context, old_generation, new_generation) -> void
```

Hash and equality are read-only. Move and drop are compiler-verified no-throw
operations. Allocation, capacity arithmetic, and rehash preparation are the
only fallible operations.

## Table state and invalidation

The table stores separate metadata, key, and value arrays with empty,
occupied, and deleted bucket states. A 64-bit generation is incremented by
reserve, structural insert, erase, clear, and rehash. The current Chtholly
reference representation remains a single pointer, so static SemIR ownership
provenance is authoritative; `get` and `get_mut` additionally validate the
table generation through `borrow_is_valid` in native lowering. Carrying the
generation inside every reference is intentionally deferred because it would
change the frozen reference ABI and LowIR object representation.

## Transactional rehash

Rehash allocates a complete candidate table before moving any source object.
All moves are no-throw, so a successful transfer can only commit. Allocation or
overflow failure leaves the old table untouched. After commit, old storage is
released without a second drop because ownership has transferred to the
candidate. Tombstones are discarded and the generation is invalidated once.

The C ABI is implemented in `runtime/src/Core/next_container_v1.c` and tested
by `chtholly_container_abi_tests`. Compiler intrinsic lowering consumes the
SemIR descriptor and fails closed for a missing descriptor. Fixed-width scalar
callbacks use the existing target-generated fast path. Verified custom nominal
callbacks are emitted as LLVM thunks that call concrete Hash/Equal witnesses
and the ordinary lifecycle lowering for move/drop. Nested nominal fields and
imported concrete witnesses are supported; source-level recursive-by-value
nominal definitions remain rejected by the recursive-type check and therefore
cannot fall back to byte copies.

The descriptor layout fingerprint includes concrete type fingerprints,
Hash/Equal witness fingerprints, container kind, and move/drop representation
facts. Function references are session-local lowering handles and callback
addresses are never persisted. A provider witness change therefore invalidates
consumer native output through the normal package/artifact closure.

The optional `rehash_begin`, `rehash_commit`, and `rehash_abort` vtable hooks
observe this transaction without owning table state. `rehash_begin` runs only
after candidate allocation and may reject the operation; `rehash_commit` runs
after the pointer swap and generation invalidation; `rehash_abort` runs when a
prepared candidate is discarded. The old runtime v1 link manifest is unchanged;
container symbols are published by the separate container link manifest.

## P1 reliability evidence

When `CHTHOLLY_RUNTIME_TESTING` is defined, container allocations route through
an internal deterministic wrapper. Tests can reset counters and fail the Nth
allocation, covering table creation, metadata/key/value arrays, and relocation
storage. The wrapper reports attempts, failures, and frees but is intentionally
excluded from the stable ABI and release link manifests.

The ABI test records key/value move and drop callbacks separately. A successful
rehash moves each occupied object into the candidate exactly once and drops no
source object; replacement, erase, clear, and destroy have explicit ownership
assertions. Every failed preparation path calls `rehash_abort` only after a
candidate has entered the transaction and leaves the old generation intact.

Stale references are checked both statically (SemIR provenance) and natively
(`borrow_is_valid(table, generation)`). Generation overflow is a terminal
status for a mutating operation and never wraps. The benchmark and fuzz targets
are test evidence and introduce no concurrent-write guarantee.
