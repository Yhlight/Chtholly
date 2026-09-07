# Next Async Semantic Contract

Status: implemented for typed SemIR, LowIR frame plan v11, task runtime ABI v1,
native source task operations, and the source contract in
[next-async-source-semantics.md](next-async-source-semantics.md).

## Type identity and outcomes

An async function is a distinct canonical type. It is not a synchronous
function returning an implicitly constructed task. Its identity contains the
ordered parameter types, success type `T`, and optional error type `E`.
Success-only and success/error functions therefore have different canonical
identities.

Cancellation is not an error payload. Every async task has three independent
terminal outcomes:

- completed, with a success payload when `T` is non-void;
- failed, with an error payload when `E` exists;
- cancelled, with no success or error payload.

Absence of `E` means that the function cannot fail statically. Public entity
fingerprints use their current versioned domains. Package state is
`CHNXTPK63` version 62 and the artifact store uses `next-v32`. Module link
dependencies and foreign symbol requirements are persisted; coroutine cleanup
graphs remain internal LowIR planning data. Older package state fails closed.

## Evaluation and creation

Async call operands are evaluated strictly from left to right and exactly
once. Task creation is eager. A child captures the current cancellation scope
and current executor at creation; there is no implicit global executor.

Child creation also records an implicit parent cancellation relation. A
parent request and child attachment linearize under the runtime scope: an
existing descendant is marked and woken, while a later child inherits the
sticky request. Propagation is transitive and downward only. Cancellation of
a child does not cancel its parent or siblings, and terminal success or error
is not replaced by a later request.

Executor switching persistently rebinds all future wake and resume scheduling.
It does not replace the cancellation scope. A child created after a switch
inherits the switched executor and the original scope.

## Cancellation

The fixed cancellation points are function entry, suspension commit,
post-resume, explicit cancellation check, and executor switch. Ordinary calls
and loop backedges are not implicit cancellation points.

Cancellation observed before terminal commit wins. Once success or error is
committed, later cancellation cannot replace that outcome. LLVM lowering emits
entry, suspension-commit, and post-resume checks for the internal coroutine
scaffold. Typed source-independent SemIR operations now represent suspension,
explicit cancellation checks, and executor switching. They are verified as
internal scaffold-only operations and have no parser spelling.

## Suspension cleanup ownership

Place-state analysis partitions the active cleanup-registration sequence at
each possible suspension. Ordinary full-expression temporaries form the
`pre_commit` partition and are consumed before the edge commits, on both the
pending and immediately-ready paths. Lexical locals, lifetime-extended
temporaries, and deferred bodies form the `transferred` partition. Their
ownership moves to the frame only after suspension commits.

LowIR represents both partitions as detached, typed cleanup graphs. A graph
may own graph-local slots, but those slots never become frame fields. Slots
captured by a transferred graph are lifted explicitly; reference-typed
captures are rejected. Wake authority is not a language cleanup registration
and remains governed by the callback/completion protocol.

Cancellation checks and executor switches snapshot the active registration
sequence into edge-specific cleanup graphs. Suspended-state cancellation
detaches wake authority, destroys saved SSA values, clears the state for
exactly-once dispatch, and then runs the transferred language cleanup graph.
Frame destruction consumes the same graph. A zero-capacity completion combine
is synchronous: it executes only `pre_commit` cleanup inline and transfers no
ownership to a frame.

## Runtime and frame ABI

`CoroutineFramePlan` v11 records pre-commit and transferred
cleanup graphs for resume states plus cleanup graphs for static cancellation
edges, and distinguishes success, error, and cancelled terminal region edges.
Task-group states additionally record normal, selected-error, or
selected-cancellation intent; owner-only or owner-plus-unexpected-child cause
policy; child escalation policy; and whether acknowledgement belongs to this
drain or an enclosing lexical scope. The same plan fixes runtime-monotonic,
absolute-normalized, earliest-active, sticky-cancellation, and first-linearized
deadline composition without adding a task outcome or source operation.
LowIR verifies unique graph ownership, semantic origin, internal CFG and SSA
closure, terminal payload identity, and separation of graph-local and lifted
slots. Frame lifting is reconstructed only from cross-edge liveness,
parameters, wake authority, and explicit transferred cleanup captures;
pre-suspension checked-status storage cannot leak into the frame. The generated
frame still uses an extensible multiword bitmap and owns success and error
payloads independently. Runtime ABI v1 is extended append-compatibly with a
failed step/state, `move_error`, `task_take_error`, persistent executor
rebinding, and child creation on the parent's current executor.

