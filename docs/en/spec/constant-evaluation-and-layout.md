# Chtholly Constant Evaluation And Layout

Status: normative for the source forms, closed evaluator, and cross-module
evaluator artifacts described here. Mutable or thread-local static storage and
static objects requiring union or enum byte packing remain outside this
normative subset.

## Declarations

A module constant has an explicit type. A local constant may infer its type:

```cns
pub const BufferBytes: usize = sizeof(i32[4]);

fn read(): i32 {
  const offset = 2;
  return offset;
}
```

`const` introduces a value, not storage. Every use denotes the canonical
compile-time value after the ordinary contextual conversion to the declared
type. A module constant may be `pub`; a local constant may not. Constant names
obey ordinary lexical and module lookup and cannot be assigned, moved from, or
borrowed as storage.

A constant function uses `const fn` or `pub const fn`:

```cns
pub const fn triangular(limit: i32): i32 {
  var total: i32 = 0;
  for (var index: i32 = 0; index < limit; index += 1) {
    total += index;
  }
  return total;
}
```

A `const fn` is a safe Chtholly definition with a body and immediate execution
kind. Foreign, unsafe, bodyless, and asynchronous declarations cannot be
constant functions. The same function remains callable at run time. A public
artifact records its `const` capability and publishes a verified evaluator
body closure. The closure includes private constant helpers and typed constant
dependencies needed by the public entry. An artifact-only consumer may
therefore execute an imported `const fn` in a constant context without provider
source or LLVM folding.

## Constant Evaluator

Constant evaluation uses the typed SemIR produced by ordinary semantic
checking. It does not parse source again and does not use LLVM folding.
Evaluation is left-to-right and exactly once under the language evaluation
order.

The admitted value forms are integers, IEEE floating values, Boolean values,
strings, null raw pointers, arrays, nominal aggregates, active union values,
and enum values. The admitted operations are:

- lossless and explicit numeric conversions already admitted by the language;
- the complete builtin numeric, bitwise, shift, comparison, logical, and
  three-way operator set;
- array, aggregate, union, enum, and conditional construction;
- expression `if` and `switch`;
- `for`, `while`, and `do...while`, including structured break and continue;
- calls to local and imported constant functions with evaluator artifacts;
- references to local, module, and imported public constants;
- `sizeof(T)` and `alignof(T)`.
- closed type-property queries `type_same(T, U)`, `type_is(T, category)`,
  `type_has(T, capability)`, and `array_extent(T)`.

Runtime calls, mutable storage outside the evaluator frame, foreign calls,
pointer dereference, allocation, and operations with external effects are not
constant expressions. Evaluation has a deterministic budget of 1,000,000
steps and 128 nested calls. Cycles, budget exhaustion, checked integer
overflow, division or remainder by zero, and an invalid shift are diagnosed at
the operation that fails. A failed constant declaration does not acquire a
fallback run-time initializer.

Canonical constant trees are interned by kind, semantic type, payload,
children, and target dependence. Public constants serialize this typed tree;
the public value fingerprint includes its canonical package, module, name,
type, kind, and value. Import reconstruction verifies every node before it is
admitted to SemIR.

## Closed Type-property Queries

V1 admits compiler-defined queries rather than an open reflection object.
`type_same(T, U)` compares canonical semantic types. `type_is` accepts the
closed categories `integer`, `floating`, `raw_pointer`, `array`, `nominal`,
`string`, and `reference`. `type_has` accepts `copy`, `move`, `drop`, and
`object_representation`; it observes the selected semantic representation and
does not grant a capability. `array_extent(T)` returns the fixed array bound as
`usize` and rejects non-array types.

`tuple_arity(T)` returns the canonical tuple element count as `usize`.
`element_type(T, N)` is a type-position query requiring a concrete zero-based
integer index, and `pointee_type(T)` unwraps references, raw pointers, and
borrowed slices during specialization. A dependent query is retained as a
symbolic SemIR instruction and a public generic-template descriptor; concrete
specialization folds it to an ordinary bool or `usize` literal before LowIR or
component publication. Type-position projections use a structural artifact
with a source type, a projection kind, and a zero-based index. The index is
encoded in the low 31 bits; the high bit identifies pointee projection, and
pointee projections require index zero.

These queries produce ordinary compile-time values and no run-time metadata.
Unknown categories or capabilities are ill-formed. Open reflection, AST
generation, user-defined queries, and template partial specialization are not
part of v1. Tuple arity and type-position projections use their canonical
aggregate types rather than being approximated through nominal layout; slices
expose their element through `pointee_type`.

Tuple types are canonical ordered aggregates. Borrowed `slice<T>` values use a
two-field pointer-plus-`usize` ABI and never own or extend the source lifetime;
mutability is part of the canonical and public type identity. The query
property and capability sets are closed: unknown strings are rejected during
semantic checking and artifact verification rather than treated as false.

## Layout Queries

`sizeof(T)` and `alignof(T)` return `usize` compile-time values. `sizeof`
returns the complete object size including padding; `alignof` returns the
required object alignment. Both use the compilation target selected before
semantic checking, so their results are target-dependent constants.

Queries are valid for complete scalar, string, raw-pointer, C function-pointer,
array, callback-adapter, and nominal object representations. An array must
have a nonzero bound and a queryable element layout. A custom nominal object
representation reports the selected object carrier layout.

A query is rejected for `void`, `never`, checked references, dependent types,
incomplete types, and types without an object representation. An imported
opaque nominal does not expose layout. Imported represented types may be
queried only through the frozen public representation/layout contract; a query
never reconstructs private layout from source-inaccessible fields.

## Readonly Static Storage

The admitted static declaration is module-level, explicitly typed, and
constant-initialized:

```cns
pub static DefaultAlignment: usize = alignof(i64);
static RetryLimit: i32 = 3;
```

A static is not a constant alias. It denotes one read-only object with static
storage duration and stable address identity. Every evaluation of a static
reference loads that object. Its initializer is evaluated during compilation;
there is no run-time initialization order, initialization guard, or destructor
registration in this subset.

Readonly static object encoding currently admits scalar values, strings, null
raw pointers, arrays, and ordinary struct aggregates whose object
representation is the nominal type itself. Union values, enum values, and
custom object carriers require an explicit constant object-packing contract
before they can initialize static storage and are rejected today.

A private static has module-internal linkage. A public static has one external
symbol derived from canonical package, module, and name; an imported reference
declares that same symbol and loads from it. The public artifact carries the
initializer value for checking and incremental invalidation, but consumers do
not replace a static load with that value. Changing a public static initializer
changes its artifact fingerprint while preserving its external symbol
identity.

Mutable statics, thread storage, dynamic initialization, initialization
dependencies, static destruction, and module unload are post-v1. The runtime
program/thread lifecycle machinery is not connected to the admitted readonly
static subset.

This completes roadmap entry 11 for concrete v1 constant evaluation. Symbolic
generic value parameters remain a separately reviewed specialization feature;
additional static storage forms require a post-v1 scope review, and new nominal
object-packing forms remain entry 36. Those future forms must reuse this
evaluator and provide their own evidence rather than making the admitted
evaluator partial.
