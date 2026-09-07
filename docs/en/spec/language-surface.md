# Chtholly v1 Language Surface

Status: frozen normative Chtholly v1 baseline. This document defines the source
surface admitted by Next for source version 1.0. The complete frozen scope is
indexed by `v1-language-roadmap.md` and `support/chtholly-v1.toml`.

Parser acceptance, a reserved token, or an internal SemIR operation does not
admit syntax into v1. An admitted feature requires a
coherent source form, static semantics, ownership and cleanup behavior,
independent-compilation representation, lowering, diagnostics, and applicable
tooling.

## Lexical And Names

Chtholly source is UTF-8. Identifiers, scalar and string literals, comments,
punctuation, and operator precedence have one canonical spelling implemented by
Next. A token reserved for recovery or post-v1 work is not a v1 production.

## Modules And Visibility

Every named source unit declares `module path;`. `import path;` makes a module
available through qualified lookup, while `import path as alias;` selects a
consumer-local qualified name. `export import path as alias;` forwards
canonical public identities, and `pub` admits an entity to the semantic
component interface. Re-export never creates a wrapper entity; aliases never
change provider identity.

## Bindings And Control Flow

`let` creates a binding initialized once. Both `let name: T;` and
`var name: T;` may initially create an uninitialized place; only a verified
callable Initialize effect can establish its first value. The `let` place then
freezes, while `var` storage may be reassigned subject to place-state and
cleanup rules. v1 structured control
flow consists of blocks, `if`/`else`, `while`, C-shaped `for`, `do...while`,
unlabeled `break` and `continue`, `return`, `defer { ... }`, assignment, and
expression statements. `if`, `while`, `switch`, and the trailing condition of
`do...while` require parentheses.

`for (initializer; condition; step)` evaluates its initializer once, then its
condition, body, and step in that order. Each clause may be omitted; an omitted
condition is `true`. The initializer accepts one `let`/`var` declaration, one
expression, or one assignment clause. The step accepts one expression or one
assignment clause. These assignment clauses remain statements with `void`
semantics and do not make assignment a general expression. `continue` enters
the step of a `for`, the condition of `while` or `do...while`; `break` enters
the nearest loop exit. Every edge performs lexical cleanup in reverse order.
The `for` initializer scope includes its condition, body, and step and ends at
the loop exit. This paragraph defines the 1.0 baseline. Language 1.3 admits
labeled loop control, and language 1.4 admits ownership-aware `foreach`; their
grammar and cleanup rules are defined by the corresponding version roadmaps.

`defer { ... }` registers one cleanup body in the nearest lexical block. Defer
and owned-local registrations execute in one strict LIFO order. Captured places
are observed at execution time. The body must fall through; nested defer,
return, residual propagation, and control escaping the body are rejected.
Unrecoverable termination and coroutine suspension are outside this cleanup
edge model.

## Nominal Types And Patterns

v1 admits structs, payload enums, named aggregate construction, and exhaustive
enum `switch` expressions with a wildcard fallback. `repr(C)` unions are the
only untagged union form and follow the active-member model in the abstract
machine specification.

## Ownership And References

Owned values are affine. `move` performs destructive transfer and `copy`
requires copy capability. `const T&` and `T&` are the complete v1
checked-reference capability spellings. Borrowed byte views use the explicit
`slice<T>` (read-only) and `slice_mut<T>` (mutable) type forms; a slice never
transfers element ownership. Initialization authority is a verified
callable effect, not a third source reference type or a call-site operator.
Source contains no lifetime or origin parameters. Raw pointer operations are
covered only by the explicit unsafe boundary.

`let reference = &temporary_expression;` is the explicit temporary lifetime
extension form and always produces read-only authority. A stable projected
borrow such as `let field = &make().value;` extends the complete root temporary
to the local binding's lexical exit. Call arguments are not extended beyond
their full expression, and a temporary reference cannot be returned. This uses
the existing borrow and binding syntax; there is no lifetime keyword or ABI
annotation.

## Generics And Inherent Functions

v1 admits generic functions and generic nominal types with inferred or explicit
type arguments. Inherent `impl` blocks define instance functions with an
explicit first `self` parameter and associated functions without one. Free,
instance, associated, and constructor calls support overloads, constant
defaults, and `.name = value` named arguments. Static interfaces and their
canonical implementation witnesses are part of the frozen surface. Interface
values, dynamic dispatch, extension methods, and method-level generics are not
admitted by this baseline. Bound method values are admitted by language 1.2.

## Callable Contracts

The source contract form is restricted to bodyless callable declarations:

```cns
fn lookup(source: const Buffer&): const Item& contract {
  borrows shared source;
  returns borrow source;
}
```

