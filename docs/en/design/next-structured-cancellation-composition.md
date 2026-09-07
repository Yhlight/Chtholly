# Next Structured Cancellation Composition

Status: implemented. Deadline expiry now composes with this contract through
the additive runtime work described in
[`next-deadline-cancellation-composition.md`](next-deadline-cancellation-composition.md).

## Boundary

Cancellation remains payload-free, sticky, downward-only, and
non-recoverable. The compiler records cancellation causes only as typed
control-flow facts. They are not source values, task outcome payloads, or
fields in runtime cancellation nodes.

`task scope` remains a structured child-ownership scope. It does not create a
shield, a separately cancellable domain, or a source-visible group handle.
Each async task remains the cancellation domain inherited by its children.

The internal causes are:

- `OwnerRequest`: the current task's sticky cancellation bit is set. Runtime
  v1 intentionally does not distinguish a root-driver request, an ancestor
  request, or an internal owner request.
- `UnexpectedChildCancellation`: a success-only child reaches cancelled while
  its group is completing a normal join-drain.
- `DeadlineExpired`: an active monotonic deadline expires before terminal
  commit. It sets the same owner cancellation bit and is not a public payload.

Cancelling children because an error or cancellation edge was already
selected is a `SelectedExitCleanup` motivation, not an owner cancellation
cause. This distinction prevents cleanup-induced child cancellation from
replacing an error selected before the drain.

## Composition And Precedence

| Selected edge or observation | Child action | Continuation |
| --- | --- | --- |
| normal edge, no cancellation | close and join-drain | preserve the edge |
| normal drain observes an unexpected cancelled child | make owner cancellation sticky | cancel-drain enclosing scopes, then cancel |
| owner request before terminal commit | cancel and drain children | cancel-drain enclosing scopes, then cancel |
| selected error | cancel and drain children | preserve the error |
| independent owner request during error drain | finish all drains | cancel before error commit |
| success or error already committed | none beyond ordinary release | preserve the committed terminal |

An error cancel-drain does not use the group's aggregate child-cancelled bit
to replace its selected error. Runtime v1 cannot distinguish a child that was
cancelled independently from one cancelled by cleanup, so the selected error
wins over either child observation. An independent owner request remains
observable through the task's sticky cancellation bit and wins until terminal
commit.

Nested scopes quiesce from inner to outer. An inner drain may observe or
escalate cancellation, but it cannot acknowledge the task's cancelled
terminal while an enclosing group is live. It first makes owner cancellation
sticky and resumes the structured continuation. The continuation cancel-drains
each enclosing group. Only the outermost drain branches to language cleanup
and the cancelled terminal.

Cancellation acknowledgement is one-way. It cannot clear the sticky request,
resume ordinary execution, convert cancellation to a declared error, or
shield descendants from an ancestor request.

## Compiler Representation

Place-state task-scope edges record an exit intent and cleanup depth. LowIR
frame plan v11 replaces task-group cancellation booleans with typed policies:

- exit intent: normal, selected error, or selected cancellation;
- child policy: escalate an unexpected cancellation or ignore child
  cancellation caused while cleaning a selected exit;
- acknowledgement: acknowledge at this drain or defer to an enclosing scope.

The LowIR verifier reconstructs these values from the task-group drain
instruction, active group values, coroutine regions, and semantic origin.
LLVM consumes the verified policy and does not infer intent from adjacent
instructions. Runtime task/group ABI v1 and frame constructor ABI epoch 1 do
not change.

Native evidence covers nested owner cancellation, unexpected child
cancellation promoted through nested normal drains, and selected-error
preservation when a child is cancelled. LowIR tests cover all three drain
instructions and both acknowledgement boundaries. Verifier corruption tests
replace valid cause and acknowledgement policies and require deterministic
rejection before LLVM.

## Diagnostics And Deferred Surface

Expected cancellation is an execution outcome, not a compiler diagnostic.
Source diagnostics remain limited to invalid cancellation contexts, Task
ownership escape, and references crossing a task-scope drain. Invalid compiler
composition facts are rejected by the LowIR verifier. Impossible runtime
group state retains coroutine protocol trap reason 8.

No source spellings are introduced for cancellation scopes, shielding,
handlers, timeouts, detach, manual requests, group handles, or executors.
Unsupported spelling examples are intentionally absent from source fixtures
and diagnostics until a source design is selected.
