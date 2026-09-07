# Next Nominal Construction, Placement, And Cleanup

Status: implementation record for the v1 normative boundary.

## Constructor Role

An ordinary `impl T` may declare an associated `fn init(...)`. A constructor
has no receiver and is selected only by `T::init(...)`. The semantic contract
uses `CallableSemanticRole::Constructor` and
`CallableSemanticDomain::NominalConstruction`; representation `init` remains
under `ValueRepresentation` and cannot enter this overload set.

The return type must be `Self` or canonical `std::result::Result<Self, E>`.
Constructor candidates use the normal owner-qualified overload planner,
including defaults, named arguments, conversion ranking, public artifact
identity, and cross-module lookup. The constructor role is part of the
semantic contract and therefore of the callable fingerprint, while the native
source name remains owner-qualified (`T::init`).

## Placement Boundary

The admitted spelling is:

```cns
T::init(args) in destination;
(T::init(args) in destination)?;
```

Placement is an expression and performs no allocation. Constructor arguments
evaluate left-to-right before the destination expression. The destination must
be a mutable local place in the moved state; placement is not assignment over a
live object. A direct constructor has type `void` in placement context. A
fallible constructor has canonical `Result<void, E>`, so postfix propagation
uses the second spelling above without inventing another failure protocol.

The native constructor ABI epoch is 1:

```text
direct:   (Self* success, args...) -> void
fallible: (Self* success, Result<void, E>* outcome, args...) -> void
```

The caller owns both slots. `Ok` commits the success slot to the destination
and writes `Ok(void)` to the outcome slot. `Err` writes only the error outcome;
the destination remains moved and non-live. Constructor-owned locals and any
registered partial work clean up in reverse construction order on return. A
committed destination acquires exactly one cleanup obligation and is destroyed
exactly once; an error path never destroys it.

An ordinary fallible `T::init(args)` call uses the same ABI and reconstructs
the source-level `Result<Self, E>` from the two channels. LowIR retains a
single `Construct` operation plus explicit commit/error CFG rather than
encoding constructor policy in LLVM lowering.

## Temporary And Destructor Ownership

`Self` constructor results are temporaries until bound, returned, or
transferred to a placement destination. A transferred result has exactly one
cleanup owner. `Result<Self, E>::Err` contains no live `Self` payload.
`drop(self: Self&)` is compiler-only and is invoked once for each live object,
including full-expression temporary cleanup. Cleanup plans stay immutable at
function level and are consumed by lowering; lowering does not infer
ownership.