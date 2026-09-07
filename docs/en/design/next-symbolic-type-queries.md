# Next Symbolic Type Queries

This design records the v1 closure for compile-time type queries and type
position projections. It follows the existing SemIR -> public template ->
concrete component pipeline and does not add run-time reflection or metadata.

## Source Forms

The admitted forms are:

```cns
type_same(T, U)
type_is(T, integer | floating | raw_pointer | array | nominal | string | reference)
type_has(T, copy | move | drop | object_representation)
array_extent(T)
tuple_arity(T)
element_type(T, N)
pointee_type(T)
```

`array_extent` and `tuple_arity` produce `usize`. The other value queries
produce `bool`. `element_type` requires a concrete, zero-based index and
`pointee_type` accepts references, raw pointers, and slices. Unknown property,
capability, query kind, or projection index is ill-formed.

## Three Phases

Semantic checking folds queries whose source types are concrete. In a generic
body, a dependent query remains a typed `SemTypeQuery` instruction. Public
generic artifacts persist a descriptor side table and a `TypeQuery` opcode;
they never persist a source spelling or an open reflection object.

During specialization, generic arguments are substituted first. Queries are
then evaluated against the canonical substituted type. The cloned SemIR must
contain only a bool or integer literal, and a type projection must have folded
to a concrete structural type. LowIR and concrete component publication reject
unresolved query or projection nodes. This is the same fail-closed boundary
used by aggregate and lifetime artifacts.

## Projection Artifact

A projection is encoded structurally as `(source, packed_index)`. The low
31 bits of `packed_index` carry the zero-based element index. The high bit is
set for `pointee_type`; pointee projections require index zero. `element_type`
uses tuple or fixed-array sources and is checked against the canonical arity or
bound. A persisted projection source is a dependent type parameter or another
projection in the same chain; other structural shells must already have been
resolved or rejected by semantic checking.

## Compatibility

The query/property whitelist and projection encoding are artifact-schema facts.
Changing them requires a package-schema bump and fail-closed rejection of older
artifacts. The current package schema is `CHNXTPK61`, concrete component schema
is `CHNXSCC40`, and the persistent cache namespace is `next-v32`.
