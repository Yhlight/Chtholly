# Chtholly Types And Values

Status: normative for the frozen 1.0-1.8 source versions. Primitive scalars,
numeric conversions, `never`, references, raw pointers, aggregates, nominal
types, generic specifics, callable types, and their versioned operator models
are defined here and by the corresponding release roadmap.

## Type Families

The frozen v1 type system includes the following families:

- primitive numeric, Boolean, character, and `never` types;
- fixed arrays, borrowed slices, tuple values, string literals, owned strings,
  and string views;
- standard-library dynamic owners such as `std::vec::Vec<T>` and their checked
  borrow iterators;
- opaque and represented nominal structs, payload enums, fieldless value
  enums, and C unions;
- shared, mutable, and initialization references;
- raw pointers and foreign function pointers;
- ordinary non-capturing callable types;
- aliases and interface-constrained types.

Tuple type and construction use `(A, B, C)` and `(a, b, c)`. Tuple, aggregate,
and payload destructuring uses braces and explicit sources:

```cns
let { first = copy .0, second = copy .1 } = pair;
```

Parenthesized destructuring is not part of frozen v1; brace-based
destructuring is the canonical form.

The Next compiler stores tuple element types in canonical declaration order and
publishes that sequence in `PublicType`. `value.0` is a statically checked
positional tuple projection: its index must be in range and its result has the
corresponding declared element type.

`slice<T>` is a read-only non-owning pointer-plus-length view; `slice_mut<T>`
is its explicitly mutable counterpart. Both are non-owning views and their
element mutability is
preserved across specialization and artifact import. A slice may be constructed
only from an addressable array or slice place. Its borrow provenance is retained
through projections, so a returned slice must be derived from an admissible
parameter source rather than local or temporary storage.

`std::text::as_bytes(string)` is the canonical string view projection. It
returns `const slice<u8>` with the source string's pointer, byte length, and
borrow provenance. The projection is read-only and non-owning: it cannot be
returned from a function unless the source provenance is admissible, cannot be
used after the source string is moved or destroyed, and cannot be converted to
`slice<u8>` without an independently verified mutable source. Embedded zero
bytes are ordinary data; a null-terminated C string remains an explicit CFDL
carrier rather than an implicit conversion.

Element indexing is part of the ownership model: fixed-array and tuple static
projections are independently borrowable and movable, while dynamic indices
use conservative wildcard overlap. Slice element projections are borrowed
views and cannot be moved or initialized. Borrowing a dynamic array element
conflicts with overlapping mutable access; slice wildcard accesses are checked
at the same boundary and reject unsupported ownership transitions.

Arrays and tuples preserve the borrowed ownership classification of contained
views. Their return provenance is the union of their borrowed elements, so a
tuple may return a parameter-derived slice but cannot hide a view of local
storage.

`std::vec::Vec<T>` owns a contiguous, dynamically sized allocation. Element
borrows and iterator values retain provenance to the Vec owner. Structural
operations that may relocate storage (`reserve`, `push`, `clear`, and removal
operations, including `remove`) are ordinary mutable owner writes and therefore cannot overlap a
live element loan or iterator. The owner remains live after such an operation;
only the outstanding allocation loans become invalid. Vec element moves transfer
the selected element without moving the owner.

## Shared Semantic Requirements

Every type defines value representation, object representation, initialization
representation, copy/move/destroy capability, equality availability, layout
visibility, generic identity, and component encoding. A type may intentionally
leave physical layout opaque while still publishing these semantic facts.

Implicit coercions must preserve every input value and may only narrow
authority. References are transparent aliases in expression contexts: a value,
`T&`, or `const T&` can be used where the pointee value is required, and an
addressable value can bind to `const T&` without writing `&` at the call site.
The compiler still rejects dangling references, mutable-authority upgrades, and
implicit moves from a borrow. `&expr` remains the explicit borrow operation;
raw pointers never enter this transparent-reference conversion.
Explicit and checked conversions use independently verified rules. Raw-pointer
conversion never manufactures checked-reference provenance.

