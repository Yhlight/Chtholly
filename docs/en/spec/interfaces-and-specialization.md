# Interfaces, Constraints, And Specialization

Status: normative for Chtholly v1. Implementation status is tracked separately
by `support/chtholly-v1.toml`.

This chapter defines source interfaces, transparent type aliases, generic
constraint satisfaction, static witness dispatch, and the boundary between
ordinary library protocols and compiler-owned semantic protocols. Interface
values and dynamic dispatch are not part of Chtholly v1.

## Source Forms

An interface is introduced by `trait`. Its implicit `Self` type is available
throughout its constraints, associated aliases, and function declarations.

```cns
pub trait Hashable<Salt> {
  alias Output;
  alias Storage = u64;

  fn hash(self: const Self&, salt: Salt): Output;
  fn ready(self: const Self&): bool { return true; }
}
```

A function without a body is required. A function with a body is a default
definition. An associated alias without a target is required; one with a
target supplies a default. Associated constants and interface values are not
admitted. An interface may require another interface through an ordinary
constraint on `Self`; v1 has no separate inheritance production. Requirement
functions do not introduce their own generic parameters or `where` lists in
v1; shared requirements belong to the interface declaration.

A conformance uses the following form:

```cns
impl Hashable<i32> for Record {
  alias Output = i32;
  fn hash(self: const Self&, salt: i32): i32 { return salt; }
}
```

Every required member must be supplied exactly once. A default member may be
overridden. An implementation member must conform to the substituted requirement
signature precisely before ordinary call conversions are considered. Extra
members, missing members, an unresolved associated alias, and a mismatching
signature are errors.

Generic constraints follow a declaration's generic parameters:

```cns
fn consume<T>(value: const T&): T::Output where T: Hashable<i32>;
pub alias OutputOf<T> where T: Hashable<i32> = T::Output;
```

Constraints are an unordered conjunction for satisfaction and a canonical
ordered list for identity. Duplicate constraints are removed. V1 has no
negative, disjunctive, conditional, or recoverable constraint form. `T::Item`
selects an associated alias from the constraints on `T`; zero matches is an
unsatisfied projection and multiple distinct matches is ambiguous.

`alias Name = Type;` declares a transparent type alias. Generic aliases
substitute their type arguments and then normalize their target. Alias names
retain declaration identity for lookup, diagnostics, tooling, visibility, and
artifacts, but do not create a distinct runtime or canonical type. Recursive
normalization is ill-formed. A public alias publishes its normalized target,
generic shape, and constraints without publishing private identities.

## Identity And Coherence

An interface declaration has a session-local identity and a portable public
identity. The portable identity contains its canonical package, module, name,
generic shape, normalized constraints, associated aliases, function
requirements, and default definitions. Changing any of these facts changes
the interface fingerprint.

A specific interface is an interface identity plus its canonical interface
arguments. A conformance key is a canonical `Self` type plus a specific
interface. The current package may declare a conformance only when it owns the
interface or owns the outermost nominal definition of `Self`. Builtin scalar
types have no separately ownable nominal definition.

At most one conformance for a key may occur in a package dependency closure.
Conformances are package coherence facts rather than ordinary overloads. A
conformance involving public identities is exported automatically; `pub` does
not form a second conformance visibility lane. Generic conformance parameters
must all be structurally deducible from `Self` or interface arguments. The
compiler rejects generic conformance patterns that might overlap. V1 has no
ordering, specialization, negative conformance, or source-order tie breaker.

Requirement slots are ordered canonically by member kind, canonical name, and
signature fingerprint. Source declaration order does not affect slot identity.
Default definition bodies remain part of the interface fingerprint.

## Satisfaction And Static Witnesses

Constraint lookup produces one of three semantic outcomes:

- `Satisfied`, with witnesses in canonical constraint order;
- `Unsatisfied`, with no diagnostic emitted while probing a candidate;
- `Invalid`, after a malformed or cyclic program has been diagnosed.

Lookup considers the current package and the packages that own the interface
and outermost `Self` nominal identity. Its cache key contains canonical `Self`,
the specific interface, and the completed provider-closure epoch. A negative
result is cached only after that closure is known. Lookup maintains an
explicit stack and diagnoses a cycle with the complete requirement chain.

An interface witness is distinct from the nominal representation and
lifecycle witness. A conformance declaration shell owns witness identity. A
completed conformance owns one immutable table containing canonical
requirement slots, direct function targets, associated alias results, and any
required child-witness fingerprints. Call sites and generic specifics borrow
the witness; they do not copy its ownership. Persistent artifacts and their
content-addressed leases own serialized witness lifetime.

Generic template bodies refer to a symbolic witness and requirement slot.
Concrete specialization resolves that operation to a direct callable target.
No vtable, hidden runtime witness argument, object-safety rule, downcast, or
interface-value ABI is introduced by v1.

## Inference And Specialization

Function type arguments are either all explicit or all inferred from bound
call arguments. Partial explicit type arguments and result-only inference are
not admitted. Candidate processing is ordered as follows:

1. bind positional, named, and default arguments;
2. infer or validate every type argument;
3. satisfy the substituted interface constraints;
4. rank admitted conversions against concrete parameter types;
5. apply the normative overload ordering.

An unsatisfied constraint removes a probed candidate. If no candidate remains,
the final diagnostic identifies the candidate and its first canonical missing
or invalid constraint. Declaration and import order never select a winner.

Specialization identity has three separate layers:

1. the session key is the `GenericId`, canonical constant arguments, and
   ordered session witness identities;
2. the portable request fingerprint is the public generic entity, public type
   arguments, ordered witness fingerprints, and semantic-options fingerprint;
3. the immutable result fingerprint covers concrete code, calls, and the full
   witness dependency closure.

Session IDs and visible-conformance iteration order never enter a portable
fingerprint. A cached result whose request, semantic epoch, dependency, or
witness fingerprint differs is not reusable. Recursive specialization retains
the existing declaration/definition state split and explicit cycle handling;
constraint lookup is completed before code generation.

Constant value parameters remain outside v1.

## Core And Library Protocol Boundary

`core::Copy`, value representation, object projection, and object lifecycle
are compiler-owned semantic protocols. Their declarations and witness epochs
are versioned compiler facts. Ordinary source interfaces cannot grant these
capabilities or manufacture their witnesses.

Equality, ordering, conversion, indexing, and callable abstractions may be
ordinary standard-library interfaces with named methods. V1 does not connect
them to operator lookup, implicit conversions, indexing syntax, or
environment-bearing callable values. Recoverable failure remains the canonical
`std::result::Result<T, E>` model; it is not an interface protocol. Destruction
remains compiler-driven nominal lifecycle rather than an ordinary callable
interface.

Interface dispatch does not cross the C or component ABI as a dynamic value.
Only the selected concrete callable and its already-defined ABI may cross such
a boundary.

## Diagnostics And Tooling

The compiler diagnoses duplicate or orphan conformances, possible generic
overlap, unsatisfied and cyclic constraints, incomplete witnesses, signature
mismatch, ambiguous projections, recursive aliases, and private identities in
public aliases or interfaces. Recovery never installs a witness for an invalid
declaration.

Definition, references, hover, and completion use the same canonical
interface, requirement, conformance, and alias identities as semantic lookup.
Generated or artifact-only consumers must produce the same answers as a source
consumer.