The runtime copies descriptors according to `struct_size`; old v1 descriptors
remain valid, and a size ending inside an appended field treats that field as
absent. Extended task queries likewise accept the original v1 prefix.

## Source-independent task protocol

Compiler-owned SemIR now represents `CoroutineScope`, owned move-only
`CoroutineTask<T, E?>`, payload-free `CoroutineTaskOutcome<T, E?>`, and
conditionally initialized `CoroutineChecked<T>` values. Internal task drivers
may create root tasks, join, query, and move a refined result or error. Running
coroutine scaffolds may create child tasks but cannot block in join. A checked
payload is available only below a structured `status == 0` branch; result and
error movement additionally require completed and failed outcome branches.

LowIR persists one immutable `CoroutineTaskCreatePlan` for each creation. The
plan binds the canonical public constructor entity, epoch 1, optional local
scaffold, task type, root/child authority, and ordered parameter types. The
public epoch-1 contract freezes eager start, left-to-right exactly-once
evaluation, and root/child support. LLVM derives a hidden constructor symbol
from the complete entity fingerprint, represents a checked value as status
plus conditional output storage, and emits lexical task release.

The existing typed distinction between `CoroutineTaskCreate` and
`CoroutineChildTaskCreate`, reconstructed as the LowIR root/child create mode,
is the compiler authority for cancellation attachment. No parallel scope ID is
stored or inferred from source names. Runtime v1 represents ancestry with
separately reference-counted lightweight nodes, so descendant lifetime does
not retain a completed parent's frame or payload storage.

The nonblocking completion protocol arms a retained task wake, distinguishes
already-ready from transferred callback ownership, probes readiness, consumes
the completion at suspension or detach, and releases every terminal authority
exactly once. Runtime task mutexes linearize publication against arm/detach;
callbacks run outside the mutex. See `next-task-semir-protocol.md`.

Compiler-only completion composition forms statically typed sets from moved
tokens and tracks dynamic membership and transferred wake authority in two
unbounded multiword bitmaps.
Wait-all completes only after consuming every active token; select transfers
the remaining owner after the lowest canonical ready index wins; race releases
all losers nonblockingly. Empty wait-all succeeds immediately. Empty select
and race return checked `-2601` without suspension. Set allocation is checked
with `-2602`, and allocation failure releases every transferred token. These
operations add no runtime ABI entry and inherit the parent task's existing
wake/rerun-debt protocol.

## Syntax boundary

Chtholly now accepts non-generic free `async fn` declarations. Their ordered
parameter types, success type, optional canonical Result error type, and async
execution kind form the function identity. Compilation binds every valid
source declaration to an epoch-1 constructor entity before LowIR; private
functions and `main` use compiler-hidden bindings. An async body with no
suspension is a valid eager task and has a zero-resume-state frame plan.

`async`, `wait`, `check`, and `cancel` are contextual spellings rather than
globally reserved tokens. Existing declarations and qualified APIs such as
`Resource::wait` therefore retain their identities. The parser owns typed
`WaitExpr` and `CheckCancelStmt` nodes. Async calls eagerly create child tasks;
`wait` consumes a Task through arm, suspend, join, query, and checked payload
take; `check cancel;` emits an explicit cancellation edge. Task obligations
are path-sensitive and independent from ordinary place cleanup registrations.
Rust-derived spellings are neither accepted nor retained as rejection
fixtures.

Payload enums are now stable across canonical types, SemIR, public artifacts,
ownership, LowIR, LLVM, and native execution. The internal task
creation/join/query/result protocol and its source lowering are implemented.
Nonblocking completion/wake and
the versioned cross-module hidden-constructor ABI are implemented below the
source boundary. The verified library/no-entry policy and native two-object
constructor execution and completion composition are complete.

Concrete-specialization components remain `CHNXSCC32` version 32. Freezing the
source contract changes no public call ABI, task runtime entry, package format,
cache namespace, or public task ABI. Explicit success, error, and cancelled
terminal edges and typed lexical task-group drain states advance the internal
frame plan to v10. Group states own their group value and completion wake;
ordinary states and inner-drain states defer acknowledgement only when an
enclosing group remains live across the edge. Cancellation reasons remain
compiler-internal and do not change the payload-free runtime outcome.
