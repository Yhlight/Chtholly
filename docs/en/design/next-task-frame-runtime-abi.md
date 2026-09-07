# Next Task Frame Runtime ABI And Coroutine Scaffold

Status: runtime ABI v1, source async execution, and compiler coroutine path
implemented. Internal frame plans use version 11. Task remains an inferred,
compiler-owned type with no public constructor.

## Boundary

The Next task runtime is an independent C ABI in
`runtime/include/chtholly/next_task_v1.h`. It does not expose compiler state.
Standard-library
format 4 uses compiler-contract epoch 5 and library API epoch 3. Package state
is `CHNXTPK63` version 62 and the artifact cache is `next-v32`. Coroutine
cleanup graphs and lexical task-group states remain internal compiler planning
data; module dependencies and foreign symbol requirements are the persisted
link boundary.

The ABI has four opaque owners: executor, cancellation scope, task, and
completion. A
compiler-produced frame is described by stable `resume`, `destroy`, optional
`move_result`, and append-only optional `move_error` entries. `task_create`
consumes the frame only on success, copies the descriptor, attaches the child
to its scope, and queues it immediately. The runtime calls `destroy` exactly
once when the last task reference is released. Success and error payloads can
each be moved exactly once, and only from their corresponding terminal state.
An old v1 descriptor prefix remains valid; a `struct_size` ending inside the
appended `move_error` field treats that field as absent.

## Scheduler And Cancellation

The scheduler owns a fixed worker pool and the transition:

```text
Idle -> Queued -> Running -> Idle / Queued / Cancelled / Completed / Failed
```

Queued wakes coalesce. A wake while running records one rerun debt. Sticky
cancellation uses the same wake path, so an idle suspended task reaches a
checkpoint and a running task cannot lose cancellation progress. Scope
cancellation reaches existing children and is observed immediately by a child
attached later. Public scope release requests cancellation and joins; worker
paths only drop internal references and never block on scope completion.

`resume` returns suspended, reschedule, cancelled, completed, or failed.
Unknown values fail closed as cancelled. Terminal publication precedes scope
removal. A late wake holds the task publication lock while acquiring the
executor, so scope detachment cannot invalidate the executor during
scheduling.

Every task retains its current executor independently of the cancellation
scope. `task_rebind_executor` is valid only from a running continuation and
persistently changes future wake and resume scheduling; the continuation then
returns reschedule. The scope also retains every executor used by its attached
tasks until structured join completes. Terminal publication transfers the
task's executor lifetime back to that scope before detachment, so dropping a
detached task and its executor handles cannot make a worker destroy its own
executor. `task_create_child` eagerly creates a child with the running parent's
current executor and original cancellation scope, so a child created after a
switch inherits the switch without changing cancellation ownership.

Every child also joins an implicit cancellation ancestry rooted at its parent.
Requesting cancellation of a task marks and wakes all attached descendants.
Creation and propagation use the scope mutex as their linearization boundary,
so a racing child is either visited or observes the parent's sticky request.
The ancestry node is separate from the task object: descendants retain only
that node, not the parent's frame, result, or error payload. Propagation is
downward only and survives executor rebinding. This strengthens runtime v1
behavior without changing any exported signature or structure layout.

## Typed Frame Plan

Source-independent `CoroutineSuspend`, `CoroutineCancellationCheck`, and
`CoroutineExecutorSwitch` instructions carry coroutine intent in SemIR. They
are accepted only inside a non-public, non-template, non-specific async
function marked `SemFunctionCoroutineScaffold`; the phase-end verifier also
requires unique function-body ownership. The function must not be called
through an ordinary `Call` instruction. Parser and source semantic
construction never produce these operations or set the scaffold flag.

LowIR owns `CoroutineFramePlan` values in a typed store. Lowering first orders
markers by canonical LowIR instruction order, splits immutable LowIR blocks
into `[begin, end)` segments after every suspension, and gives every segment a
typed single-entry region. Region edges explicitly distinguish ordinary
control flow, suspension, and return. Deterministic worklists compute backward
SSA/root-place liveness and forward may-owned root places. Each plan records:

- function, success type, and optional error type;
- lifted slots, typed addressable frame places, and one initialization-bit
  index per logical place;
- canonical segments, their unique region ownership, and verified region
  live-in/live-out facts;
- explicit frame SSA values, lifted root slots, and global/per-state cleanup
  order derived from cross-edge liveness and may-owned facts;
- sequential resume states, suspension/continuation region identities, and
  semantic/LowIR suspension identities;
- live SSA values and live root places for each suspension edge;
- the verified callback wake ABI plan and lifted wake slot for every state;
- detached `pre_commit` and transferred cleanup graphs for each state, plus
  edge-specific cleanup graphs for explicit cancellation and executor switch;
- frozen evaluation, eager-creation, executor inheritance/switch, terminal
  precedence, and cancellation-point policies;
- internal execution-entry authorization and frame-plan version.

The verifier reconstructs segment use/def sets, runs the complete region
liveness fixed point independently, and checks every region edge against the
underlying LowIR branch target or suspension continuation. It rejects incomplete or
multiply-owned partitions, duplicate functions or states, invalid and
unreachable markers, waits that do not consume a callback wake,
ordinary/public/called scaffolds, reference-typed results, and references live
across a suspension.
The latter diagnostic is path-specific: it reports the borrow or reference
origin, the suspension instruction, and a reachable later use. References that
end before every suspension are not lifted or rejected. Frame-plan v7 assigns
contiguous typed IDs and bits to stable root and projected places. LLVM stores
them in `[ceil(frame_places / 64) x i64]`; result and error ownership use a
separate terminal byte. Moving a projected place clears its ancestors and
descendants while preserving siblings, and cleanup walks root-first while
clearing descendants after an owner is destroyed. There is no root-only or
62-slot limit. Void values and references are excluded only when projecting
complete region liveness into persistable frame state; retaining them in
region facts preserves analysis precision while preventing impossible LLVM
frame fields. Dereference projections remain ineligible because their
addresses are not stable frame-relative paths.