`sizeof(T)` and `alignof(T)` are target-dependent `usize` compile-time values
and may expose only layout committed by the current module or a public
representation contract. Their complete admitted boundary is specified in
`constant-evaluation-and-layout.md`.

## Fieldless Value Enums

Language 1.8 admits a distinct value-enum form when an enum declaration uses
explicit discriminants. Every variant must provide a unique i32 literal and
must have no payload. Value enums are represented by that signed 32-bit value,
have size and alignment 4, construct as `Type::Variant`, and convert only by an
explicit `as i32`. They do not inherit integer operators or conversions.
Payload enums remain algebraic data types with ordinal runtime tags. The full
declaration and switch contract is specified by `v1.8-language-roadmap.md`.

## Never

`never` is the uninhabited type of a computation that cannot produce a value.
It has no values, object representation, initialization representation, size,
or alignment. Source cannot construct a `never` value. It is permitted as an
ordinary Chtholly function return type and as a generic type argument. It is
not permitted as a by-value parameter, field, enum payload, or array element;
those positions require object representation. Raw pointers and function
results may name `never` without creating an object of that type.

A `never` expression adjusts to any required result type solely because its
path cannot provide a value. This is a control-flow rule, not an implicit
conversion and not an overload-ranking preference. A set of branches with no
reachable result has type `never`.

## Contextual Control-Expression Results

An expected result type from an explicit local, return, call parameter, or
enclosing `if`/`switch` result flows into the expression forms that admit that
context. A function-value context selects exactly one matching non-capturing
overload, and a raw-pointer context types `null`. Without an expected type,
control results and nonempty array elements converge symmetrically: the
compiler finds a unique type to which every reachable value can apply an
already admitted adjustment. `never` arms are excluded from the candidate set.
Ambiguous or absent common types are an error.

The implemented adjustment set covers identity, `never`, contextual numeric
literals, the normative lossless numeric matrix, checked-reference projection,
and permitted reference or raw-pointer authority narrowing. Common-type
selection minimizes conversion cost and then chooses the unique narrowest
viable target. Source order never selects a result.

This contextual model is complete for the v1 forms described here. Empty
aggregates, tuple and slice construction, and environment-bearing callable
literals remain with their owning roadmap entries and must reuse this model if
later admitted.

## Numeric Types And Conversions

The fixed scalar types are `i8`, `i16`, `i32`, `i64`, `u8`, `u16`, `u32`,
`u64`, `f32`, and `f64`. `isize` and `usize` are signed and unsigned integers
with the active target pointer width. Their identity is target-dependent and
is materialized before semantic checking.

An implicit conversion must preserve the complete source type domain exactly:

| Source | Implicit target |
| --- | --- |
| signed integer | same-signed integer of at least the same width |
| unsigned integer | unsigned integer of at least the same width, or a signed integer of strictly greater width |
| integer | `f32` when its value-bit count is at most 24; `f64` when at most 53 |
| `f32` | `f64` |

All other numeric conversions require `as` or `as?`. In particular,
signed-to-unsigned, float-to-integer, narrowing, and any conversion that loses
integer precision are never implicit. Reference and raw-pointer authority may
narrow only when the pointee identity is unchanged. Nominal and callable
values do not gain structural implicit conversions.

`as` uses deterministic target-width conversion. Integer narrowing keeps the
low target bits, integer widening uses the source signedness, integer-to-float
uses IEEE rounding, and float narrowing uses IEEE rounding. Float-to-integer
truncates toward zero and traps before conversion for non-finite or
out-of-range input.

`value as? T` returns
`std::result::Result<T, std::convert::CastError>`. It returns `Ok` only when the
mathematical value is preserved exactly. Failure is `NonFinite` for a
non-finite floating input, `OutOfRange` when no target value exists, and
`Inexact` when the target exists only after rounding or truncation.

## Remaining Aggregate Requirements

The aggregate review owns bounds, encoding, slice provenance, public enum
evolution, active C-union alternatives, and partial ownership. The v1 scalar
operator and numeric failure rules are normative in
`expressions-and-control-flow.md#builtin-operators`.
