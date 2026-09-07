# Chtholly Declarations Callables And Generics

Status: normative for free and inherent callable overloads, default arguments,
named arguments, generic type deduction, public callable identity, nominal
constructors, and compiler-invoked destruction described here. Interfaces are
part of frozen 1.0. Environment-bearing closures and bound methods are admitted
by 1.2, and operator protocols by 1.3. Compile-time generic value parameters
are not admitted by frozen 1.0-1.8.

## Callable Families

The implemented callable families are free functions, associated functions,
nominal constructors, instance methods, and zero-environment function values.
An instance method has
an explicit first parameter named `self` and is called with `value.name(...)`.
An associated function has no `self` parameter and is called with
`Type::name(...)`. A constructor is declared as `fn init(...)` in an ordinary
`impl T` and called as `T::init(...)`; it has no explicit `self` parameter.
Both remain outside the module-level free-function namespace.

The complete versioned callable design relates without conflating bound
methods, environment-bearing closures, unsafe and foreign function pointers,
destructors, interface requirements, and operator definitions. Allocation is
not implied by nominal construction.

## Source Overload Identity

Free, associated, and instance functions form closed overload sets. A source
overload is identified by:

- canonical package, module, and function name;
- canonical nominal owner and instance or associated member kind;
- generic type-parameter count and binding positions;
- the ordered parameter type pattern, including the explicit instance
  receiver.

Generic parameter names are not identity. Return type, parameter names,
defaults, `const`, `unsafe`, and semantic contracts cannot distinguish two
source overloads. Two declarations with the same source overload identity must
be compatible redeclarations; two definitions are rejected. `main` and
foreign C declarations never form overload sets.

## Default And Named Arguments

A default follows its parameter type and defaults must form a trailing suffix:

```cns
fn scale(value: i32, factor: i32 = 2, bias: i32 = 0): i32 {
  return value * factor + bias;
}
```

A default is checked in its declaration context, converted to the parameter
type, and executed by the constant evaluator. Only a concrete compile-time
value is accepted. Foreign and variadic declarations cannot provide defaults.
Public artifacts retain the typed constant tree so an artifact-only consumer
materializes the same value; defaults do not add native ABI parameters or
runtime evaluation.

A named argument uses `.name = expression`:

```cns
let result = scale(10, .bias = 3);
```

Positional arguments must precede named arguments. A parameter may be bound at
most once, every name must exist on the selected declaration, and every
unbound parameter must have a default. Public parameter names are stable
source-interface facts. Renaming one changes the public entity fingerprint but
does not create a different native overload identity.

The receiver and explicit argument expressions evaluate left-to-right exactly
once in source order. Argument binding may reorder their already-evaluated
values into parameter order. Default constants fill remaining parameters in
parameter order.

## Candidate Resolution

Direct calls collect every visible candidate from the selected free or
owner-qualified member overload set. For a generic candidate, explicit owner
arguments and argument deduction first select a concrete specialization;
conversion ranking then uses its concrete parameter types. A candidate is
viable only when argument binding succeeds, generic deduction succeeds, its
receiver can bind, and every supplied value has an admitted implicit
conversion.

Viable candidates are ordered lexicographically by:

1. worst conversion rank;
2. sum of conversion ranks;
3. non-generic before generic when the conversion ranks tie.

Identity and `never` conversions have rank 0, contextual literals and `null`
have rank 1, and admitted lossless numeric, reference acquisition, and
authority-narrowing conversions have rank 2. An exact candidate therefore
beats a converting candidate. Explicit conversions never enter resolution.
Equal best tuples are ambiguous; declaration order is not a tie breaker.

A function name used as a value does not run general call ranking. Its
contextual function type must select exactly one concrete callable. An
unconstrained overload set is ambiguous.

## Public And Native Identity

Artifacts preserve overload sets rather than choosing the first spelling.
Each callable publishes parameter names, typed default constants, owner/member
kind, generic shape, parameter pattern, return and semantic contracts, and a
stable entity fingerprint. Imported lookup and incremental observations select
the exact entity fingerprint. Tooling uses that same identity for hover,
definition, references, and grouped completion, so references to one overload
do not leak into another.

Native Chtholly symbols append two independent 12-hex fingerprints to the
qualified package/module/name prefix. The first commits to source overload
identity. The second commits to the concrete callable ABI type after generic
specialization, including canonical nominal identities and callable ownership
or registration contracts. This lets generic and non-generic overloads that
concretize to the same machine signature remain distinct while provider and
artifact-only consumer sessions produce the same symbol. Foreign C symbol
names are unchanged.

The source-overload hash domain is version 4. A compiler-generated readable
`$specific$...` suffix is excluded from that lane and affects only the concrete
ABI lane. Consequently two specializations of one generic entity retain one
source lane while differing in their concrete lanes.

The detailed encoding and compatibility boundary are recorded in
`../design/next-call-resolution-and-callable-abi.md`.

## Remaining Boundary

Constructor selection uses the same owner-qualified candidate planner as
ordinary associated calls, including defaults, named arguments, conversions,
artifact lookup, and stable identity. Source checking admits `Self` or
canonical `std::result::Result<Self, E>`. The public ABI identity includes
constructor epoch 1 and either `DirectSelf` or `FallibleSelf`; changing that
result contract changes the public entity fingerprint.

Placement is written `T::init(args) in destination`. It evaluates arguments
before the destination, returns `void` for direct construction, and returns
`std::result::Result<void, E>` for fallible construction. The fallible native
ABI is `(Self* success, Result<void, E>* outcome, args...) -> void`; ordinary
calls reconstruct `Result<Self, E>` from these channels. Representation
`init` remains a separate canonical role and is never selected by `T::init`.
`drop(self: Self&)` remains a compiler-only canonical lifecycle role and does
not enter ordinary call resolution.

This section's 1.0 callable identity excludes extension methods and partial
explicit function-generic arguments. Interface constraints are specified by
`interfaces-and-specialization.md`; bound methods and closures by the 1.2
specifications; operator protocols by the 1.3 roadmap. All reuse the same
candidate collection and concrete callable identity.

This completes public and native identity for every callable family admitted
by v1. A future callable family owns its additional identity evidence and does
not make this closed family set partial.