Each cleanup graph owns an explicit entry, detached block list, semantic
origin, and graph-local slots. The verifier requires every graph to have one
frame-plan owner, remain inside one scaffold, terminate through
`CoroutineCleanupEnd`, branch only within the graph, and close over only its
own SSA. Graph-local slots cannot also be lifted. A transferred graph may
capture outer slots; those slots become frame-owned unless their type is a
reference, which is rejected before LLVM lowering.

## LLVM Scaffold

Each verified frame plan emits an internal frame type, `resume`, `destroy`,
`move_result`, and optional `move_error` thunks, plus a read-only ABI descriptor
and an epoch-1 hidden status/out-task constructor keyed by the full canonical
public-entity fingerprint. Local scaffolds explicitly bind that entity;
imported creation plans carry the entity and no local scaffold. State 0 enters
the real initial CFG.
A marked wait uses the continuation region already named by the verified
plan: pending stores live SSA
values, the active wake, and the next state before returning suspended; a later
resume finishes that wake and branches into that same region. LLVM iterates
the canonical segments and does not rediscover continuation boundaries or
recompute the frame-value union. The
compiler-generated adapter retains the runtime task, wakes it through the v1
runtime entry, and releases exactly one retained reference. Runtime code still
owns rerun and requeue decisions. Every resume reconstructs place
initialization state from the frame bits before entering the continuation, so
cleanup observes the same ownership state that was persisted at suspension.

The frozen cancellation points are function entry, suspension commit,
post-resume, explicit cancellation check, and executor switch. Ordinary calls
and loop backedges are not implicit points. Cancellation observed before a
terminal commit wins; committed success or failure cannot later be replaced by
cancellation. The compiler scaffold emits entry, suspension-commit, and
post-resume checks. An explicit cancellation operation branches to the same
state-aware cleanup. Executor switching calls `task_rebind_executor`, checks
cancellation, and returns `STEP_RESCHEDULE` after a successful persistent
rebind.

At a possible suspension, ordinary temporaries execute through the state's
`pre_commit` graph on both the pending and immediately-ready paths. Lexical
locals, lifetime-extended temporaries, and deferred bodies transfer through
the state's language cleanup graph only when the state commits. Wake authority
is deliberately outside that graph.

Cancellation dispatches on the current state, detaches only that state's
active wake without blocking, destroys saved SSA values in reverse order,
clears the state, and then executes the transferred language cleanup graph.
Clearing before graph entry makes a repeated destroy/cancel observation
exactly once; every graph path must end at its typed cleanup terminator.
Explicit cancellation checks and executor switches use their own static
registration snapshots before entering terminal cancellation. Destroy uses
the same suspended-state chain and additionally destroys an untaken result.
Cleanup is type-specific: callback values use their
completion or release protocol, explicit object lifecycle functions are called
when present, and aggregates are traversed in reverse field order. Constructor
failure retains frame ownership, runs the same initialized-value cleanup, and
deallocates the frame; successful `task_create` consumes it.

Task-completion suspension is separate from callback-wake suspension. The
runtime completion arm either returns ready with caller-owned callback context
or atomically transfers it until terminal publication/detach. The task mutex
linearizes waiter state with publication, while wake and release callbacks run
only after unlock. A completion retains its observed task and uses separate
external/internal references so publication, detach, and release races cannot
free it early. Terminal publication or detach releases transferred authority
exactly once.

Completion sets are compiler-private storage above runtime ABI v1. Their
ordered completion array is paired with `ceil(N / 64)` active words, removing
the former single-word/62-slot design limit without adding a runtime owner or
entry point. Wait-all releases ready members incrementally and suspends while
any active member is pending. Select consumes the lowest ready member and
transfers the remaining owner; race consumes the same canonical winner and
releases all losers nonblockingly. Empty wait-all completes synchronously,
while empty select/race return checked `-2601` and never acquire a frame state.
All zero-capacity combines run any ordinary-temporary pre-commit cleanup
synchronously and transfer no lexical cleanup ownership.
Allocation failure returns checked `-2602` after releasing all transferred
completion owners.

Compiler-only task drivers now exercise the typed SemIR protocol. A thin host
wrapper creates an executor and scope, then the driver performs root creation,
status refinement, join, terminal query, outcome refinement, payload movement,
and lexical task release. A running scaffold also creates and releases a child
task through the same typed protocol. Cancelled tasks never enter payload
movement, and every joined path releases all runtime owners. Native tests link
a controlled C callback provider and execute opposite branch paths through callback
suspension states: one path completes after multiple wake/resume cycles, while
the other
requests runtime cancellation on its second actual arm and exits through the
native cancellation path. The completion path also exercises wake-during-run
rerun debt. The older zero-parameter execution-entry runner remains only as
cancellation-fixture authority while that path is migrated. Neither mechanism
is a public Task constructor or a source-language entry point. See
`next-task-semir-protocol.md`.

The completion path also creates real child tasks for nonempty wait-all,
select, and race. These add verified set-combination suspension states, move a
select remainder owner, release race losers, and preserve the native result of
42. Empty variants execute in the same scaffold without increasing its
suspension-state count.

Ordinary unmarked `callback wait` remains the existing blocking operation.
Source `async fn`, prefix `wait`, `check cancel;`, and `task scope` lower only
through the typed task protocol. No alternate suspension spelling is retained.
