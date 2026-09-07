# Next Deadline And Cancellation Composition

Status: implementation milestone over task runtime ABI v1 and LowIR frame plan
v11. No source spelling is introduced.

## Boundary

A timeout is an operation that registers a deadline. A deadline is an
absolute instant in the hosted runtime monotonic clock domain. Expiry requests
ordinary sticky task cancellation; it does not add a timeout payload, a
declared error, or a fourth task outcome. `DeadlineExpired` is an internal
cause used by compiler verification and runtime tests only.

The hosted clock authority is `chtholly_next_runtime_v1_monotonic_now`. Its
origin is
arbitrary and cannot be converted to civil or Unix time. A deadline is stored
as normalized seconds and nanoseconds, with nanoseconds less than one billion.
Absence is represented separately rather than by a sentinel instant. Relative
timeouts are converted to an absolute instant exactly once when registered.
Zero expires immediately; overflowing addition saturates at the greatest
normalized instant.

Service contexts use the same clock and provide useful saturating-arithmetic
precedent, but their handles, cancellation reasons, and wait ABI remain a
separate domain. Next tasks do not retain or query a service context.

## Inheritance And Ownership

Every active registration contributes one absolute deadline to the current
task cancellation domain. The effective deadline is the earliest active local
or inherited deadline. A local registration can tighten but cannot extend an
outer deadline. Releasing an inner registration restores the remaining outer
minimum.

Children created while a registration is active inherit its cancellation
ancestry. A child attached after expiry observes the already-sticky request.
The compiler must keep the registration alive until all structured children
created under it are drained. Detach is unavailable, so no child can outlive
that ownership boundary. Executor rebinding preserves the absolute instant;
all hosted executors share the runtime monotonic clock domain.

Registration release is nonblocking. Release and timer selection linearize on
the owning executor: release first prevents expiry, while timer selection
first is allowed to deliver cancellation. Timer processing never invokes task
cancellation while holding the executor lock.

## Terminal Precedence

| Linearized state | Result |
| --- | --- |
| success or error commits before expiry | preserve the committed terminal |
| explicit or ancestor cancellation before commit | cancelled |
| effective deadline expires before commit | cancelled |
| selected error requests child cleanup cancellation | preserve the error |
| independent deadline expires during error drain | finish drains, then cancel |
| explicit cancellation races deadline expiry | first internal cause wins; public result is cancelled |

Terminal selection under the task mutex is the commit boundary; observable
publication follows that selection. The runtime rechecks the sticky owner
request while committing a completed or failed resume result. Cancellation
requests use the same mutex, closing the race between the compiler's final
checkpoint and runtime selection.
Cleanup-induced child cancellation does not set the owner request and therefore
cannot replace an already-selected error.

Expiry requests and wakes; it does not preempt a running continuation. The
existing entry, suspension-commit, post-resume, explicit-check, and
executor-switch cancellation points remain the acknowledgement boundaries.

## Runtime And Compiler Contract

Task runtime ABI v1 grows append-only deadline registration functions while
keeping the ABI epoch and task outcome layout unchanged. Each executor owns a
minimum deadline heap. Production waits use the same monotonic domain used to
compute deadlines. The isolated test runtime instead uses an executor-local
manual clock whose advance operation wakes workers, so precedence tests do not
sleep on wall time.

LowIR frame plan v11 types the fixed policies: runtime monotonic clock,
absolute normalized representation, earliest-active inheritance,
deadline-as-cancellation outcome, first-linearized internal cause, and
cancellation-before-terminal-commit. The verifier rejects any other
combination before LLVM. LLVM does not infer deadline policy from surrounding
instructions.

## Deferred Source Surface

No parser token, timeout statement, deadline handle, wall-clock conversion, or
diagnostic spelling is added in this milestone. A source form should wait for
stable duration and instant value semantics and must compose with lexical task
ownership without exposing runtime registrations.

Detach, manual executor source control, cancellation shielding, and recoverable
cancellation remain unavailable. Expected expiry is an execution outcome, not
a diagnostic. Source diagnostics continue to cover only invalid async context,
task ownership escape, and references that cross a structured drain.