Definitions with bodies infer ownership and effect facts from their bodies and
do not carry a contract block. Contract entries remain the existing forms:
`reads`, `writes`, `takes`, `initializes`, `borrows shared`, `borrows mutable`,
`ensures initialized`, `ensures invalidated`, and `returns borrow`.
`initializes p` grants one initialization capability for an uninitialized place
`p` and must be paired with `ensures initialized p` on successful returns. A
return guard uses Boolean parameters and literals with `!`, `&&`, and `||`;
`otherwise` is the final complement arm. Contracts describe Chtholly ownership
facts only. ABI, foreign resource flow, callback roles, and release obligations
remain in CFDL artifacts.

The compiler may attach a bounded boolean path condition to an inferred
postcondition. This is an internal/artifact fact, not a source contract
spelling: `ensures initialized` and `ensures invalidated` remain unconditional
declarations. Existing `switch` and `if` are the only Result recovery forms;
Result discriminant conditions are reserved for a later outcome-channel
extension.

## Nominal Construction And Placement

An ordinary `impl T` declares a constructor as `fn init(...)` returning `Self`
or canonical `Result<Self, E>`. It is called as `T::init(args)`. Placement is
the expression `T::init(args) in destination`; the destination must be a
mutable local place already moved from. The direct form returns `void`, while
the fallible form returns `Result<void, E>` and may be written
`(T::init(args) in destination)?`. Construction performs no implicit
allocation. `drop(self: Self&)` is invoked only by the compiler for each live
object and is not an ordinary callable.

## Representation And Lifecycle

Nominal representation is opaque by default. `repr(C)` selects verified target
C layout. The admitted `repr(...)` and `lifecycle(...)` policies are the
compiler-verified custom carrier and deterministic lifecycle subset defined by
the core language and abstract machine; bracketed attributes are not an
alternate spelling.

## Unsafe And Foreign Boundaries

`unsafe` grants authority for operations with specified external preconditions
without disabling initialization, ownership, or type checking. Chtholly source
does not declare foreign functions or inline ABI contracts. C representations
and physical foreign signatures belong to CFDL artifacts; Chtholly source
consumes only ordinary imported public entities.

## Components And CFDL

Semantic component interfaces preserve canonical identity, ownership,
provenance, effects, cleanup, representation, and ABI facts. CFDL is the
separate binding-author source boundary for incomplete foreign identities,
explicit scalar/pointer carriers, ABI-only record carriers, and resource flow;
ordinary `.cns` users consume only its exported artifact entities and never
the carrier fields.

## Failure Propagation

Recoverable failure uses the canonical
`std::result::Result<T, E>` payload enum. Postfix `?` consumes a Result value:
the `Ok` arm produces its `T` payload, while the `Err` arm constructs the
enclosing function's Result return value and exits through ordinary return
cleanup. The enclosing return type must be `std::result::Result<U, E>` with
the exact same error type. The implemented baseline provides no implicit error
conversion, exception path, interface abstraction, or user-defined residual
protocol. A user enum named `Result` does not acquire this compiler-known
behavior.

## Typed Channels

The experimental `std::typed_channel::Channel<T>` library type is a nominal
runtime handle whose payload type is fixed by its generic specialization. Its
`init` and `close` operations return `Result<void, std::error::ErrorCode>`.
`receive()` returns `Result<T, std::error::ErrorCode>`; only success creates T.
`send(move value)` consumes the source and returns
`Result<void, std::typed_channel::SendError<T>>`. Before commit, failure moves
the original value into the error's `value` field alongside its `error` code.
Dropping that error destroys the value; moving it out permits retry. No
output-parameter receive compatibility wrapper is provided. Closing consumes
the channel and is written `(move channel).close()`. Payloads must have
concrete, transferable
Send/Sync and move/drop witnesses; references, slices, raw pointers, function
types, and dependent types are rejected. The low-level prepare/commit/cancel
token operations are compiler-internal and are not source syntax. `Channel<T>`
has a compiler-owned drop operation that closes a live runtime handle when an
explicit close was not performed.

The compiler internally normalizes Result, task, channel, foreign completion,
and read projections to one outcome protocol with one-shot and multi-submit
cardinality. This is an implementation and artifact fact only: v1 admits no
`Outcome<T>` source type, outcome operator, or alternate spelling for `Result`.

Wave B completes the internal ownership consumer migration without extending
this surface. Result/task terminal facts, typed-channel commit/cancel effects,
and foreign read projections are verified through the compiler-owned outcome
protocol; no new source spelling, recovery operator, lifetime annotation, or
ABI contract is admitted by that migration.

## Cross-Cut Semantic Closure

Concrete `const` and `const fn` evaluation uses one typed, deterministic,
budgeted evaluator locally and through verified public artifacts. Readonly
static loads remain address-bearing storage rather than constant aliases.

