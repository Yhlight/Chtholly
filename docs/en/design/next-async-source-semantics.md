# Next Async Source Semantic Contract

Status: implemented for declarations, eager child creation, inferred Task
ownership, `wait`, explicit cancellation checks, terminal outcomes, and the
async entry identity, and explicit lexical `task scope`. Detach, group handles,
manual cancellation, and executor source syntax remain outside this milestone.

## Declarations and identity

An asynchronous free function is introduced by `async fn`. In the first
source milestone, `async` is permitted after an optional `pub` and before
`fn`. Methods, generic functions, constant functions, and foreign functions
cannot be asynchronous.

The four source words introduced by this design are contextual. In particular,
`wait` remains a valid declaration and qualified member name outside the
prefix-expression position, preserving existing foreign resource APIs.

The declared return type remains the type checked inside the source body. A
return type other than canonical `std::result::Result<T, E>` gives the async
entity success type `T` and no error type. A canonical `Result<T, E>` gives the
entity success type `T` and error type `E`. Parameter types, success type,
optional error type, and asynchronous execution kind form the declaration
identity. Declaration merging and overload selection compare that identity
before argument conversion.

The public entity and its compiler-owned scaffold are distinct identities.
Only the epoch-1 hidden constructor contract is published. The scaffold is
never an ordinary callable symbol. The public callable contract remains
unchanged. The current `CHNXTPK63` package schema additionally persists module
link dependencies and foreign symbol requirements, the artifact store uses
`next-v32`, and task runtime ABI v1 is unchanged.

## Task values and structured ownership

Calling an async entity is valid only in an async body. Arguments are evaluated
left to right and exactly once, and child creation starts eagerly. The result
is a compiler-owned, move-only `Task<T, E?>`. Task types can be inferred for
locals but cannot be written in source or used in parameters, returns, public
interfaces, or aggregate storage. Moving a task between inferred locals moves
its outstanding completion obligation.

Each async invocation participates in the current structured cancellation
ancestry. `task scope { ... }` adds an explicit lexical group without exposing
a handle. Success-only calls may transfer their obligation to that group;
fallible calls still require `wait`. Normal exits close and drain, error exits
cancel and drain before preserving the error, and cancellation exits cancel
and drain before continuing cancellation. Group drain precedes `defer` and
local destruction. See [next-lexical-task-scope.md](next-lexical-task-scope.md).

`wait task` consumes one task. It produces `T` for a success-only task and the
canonical `Result<T, E>` for a fallible task. The result is an ordinary
temporary value and follows the existing full-expression, materialization,
and lifetime-extension rules. Waiting twice, using a task after it is waited,
or discarding an async call directly is invalid.

Every reachable non-cancellation edge outside `task scope` must have consumed
the task with `wait`. Inside a task scope, only success-only tasks may transfer
that obligation to the group. Task provenance follows moves and assignments
and cannot escape through a return, external place, or outer local.

## Suspension and cancellation

`wait` is a prefix expression. It accepts an owned task temporary or consumes
a task place. Task completion is armed nonblockingly; an already-ready task and
a task that suspends converge on the same typed outcome refinement. Success
moves the result, failure moves the error and constructs canonical `Result`,
and cancellation enters the current scaffold's cancellation terminal edge.
Cancellation never becomes an `E` value.

`check cancel;` is an explicit cancellation point. Both `wait` and
`check cancel` are restricted to async bodies and are forbidden in `defer` and
constant evaluation. Cancellation scope handles, manual cancellation,
detached tasks, and executor switching have no source spelling in this
milestone.

References cannot be task outcomes and cannot remain live across a possible
suspension. Diagnostics are issued at the source suspension boundary rather
than reported later as invalid LowIR.

Compiler/runtime protocol failures while creating or arming a task are not
declared errors and are not cancellation. They enter the compiler's
deterministic runtime-fault path. The same rule covers join, query, payload
take, and an impossible terminal refinement. Runtime ABI
`chtholly_next_runtime_v1_trap_coroutine(reason)` owns the stable reason range
1 through
8; reason 8 is the lexical-group protocol failure. This does not widen the
task runtime ABI.

## Terminal lowering

Source returns do not survive as ordinary function returns in a scaffold.
Success-only functions end in `CoroutineReturnSuccess`. A canonical Result
return is refined into explicit success and error payload branches ending in
`CoroutineReturnSuccess` or `CoroutineReturnError`. Child cancellation ends in
`CoroutineReturnCancelled` and is never converted to the declared error type.
`void` success or error channels carry their state without frame payload
storage or a runtime move callback.

All three terminals take structured cleanup snapshots. Success and error edges
require every Task obligation to be fulfilled. A cancellation terminal may
transfer outstanding children to the cancelling runtime scope. LowIR frame
plan v10 reconstructs distinct success, error, and cancelled region edges;
normal, selected-error, and selected-cancellation task-group checkpoints; and
typed cancellation acknowledgement facts. It rejects an ordinary return
inside a scaffold.

## Entry point

The only asynchronous entry signature in this milestone is
`async fn main(): i32`. The compiler marks its scaffold as the coroutine
execution entry and generates the existing internal root task driver. A
synchronous function cannot create or wait for a task. Business failures must
be handled inside `main`; cancellation and task protocol failures retain their
reserved process exit codes.

Native end-to-end coverage compiles source `async fn main()` through every
compiler phase, emits and links its object, and executes successful child,
handled child-error, and root-cancellation paths. Executable emission
authorizes only the generated root runner for an async entry; the source
scaffold is not a second host entry candidate. The installed runtime supplies
the versioned hosted adapter described by
[`async-and-hosted-program-model.md`](../spec/async-and-hosted-program-model.md).
Test-only cancellation injection remains outside that production ABI.

## Diagnostic boundary

The frontend diagnoses unsupported async declaration forms, invalid async
entry signatures, async calls outside async bodies, invalid `wait` operands,
suspension or cancellation checks in `defer`, task discard or escape,
unconsumed task obligations on non-cancellation edges, and references live
across suspension. Rust-derived `await`, `.await`, and `Future` spellings are
not accepted and are not retained as rejection fixtures.
