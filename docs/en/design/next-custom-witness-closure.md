# Custom Container Witness Closure

Status: implemented and verified for the current container bridge.

## Boundary

`HashMap<K, V>` and `HashSet<T>` use the independent
`chtholly_next_container_v1` callback ABI. Scalar keys and values retain the
target-generated fast path. A concrete nominal type may use the same bridge
when its `std::hash::Hash` and `std::hash::Equal` witnesses are resolved and
its move/drop representation is verified by SemIR and LowIR.

The source language gains no new syntax. Witnesses remain ordinary static
interface implementations; callback addresses are process-local and are never
written into artifacts.

## Concrete lowering

Semantic specialization records the concrete Hash/Equal function references in
the session-local `SemConcreteContainerVTable` descriptor together with stable
type and witness fingerprints. LLVM emits private callback thunks for hash and
equality and calls the existing object/lifecycle lowering for move and drop.
This preserves computed projections, custom nominal destruction, nested nominal
fields, and imported provider targets. A missing target or unsupported type
fails closed before native linking; no byte/memcpy witness is synthesized for
non-scalar types.

The descriptor layout fingerprint includes the concrete type fingerprints,
container kind, Hash/Equal witness fingerprints, and move/drop representation
facts. This is session-independent identity; `FunctionRefId` values are only
lowering handles.

## References and generation

The Chtholly reference ABI remains a single pointer. Static SemIR provenance
rejects source-level use after a structural container mutation, while native
`borrow_is_valid` checks the table generation defensively. Failed transactional
rehash leaves the old generation valid; successful reserve, insert, erase,
clear, and rehash invalidate prior generations.

## Borrowed Option payloads

Specialized `Option<const T&>` and `Option<T&>` consuming `unwrap` summaries
now carry receiver-rooted return provenance. The payload remains borrowed and
does not acquire an ownership drop. Direct `unwrap` is covered for const and
mutable container borrows; the previous `is_some`-only regression workaround
is no longer required.