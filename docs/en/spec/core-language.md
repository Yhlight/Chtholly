# Chtholly Core Language

Status: normative source direction. The replacement compiler may implement a
documented subset, but implementation experiments do not extend this surface.

## Core Spelling

Chtholly retains `fn`, explicit parameter types, colon return types, `let` and
`var`, named aggregates, `move`, `copy`, `const T&`, `T&`, raw
pointers, `unsafe`, modules, nominal types, generics, `impl` blocks, and
Chtholly policy prefixes such as `repr(...)` and `lifecycle(...)`.

```cns
fn inspect(value: const Data&): i32
fn update(value: Data&): void
fn consume(value: Data): Result<void, Error>

let snapshot = copy value;
let owned = move value;
```

`let name: T;` and `var name: T;` create typed storage without an initializer.
The place starts uninitialized and is passed to an eligible call using its
ordinary name. The call must publish an exact `Initialize` effect for that
parameter; reads, moves, returns, ordinary borrows, and scope cleanup before
that transition are rejected. A `let` place freezes after initialization,
while a `var` place remains mutable. CFDL `out T` lanes are one producer of
verified Initialize effects, but CFDL spelling is not Chtholly source syntax.

There are no named lifetime or origin parameters in source, and no postfix
origin annotation syntax. A callable definition with a body does not spell an
effect, postcondition, or return-provenance summary: its body is the source of
those facts. A bodyless declaration may instead carry an explicit `contract
{}` summary when the implementation is supplied elsewhere or the declaration
is an opaque callable boundary.

## Contract Abstraction Boundary

Chtholly has one compiler-owned canonical contract model. Ordinary source does
not construct or compose its proof facts. CFDL lets binding authors state
resource types, flow relations, ABI outcomes, and release obligations, from
which the compiler derives Interop facts. CFDL has a dedicated binding source
boundary and publishes an artifact; it is not a Chtholly declaration mode.

Named fact bundles such as `contract F` and contract attachment through `where`
are not part of the language direction. `where` remains a type and generic
constraint facility. The normative layering and proposal review gate are in
[Contract Abstraction Layers](contract-abstraction-layers.md).

## Calls And Callable Values

Argument evaluation is left to right. Ownership transfer and borrow creation
must remain visible at a named source when they can change later source use.
Final-use moves and temporary transfers may be inferred. Borrow creation is
checked against the callee parameter capability and may not extend the source
provenance.

Checked references are transparent borrowed aliases. Source expressions do not
need a dereference operator for `T&` or `const T&`; value contexts resolve any
reference alias chain to its pointee, and ordinary parameters may bind an
addressable value or a temporary const reference directly. Assignment never
rebinds a reference, `&ref` reborrows the referent rather than constructing a
reference to a reference, and projection cannot add mutable authority. These
conversions never create an implicit move. Borrow escape, temporary lifetime,
and owner provenance are checked independently by the ownership analyzer. Raw
pointers do not participate in this adjustment. Initializing previously
uninitialized reference storage establishes its reference identity once; this
is initialization rather than reference assignment.

Raw-pointer dereference uses the traditional prefix expression `*pointer` and
must occur in an `unsafe` block or `unsafe expression`. The result is a place,
so the initial surface supports reading it, writing it through `T*`, and
projecting fields or array elements as `(*pointer).field` and
`(*pointer)[index]`. A `const T*` pointee is read-only. Dereference does not
create a checked reference or grant ownership capabilities: `&*pointer`,
`move *pointer`, `copy *pointer`, their projected forms, and method-receiver
adjustment through a raw pointee are rejected. `void*` and function-pointer
values are not dereferenceable. Pointer indexing, `->`, arithmetic, and casts
are separate future features rather than alternate spellings for this rule.

Nominal types may define inherent instance methods in the module that defines
the type. The receiver is an explicit first parameter named `self`; its type is
`const Self&`, `Self&`, or `Self`. The receiver participates in ordinary
generic deduction as parameter zero and methods use the ordinary function ABI.

```cns
pub struct Box<T> { value: T; }

impl<T> Box<T> {
  pub fn identity(value: T): T { return move value; }
  pub fn zero(): i32 { return 0; }
  pub fn get(self: const Self&): const T& { return &self.value; }
  pub fn replace(self: Self&, value: T): void {
    self.value = move value;
    return;
  }
  pub fn consume(self: Self): T { return move self.value; }
}

let view = box.get();
let value = (move box).consume();
let inferred = Box::identity(42);
let zero = Box<i32>::zero();
```

