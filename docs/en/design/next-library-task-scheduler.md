# Next Library Task And Scheduler State Machine

Status: implemented as a multi-module Next reference library above the frozen
callback ABI epochs 10, 12, 13, and 14.

## Library Surface

The implemented shape is:

```cns
lifecycle(copy = delete, move = default, drop = default)
pub struct Task<T> { /* private cancellation, identity, and result */ }

lifecycle(copy = delete, move = default, drop = default)
pub struct TaskOutcome<T> { /* private cancelled flag and result */ }

pub struct SchedulerStep { /* private state tag */ }

lifecycle(copy = delete, move = default, drop = default)
pub struct CancellationScope { /* private registration and identity */ }

pub fn resume<T>(value: const Task<T>&): SchedulerStep;
pub fn request_cancel<T>(value: const Task<T>&): void;
pub fn join<T>(value: Task<T>): TaskOutcome<T>;
pub fn request_scope_cancel(value: const CancellationScope&): void;
pub fn join_scope(value: CancellationScope): void;
```

`SchedulerStep` exposes mutually exclusive `step_completed`,
`step_cancelled`, `step_suspended`, and `step_reschedule` observations.
`TaskOutcome<T>` exposes cancellation observation and consuming result access;
its physical tag and result remain private.

This query-based result is an intentional frozen reference encoding, not the
final preferred surface. Next now implements payload enums and exhaustive
switch patterns, so a later library-only migration can use
`SchedulerStep<T>` and `TaskOutcome<T>` payload enums. The runtime state
machine and ownership boundary do not depend on that migration.

A general task constructor is not frozen. Provider adapters may construct a
concrete task only after registering it with a scope. This avoids exposing a
manual poll driver or pretending that an ordinary once-callable is a
coroutine.

## Ownership And Cancellation

`Task<T>` owns one private epoch-14 completion/cancellation value. Dropping a
task reaches the existing epoch-12 finish path and may block. A scope owns its
private epoch-10 registration; dropping it uses quiescent unregister after its
children have resolved. `join` and `join_scope` consume their owners. Runtime
detach remains an unsafe implementation detail and is not available through
the safe facade.

Cancellation requests are nonblocking and sticky. A scope request reaches all
existing children, and a child attached after the request observes cancellation
immediately. Propagation is downward only: a child cancellation does not
implicitly cancel its parent or siblings. The scope cannot finish until child
release obligations are resolved by completion, join, blocking cleanup, or a
trusted runtime handoff.

## Wake Coalescing

Each stable task identity has one scheduler state and one rerun debt:

```text
Idle -> Queued -> Running -> Idle / Queued / Terminal
```

- `Idle -> Queued` publishes one enqueue.
- repeated wakes while `Queued` do not enqueue again;
- claim is the only operation that changes `Queued` to `Running`;
- a wake while `Running` records one rerun debt;
- suspension with rerun debt returns `Reschedule` and republishes one enqueue;
- suspension without debt returns `Suspended` and changes to `Idle`;
- completion or cancellation changes to `Terminal`.

Cancellation sets a sticky terminal observation before using the same wake
transition. Coalescing therefore cannot suppress cancellation progress: an
already queued task observes cancellation when claimed, and a running task
retains a rerun debt.

The transition must be atomic in the runtime implementation. The foreign
helper signature is not proof of atomicity, and it is not a new callback ABI
contract. The Next pipeline test executes a state-machine model covering
duplicate wakes, claim/wake/suspend races, terminal wakes, existing children,
and late child attachment.