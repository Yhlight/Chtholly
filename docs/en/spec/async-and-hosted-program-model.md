# Async And Hosted Program Model

Status: normative for Chtholly 1.1 and preview product capability. Chtholly
1.0 remains unchanged. A package
must select `package.language = "1.1"` before using this surface.

## Source Surface

An asynchronous declaration has this form:

```chtholly
async fn fetch(): i32 { return 42; }
```

`pub` may precede `async`. An async declaration is a safe, non-constant,
non-generic free function definition. Methods, foreign declarations and
functions returning references are not async declarations. `async`, `wait`,
`check`, `cancel`, `task`, and `scope` are contextual words, so declarations
and qualified members may otherwise retain those names.

Calling an async function is allowed only in an async body. Arguments are
evaluated left to right exactly once and the child starts eagerly. The call
produces a compiler-owned, move-only task value. Its type may be inferred for
a local but cannot be named, stored in an aggregate, published in an
interface, or returned. Every path must consume or structurally transfer the
task exactly once.

`wait task` is a prefix expression that consumes a task. For an async function
returning `T`, it produces `T`. For a function returning the canonical
`std::result::Result<T, E>`, it produces that result after preserving success
and error as distinct task terminals. Cancellation is never converted to
`E`. A reference may not remain live across a wait that can suspend.

`check cancel;` is an explicit cancellation point. `wait` and `check cancel;`
are valid only in an async body and not in a `defer` body or constant
evaluation. Ordinary calls and loop backedges are not implicit cancellation
points.

`task scope { statements }` creates a lexical structured-child set. A
success-only child may transfer its completion obligation to that scope;
fallible children still require `wait`. Normal exits close and drain the set.
Error and cancellation exits request child cancellation and drain before
continuing the selected terminal. Nested scopes quiesce from inner to outer,
before deferred bodies and local destruction.

There is no detached task, task handle type, group handle, manual cancellation,
executor selection, timeout, shielding, recovery, `await`, or postfix
suspension syntax in 1.1.

References, slices, iterators, and aggregate values containing borrowed
provenance may not remain live across `wait`, task-scope drain, or another
possible coroutine suspension. The compiler reports
`chtholly.next.sem.async.reference-across-suspension` at the suspension
boundary. Owned values with verified transfer/share facts may be lifted into
the frame. Chtholly 1.1 does not lift checked borrows into frames and does not
add a lifetime or borrow-handle ABI.

## Hosted Entry

A hosted executable has exactly one selected root entry. It may use the frozen
synchronous entry or this asynchronous entry:

```chtholly
async fn main(): i32 { return 0; }
```

No other async `main` signature is valid. The compiler emits one internal root
runner, creates the runtime-v1 executor and cancellation scope, starts the
root task, joins it, takes its terminal payload, and releases task, scope, and
executor in that order. A successful result becomes the process status.
Cancellation uses status 3, an unhandled declared error uses status 4, and
runtime protocol failures use reserved statuses 120 through 122 or the stable
coroutine trap reasons.

The source scaffold is not a second host entry and cannot be called through a
synchronous function value. Startup and shutdown remain hosted. Freestanding
startup, user-selected executors, async command-line parameters, and multiple
entry candidates are outside 1.1.

## Runtime And Link Boundary

The production bridge is the C ABI declared by
`runtime/include/chtholly/next_hosted_async_v1.h`. Its symbols use the
`chtholly_next_hosted_async_v1_` prefix and adapt standard-library
subscription, completion, task, and scope operations to task runtime v1.
Project-provided test hooks and unqualified scheduler or `c_*` symbols are not
part of the language or runtime contract.

Standard-library manifest format 4 records each module's canonical imports
and runtime-symbol requirements. The compiler starts from workspace imports,
computes the manifest's transitive standard-library closure, and compiles only
that set. After compilation it requires the source-derived artifact closure to
equal the manifest declaration.

`CHNXTPK63` package and check artifacts persist sorted, unique module
dependencies and foreign-symbol requirements. Executable linking starts at
the selected root module and follows only those persisted dependencies.
Missing modules, undeclared packages, duplicate metadata, a standard-library
source/manifest mismatch, or an unknown symbol in the hosted-async namespace
fails closed before invoking the native linker. Artifact-only builds never
rediscover imports from source text.

## Concurrency Contract

Task arguments and frame-owned values use the compiler-derived transfer and
sharing facts defined by the 1.1 concurrency memory boundary. Runtime
scheduling and cancellation synchronize through task runtime v1; neither
`volatile` nor an unsafe block establishes inter-thread synchronization.
Unsafe foreign callbacks retain their declared ownership and authority
contracts across suspension.