An inherent `impl` for a generic owner must repeat the complete original type
parameter pattern in order. A function whose first parameter is named `self`
is an instance method; a function without a `self` parameter is an associated
function. Associated functions use `Type::function(...)` and never bind an
instance receiver. Generic owner arguments may be inferred from ordinary call
arguments or stated completely as in `Box<i32>::zero()`. `new` is not reserved
and has no constructor-specific behavior. Each member function controls
visibility with `pub fn`. Fields, instance methods, and associated functions
share one nominal member namespace and may not collide. Method-level generics,
extension methods, overload sets, implicit general conversions, and method
values are not part of this surface.
Method-call adjustment is deliberately narrow: direct shared/mutable borrowing
and reborrowing are supported; a by-value place receiver requires explicit
`move`. A read-only receiver cannot call a method requiring `Self&`, and a
temporary that is not a place cannot be implicitly borrowed as a method
receiver. These rules apply identically to methods loaded from another
module's public artifact. Associated functions use the ordinary function ABI
without a hidden receiver. Private member functions remain visible only inside
the defining module; public artifacts do not disclose their names.

An ordinary callable value uses the non-capturing function type `fn(T...): U`.
It is an immutable, zero-environment reference to a safe concrete ordinary or
associated function and has trivial copy semantics. Generic templates require
concrete specialization before forming a value. Instance methods, unsafe
functions, and foreign declarations do not form ordinary callable values.

```cns
fn(i32): i32
```

Environment-bearing closures and capture-derived invocation capabilities are
not part of source version 1.0. They are admitted by language 1.2 with the
callable, ownership, cleanup, provenance, and component-identity rules in
`environment-bearing-callables.md`.

## Callable Contracts

Where a bodyless Chtholly declaration uses a contract block, it describes only
compiler-checked access and postcondition facts for that callable. A contract
block cannot be attached to a definition with a body, and it cannot declare an
ABI, external symbol, callback role, resource protocol, or release obligation.
Those facts belong to the CFDL artifact and cannot be overridden by Chtholly
source. A contract is one declaration-side summary; it is not a second effect
language inside a function body.

Contract entries are `reads`, `writes`, `takes`, `borrows shared`, `borrows
mutable`, `ensures initialized`, `ensures invalidated`, and `returns borrow`.
A return guard is a pure Boolean expression over Boolean parameters and
literals using `!`, `&&`, and `||`. A final `otherwise` arm means the complement
of the preceding return guards. Guards cannot call functions, inspect object
state, or create ownership effects.

Control-flow inference uses the same condition model. A borrowed return is
published only for paths that reach that return. A path that recurses forever
or terminates without returning does not widen a guarded source into an
unconditional source.

## Representation Policies

Nominal types are opaque by default. `repr(C)` opts an eligible nominal into
target C layout:

```cns
repr(C) pub struct Pair {
  pub left: i64;
  pub right: f64;
}

repr(C) union Value {
  integer: i64;
  pointer: void*;
}

let value = Value { .integer = 7 };
```

Custom value/object carriers and lifecycle selection use Chtholly prefix
policies such as `repr(value = T)`, `repr(object = Storage)`, and
`lifecycle(copy = custom, drop = custom)`. Field projection policies use the
same prefix form immediately before the field. Bracketed attribute syntax is
not part of the replacement language.

`repr(C)` fixes representation, not ownership. It cannot be combined with an
empty aggregate, a custom carrier, or a computed or bit-packed projection.
Nested fields must themselves have a C-compatible representation before the
type may cross a C boundary. A generic `repr(C)` definition is a delayed
layout template: every concrete specific is checked again after substitution,
and a non-C-compatible specific is rejected.

A union must use `repr(C)` and an initializer must name exactly one member.
Union members have no defaults and must be trivially copyable, movable, and
destructible C-compatible object types. Construction and direct member
assignment establish that member as active. Safe member access requires the
compiler to prove that the named member is definitely active. `unsafe`
authorizes one access when the programmer establishes the C active-member
precondition; it does not create or refine an active-member proof.

## Failure And Unsafe Boundaries

Recoverable failure is represented by ordinary typed values and propagated by
postfix `?`. In v1 its operand is exactly canonical
`std::result::Result<T, E>`, and the enclosing function returns canonical
`std::result::Result<U, E>` with the same `E`. Success transfers the `Ok`
payload into the continuing expression. Failure transfers the `Err` payload
into the enclosing return value and follows the same place-state and cleanup
rules as an explicit return. There is no implicit error conversion or
user-defined residual protocol. `unsafe` authorizes operations with documented
preconditions; it does not disable type checking, initialization checking, or
component contract verification.