Every admitted expression evaluates observable operands left-to-right exactly
once. Named argument and aggregate field rebinding occurs only after source-
order evaluation; conditional and short-circuit forms skip unselected work.
Constructor calls, compound assignment, `defer`, full-expression cleanup, and
exact-error residual flow use that same order.

Expected local, return, call, control-result, function-value, and raw-pointer
types flow only into expression forms that admit context. Otherwise control
arms and nonempty array elements choose one symmetric common type with `never`
arms excluded. Source order is never a type-selection tie breaker.

Public callable entity identity, source-overload identity, and concrete native
ABI identity are distinct. V1 closes these rules for its admitted callable
families; later aggregate or environment-bearing callable forms must supply
their own conformance evidence.

## Frozen Authority

The generated [surface table](v1-surface.generated.md) is the authoritative
projection of v1 scope, design status, implementation status, and executable
evidence. Chtholly v1 is frozen: every v1 entry is normative and complete, and
the manifest has no remaining closure wave. Post-v1 entries reserve no syntax
or behavior in source version 1.0.

## Diagnostic Recovery

Invalid source must not extend the accepted language. The parser emits stable
typed diagnostics and explicit error nodes, guarantees forward progress after
every failed declaration or statement, and synchronizes at declaration,
statement, and comma-separated list boundaries. Synchronization is aware of
nested parentheses, brackets, and braces and must not consume an unmatched
outer closing delimiter.

An error is contained by the smallest recovered subtree. A later valid
declaration remains a clean subtree and is available to diagnostics and
tooling. Lexical errors still produce a verified recovery tree, but any lexical
or parse error prevents semantic analysis, artifact publication, and code
generation. Recovery never assigns semantics to an error node.

## Non-capturing Callables

`fn(T...): U` names an ordinary non-capturing callable type. Its value is an
immutable, zero-environment function reference with pointer representation and
trivial copy semantics. A safe concrete ordinary function may form this value
implicitly. This includes associated functions selected through a nominal type.

Generic templates do not form values before concrete specialization. Instance
methods require receiver state and therefore do not form zero-environment
callables. Unsafe functions do not implicitly acquire or lose authority through
this type. Foreign ABI callable types are not a Chtholly source form.
Environment-bearing closure syntax and representation are
not part of the implemented baseline and are post-v1 design work.

## Versioned Extensions

Language 1.1 admits public asynchronous execution. Next accepts `pub? async fn`
for non-generic free functions, publishes its hidden constructor identity, and
implements eager async calls, inferred move-only Task locals, prefix
`wait task`, `check cancel;`, path-sensitive Task obligations, fallible terminal
lowering, the asynchronous `main(): i32` entry identity, and contextual
`task scope { ... }`. A task scope implicitly owns success-only children,
drains them before lexical cleanup on every exit, and cancels them before error
or cancellation propagation. Fallible children still require `wait`; Task
handles cannot escape the group. `async`, `wait`, `check`, `cancel`, `task`,
and `scope` are contextual spellings and do not prevent existing APIs from
using those names. Detach, group handles, manual cancellation, and manual
executors remain unavailable.
Nested task scopes quiesce from inner to outer. A child cancellation observed
by a normal drain makes owner cancellation sticky; child cancellation observed
while cleaning an already-selected error does not replace that error. An
independent owner or ancestor request still wins before terminal commit.
Cancellation acknowledgement cannot clear, recover, shield, or produce an
error payload.
The hosted task runtime can register and release absolute monotonic deadlines,
and frame plan v11 fixes their inheritance and cancellation precedence. This
is an internal execution primitive only. The separately implemented
`std::time` module exposes ordinary `Duration` and opaque monotonic `Instant`
values plus a fallible clock query through existing language constructs. It
adds no unit suffix, time operator, timeout expression, deadline statement or
handle, or expiry diagnostic; task outcomes remain success, error, or
cancelled.
The asynchronous entry identity has native success,
handled-failure, and cancelled-process execution evidence. It remains a
versioned extension and does not change source version 1.0's synchronous
hosted program model. The ownership and exit matrix is recorded in
`docs/design/next-lexical-task-scope.md` and
`docs/design/next-structured-cancellation-composition.md`.
Internal task, callback, witness, or coroutine operations do not by themselves
admit corresponding application syntax. Static interfaces are part of the
frozen surface; dynamic interface values and environment-bearing callables are
governed by their versioned specifications rather than inferred from IR support.

Language 1.2 admits environment-bearing closures, callable interfaces, and
bound methods. Language 1.3 admits operator protocols, module aliases, and loop
labels. Language 1.4 admits `foreach`. Each extension is unavailable to earlier
source versions and has an explicit version diagnostic or standard-library
module gate.

Language 1.9 adds ordinary `Collection` and `Collector` library protocols plus
mutable callable iterator adapters. It adds no grammar. Projection-sensitive
carrier loans ensure a mutable continuation cannot be invoked while its prior
borrowed Item remains live.
