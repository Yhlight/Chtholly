# Chtholly Failure Lifecycle And Storage

Status: normative for Result, unrecoverable failure, and the v1 storage-duration
set. Allocation and the program lifecycle remain owned by their separate
roadmap entries.

## Result Flow

The only compiler-known result type is the nominal
`std::result::Result<T, E>` declaration. Its standard-library definition is a
payload enum with `Ok { T }` and `Err { E }` variants. An identically shaped or
named user type has no result behavior.

Postfix `?` evaluates its operand exactly once and requires that operand to
have canonical Result type. `Ok` moves the success payload into the enclosing
expression. `Err` moves the error payload into a canonical Result whose error
type is exactly identical to the enclosing function's error type, commits that
value to the function result slot, and then takes the ordinary return cleanup
edge. There is no implicit error conversion and no user-programmable residual
protocol in v1.

Compiler-owned callable summaries may attach a bounded condition to an
inferred postcondition. This is not an additional source contract form:
`ensures initialized` and `ensures invalidated` remain unconditional source
declarations. The inferred condition records the control-flow paths on which
the fact holds, so a guarded write is not widened to an unconditional live
object when another path preserves the prior state. Conditions are canonical
and survive generic/imported artifact replay; over-complex conditions widen
conservatively to `always`.

Recovery is expressed with existing `switch`/`if` syntax. A Result arm moves
only its selected payload and applies ordinary lexical cleanup independently.
Result discriminant predicates are not yet part of the persisted callable
condition vocabulary, and cancellation remains a separate task terminal.

Nested propagation and propagation during partial initialization use the same
rule: only live, initialized places are cleaned, in reverse registration order,
after the enclosing result slot has been committed. Operand and payload
temporaries obey ordinary full-expression registration and transfer. A
borrowed success payload retains its ordinary provenance; neither `?` nor
Result construction extends a referent lifetime, and a reference into a
temporary cannot escape by return. Local, generic, imported, artifact-only,
and native code have identical behavior.

## Construction And Cleanup

Construction transitions valid uninitialized storage to a live object only
after all required subobjects are initialized. `T::init(args)` may return
`Self` or canonical `Result<Self, E>`. `T::init(args) in destination` places
the result into a mutable local destination that has already been moved. The
direct form returns `void`; fallible placement returns `Result<void, E>`.
`Ok` commits the destination and transfers one cleanup obligation. `Err`
leaves the destination moved and non-live. Constructor-owned locals clean up
in reverse order on both returns. Destruction applies exactly once to a live
object and cannot report recoverable failure. `drop(self: Self&)` is a
compiler-only lifecycle role and is never an ordinary user call.

Deferred actions register lexically and share one strict LIFO timeline with
owned-local destruction. They run on normal, return, Result residual, loop,
and constructor exits. Captured places are read when cleanup executes, after a
return value has been transferred to its result slot. A defer body must fall
through and cannot escape or register another defer. They supplement resource
lifecycle but do not replace nominal destruction. Unrecoverable termination
and coroutine suspension do not run defer in this version.

## Unrecoverable Failure

Recoverable failure is typed Result value flow. Unrecoverable failure covers
failed assertions, executed unreachable assertions, bounds violations, failed
mandatory checks, and arithmetic traps. It is process termination, not an
exception, Result residual, coroutine cancellation, or recoverable task
failure.

The source statements are:

```cns
assert condition;
unreachable;
```

`assert` evaluates `condition` exactly once and requires `bool`. A true
condition completes its full-expression normally. A false condition terminates
immediately. Executing `unreachable;` also terminates immediately; it is a
checked runtime assertion and does not grant the optimizer an unverified source
assumption.

Termination performs no full-expression temporary cleanup, local destruction,
`defer`, static destruction, Result conversion, task cancellation, or child
join. It never unwinds through a Chtholly, C, or component boundary. `assert`
is permitted in a defer body and terminates immediately on failure;
`unreachable;` is rejected there by the existing requirement that every defer
body fall through. V1 does not make a message or source location part of the
runtime contract.

The runtime ABI operation `chtholly_next_runtime_v1_trap_failure(reason)` is
`noreturn`,
`nounwind`, and cold. Reason `1` is assertion failure and reason `2` is an
executed unreachable assertion. Lowering emits the call followed by an IR
unreachable terminator. This terminal operation is distinct from a compiler-
proven unreachable block, which may lower directly to optimizer unreachable.

During constant evaluation, a true assertion continues. A false assertion or
executed unreachable assertion is ill-formed with a stable fatal-evaluation
diagnostic; constant evaluation never invokes the runtime operation.

## Storage And Allocation

The storage review distinguishes automatic, static, thread, external, and
dynamically allocated storage. Constants are compile-time values, not a synonym
for runtime static objects. Heap allocation belongs to explicit owner and
allocator APIs; `T::init(args) in destination` performs no allocation and uses
caller-provided success storage. Fallible construction additionally uses a
caller-provided `Result<void, E>` outcome slot. Placement verifies local
storage authority, mutability, moved state, failure cleanup, and deallocation
responsibility. General raw-buffer placement and dynamic allocation remain
separate work.

V1 admits only the existing readonly `static` subset. Each declaration is at
module scope, explicitly typed, constant-initialized, and denotes one immutable
object with a stable process-lifetime address. It performs no run-time
initialization or destruction and therefore has no initialization dependency
graph, guard, unload action, or connection to runtime program/thread lifecycle
machinery. Its source, ABI, and supported object-encoding rules are in
`constant-evaluation-and-layout.md`.

Automatic objects retain lexical lifetime. Compile-time constants remain
values without runtime storage. Foreign storage remains governed by its
declared external ownership contract. Mutable statics, thread-local statics,
dynamically initialized statics, and destructible statics are post-v1 and have
no admitted source form. Enum, union, and custom-carrier static packing remains
owned by the representation-artifact roadmap entry rather than this storage
entry.
