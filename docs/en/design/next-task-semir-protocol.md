# Next Source-Independent Task SemIR Protocol

Status: implemented for local and imported compiler-owned coroutine
scaffolds. No source syntax or public task constructor is defined.

## Typed protocol

SemIR owns compiler-only `CoroutineScope`, `CoroutineTask<T, E?>`,
`CoroutineTaskOutcome<T, E?>`, and `CoroutineChecked<T>` identities. A task is
an owned move-only handle whose ordinary cleanup releases the runtime task.
Scopes are borrowed host authority. Outcomes distinguish pending, completed,
failed, and cancelled; cancellation has no payload.

`CoroutineTaskCreate` borrows an explicit scope in an internal task driver.
`CoroutineChildTaskCreate` is valid only in a running coroutine scaffold and
uses the current runtime task, preserving its current executor and original
cancellation scope. Both operations evaluate ordered arguments once and
eagerly create the task. A local scaffold is explicitly bound to its canonical
public async entity; imported targets carry that canonical entity directly and
never infer identity from a source name or process-local `FunctionId`.

`CoroutineTaskJoin`, `CoroutineTaskQuery`, `CoroutineTaskTakeResult`, and
`CoroutineTaskTakeError` borrow the owned task. Blocking join is restricted to
the single compiler-only `(CoroutineScope) -> i32` task driver and cannot
appear in a coroutine scaffold. This exact driver ABI is verified before LLVM
builds its zero-parameter host wrapper.
`CoroutineCheckedStatus` exposes runtime status, while
`CoroutineCheckedTake` extracts a conditionally initialized value only in a
structured `status == 0` branch. Query requires a successful join. Result and
error movement additionally require completed and failed outcome refinements,
respectively. Cancelled paths cannot move either payload.

`CoroutineTaskCompletionArm` nonblockingly observes a task and returns a
checked owned `CoroutineTaskCompletion`. A successful `ARMED` disposition
means the runtime owns the wake callback and its retained task context; a
successful `READY` disposition keeps callback ownership with the caller.
`CoroutineTaskCompletionReady` is a non-consuming probe. A pending completion
is moved into `CoroutineSuspend`, which records a distinct task-completion
suspension kind. `CoroutineTaskCompletionDetach` consumes without waiting, and
ordinary completion cleanup releases the opaque runtime completion owner.
Structured success refinement and exactly-one take apply to completion handles
just as they do to task creation.

## Completion composition

SemIR additionally owns compiler-only homogeneous completion-set and selection
identities parameterized by `(provider completion type, N)`. The historical
`CoroutineTaskCompletionSet<N>` and `CoroutineTaskSelection<N>` names remain an
internal compatibility surface, but their identity may now carry either the
task-v1 completion provider or one canonical foreign resource's
`Resource::Completion`. `CoroutineTaskCompletionSetCreate`
accepts exactly `N` explicitly moved completion tokens and produces a checked
set owner because compiler-private heap allocation can fail. Status `-2602`
denotes allocation failure; that path releases every token already transferred
to the operation. Physical storage is an opaque pointer to ordered
provider-value `completion[N]` elements plus two `ceil(N / 64)` bitmaps.
`active` records dynamic membership; `armed` records transferred wake
authority. Both have no 62-slot or single-word limit. `N` and the provider
owner remain static type facts.

A foreign provider is accepted only when its completion storage has the full
epoch-14 ready, arm, detach, and wake-cleanup capability. All operands must
have exactly the same `Resource::Completion` identity; layout-compatible
resources cannot be mixed. Initial suspension arms every `active && !armed`
foreign completion with retained parent-task wake authority and then probes
again. A transferred arm sets `armed`; an already-ready arm releases the
temporary retain and leaves it clear. Resume only probes. Ready completion,
detach, and cleanup clear both bits. Race and cancellation detach pending
foreign members nonblockingly; wait-all and normal owner destruction use the
declared completion cleanup.

Three consuming operations share canonical operand order `0..N-1`:

- `CoroutineTaskCompletionWaitAll` consumes every completion. An empty set
  completes immediately. Ready elements are released and removed on each
  observation; pending elements remain armed across suspension.
- `CoroutineTaskCompletionSelect` returns checked
  `CoroutineTaskSelection<N>`. The lowest canonical ready index wins, its
  completion is released, and the selection transfers the same owner holding
  both membership and arm state for every remaining element. A later wait or
  selection therefore cannot register an already-armed completion again.
  `CoroutineTaskSelectionWinner` observes the
  index and `CoroutineTaskSelectionTakeRemaining` moves the remaining set at
  most once. Selection cleanup releases it when it is not taken.
- `CoroutineTaskCompletionRace` returns a checked winner index using the same
  lowest-index rule, then nonblockingly releases every remaining active
  completion and the set owner.

Empty select and race return checked `INVALID_ARGUMENT` status `-2601`
synchronously and never become suspension states. Nonempty operations reuse
the existing completion wake callback, with all callbacks targeting the same
parent task and the scheduler's existing rerun-debt coalescing. No new runtime
ABI entry is required.

## LowIR and LLVM

LowIR owns one immutable `CoroutineTaskCreatePlan` per creation instruction.
The plan records the typed target, canonical constructor entity, constructor
ABI epoch, optional local scaffold, task identity, root/child mode, and ordered
parameter types. `CoroutineTaskCompletionArmPlan` independently freezes the
epoch-1 wake contract. Phase-end verification checks unique plan ownership and
reconstructs both instruction contracts from SemIR and the public registry.

Completion composition adds uniquely owned immutable
`CoroutineTaskCompletionSetPlan` and
`CoroutineTaskCompletionCombinePlan` values. They freeze operation kind,
operand count, bitmap word count, canonical order, winner and loser policies,
semantic suspension, continuation, and ABI epoch 1. Continuation binding is a
single invalid-to-valid lowering transition; only nonempty combinations may
bind one. Phase-end verification reconstructs every field, rejects unowned or
duplicate plans, and proves empty combinations did not enter frame-state
reconstruction. LowIR printing and metrics expose both plan stores. Each plan
also freezes a provider descriptor containing the completion type, foreign
nominal owner and protocol identity, and the verified epoch-14 wake plan. LLVM
uses the descriptor for physical element layout and provider-specific probe,
finish, arm, and detach operations; it no longer assumes every set element is
a task-runtime pointer.

LLVM names the epoch-1 constructor
`__chtholly_next_coro_ctor_v1_<full-public-entity-fingerprint>`. A producer
emits a hidden definition; an imported consumer reconstructs the signature
from the public async entity and emits the same hidden declaration without a
local scaffold. The physical ABI is authority pointer, ordered source
arguments, out-task pointer, child-mode `i1`, and `i32` status. The constructor
initializes the frame and selects runtime `task_create` or `task_create_child`.
Lowering never searches for a source-derived symbol or infers task identity
from LLVM types. Checked values use status plus conditional output storage, so
failure paths never contain a poison payload. Lexical task cleanup lowers to
`task_release` on every structured exit.

Runtime ABI v1 adds an opaque completion and nonblocking `completion_arm`,
`completion_ready`, `completion_detach`, and `completion_release` entries.
Terminal publication and waiter changes linearize under the task mutex, but
wake and release callbacks always run after unlocking. Publication or detach
releases transferred callback authority exactly once. Internal/external
completion references and a retained observed task prevent races and use after
free.