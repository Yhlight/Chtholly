# Chtholly Preview Standard Library Reference

This is the user-facing reference for the standard library shipped in the
0.2.0 preview. The machine-readable authority is
[`stdlib/manifest.toml`](../stdlib/manifest.toml); this page explains
the ownership, failure, runtime, and safety boundaries rather than inventing a
second API manifest.

The current library format is 5, compiler contract is 14, and library API
epoch is 21. Imports are explicit; there is no implicit prelude.

Compiler artifact caching is an implementation/tooling concern rather than a
standard-library API. Release and development evidence can be generated with
`scripts/report-artifact-cache.py`; its reports describe cold/warm reuse,
reachability, stale leases, and explicit-GC recovery without changing program
semantics or the artifact identity contract.

`std::net` is the Result-based synchronous loopback TCP API. Its opaque
`Socket` handles are move-only foreign resources; `listen` and `accept`
initialize handles and `close` consumes one while returning a `Result`. The
`std::net::raw` CFDL module is the explicit physical carrier boundary; its
`read`/`write` operations return byte counts and use zero for EOF. Negative
host statuses remain compatible with `std::error` classification. The initial
boundary intentionally does not provide DNS, timeout, TLS, or asynchronous
socket syntax.

`std::typed_channel` is the experimental generic `Channel<T>` facade. A
specialization fixes the payload representation and compiler-owned move/drop
and Send/Sync witnesses. `init` and `close` return `Result<void, ErrorCode>`.
`receive()` returns `Result<T, ErrorCode>` and creates a payload only on success.
`send(move value)` returns `Result<void, SendError<T>>`; its error contains
`error: ErrorCode` and the still-owned `value: T` for explicit retry. Dropping
that error destroys the value once. The previous experimental output-parameter
receive interface is removed. Borrowed, pointer, dependent,
or non-transferable payloads are rejected. Cancellation-aware waits and task
wrappers are not part of this API yet.

The experimental runtime boundary rejects zero-sized or non-power-of-two
alignment descriptors before allocation, and caller-owned send/receive tokens
are single-use: a second commit, cancel, or receive completion is an invalid
argument. Prepare/cancel preserves the source value; only a successful commit
invokes the destructive move witness. These checks are runtime defenses in
addition to the compiler's descriptor and ownership verification.

| Module | Since | Boundary and primary use |
| --- | --- | --- |
| `std::result` | 1.0 | Canonical `Result<T, E>` payload enum and exact-error `?` propagation. |
| `std::option` | 1.3 | `Option<T>` for optional owned values, provenance-preserving `as_ref`/`as_mut` projections, and `Vec::pop`/`Vec::remove`. |
| `std::compare` | 1.0 | Comparison vocabulary used by standard protocols. |
| `std::convert` | 1.0 | Explicit and checked conversion error types. |
| `std::env` | 1.3 | Immutable UTF-8 process arguments. |
| `std::error` | 1.0 | Explicit cross-platform error domains and codes. |
| `std::io` | 1.3 | Safe stdout/stderr `Result` wrappers over the raw host boundary. |
| `std::fs` | 1.3 | Safe UTF-8 path operations plus owned Result-based file open/read/close streams. |
| `std::text` | 1.3 | Builtin string helpers and provenance-preserving byte views. |
| `std::time` | 1.0 | Monotonic time values and checked failure results. |
| `std::vec` | 1.3 | Owning dynamic storage, checked growth, element borrows, iterators, and deterministic drop. |
| `std::iter` | 1.5 | Static iterator protocol and consuming `next`. |
| `std::iter::adapters` | 1.6 | Range, array/slice adaptation, `take`, `map`, and `filter`. |
| `std::iter::algorithms` | 1.7 | Standard iterator algorithms and collection helpers. |
| `std::iter::mutable` | 1.9 | Mutable callable adapters and ownership-aware algorithms. |
| `std::collections` | 1.9 | Shared collection views, collectors, and the HashMap/HashSet open-addressing surface; scalar and verified custom nominal source-to-native witnesses are available, while unsupported recursive-by-value layouts fail closed. |
| `std::hash` | 1.10 | Explicit hash/equality vocabulary, compiler-owned integer/bool/char/float/pointer witnesses, and wrapping multiplication. |
| `std::callable` | 1.2 | Non-capturing and ownership-qualified callable protocols. |
| `std::ops` | 1.3 | Core operation protocol vocabulary. |
| `std::atomic` | 1.1 | Scalar atomics with explicit C++20/LLVM memory order. |
| `std::volatile` | 1.1 | Unsafe volatile scalar access to externally valid memory. |
| `std::host` | 1.3 | CFDL-backed host IO, monotonic clock, and task runtime handles. |
| `std::net` | 1.10 | Synchronous loopback TCP sockets with explicit move/close resource flow. |
| `std::net::raw` | 1.10 | CFDL physical socket carrier used by the Result-based `std::net` wrappers. |
| `std::sync` | 1.10 | Result-based blocking mutex/condition-variable API with move-only handles and checked lock/unlock/wait/notify/close operations. |
| `std::sync::raw` | 1.10 | CFDL physical mutex carrier used by the Result-based `std::sync` wrappers. |
| `std::channel` | 1.10 | Bounded byte transport with blocking send/receive and explicit close. |
| `std::typed_channel` | 1.10 | Experimental typed `Channel<T>` with compiler-owned lifecycle and Send/Sync witnesses. |
| `std::log` | 1.10 | Level-aware Result-returning stderr sink for formatted messages. |

## Safety and ownership

`Vec<T>` owns its allocation. Element references and iterators retain
provenance to the owner; structural writes such as growth, clear, and removal
cannot overlap live loans. Slices are borrowed views and never transfer
element ownership.

`std::atomic` is affine, non-copyable, and admits only the documented scalar
specializations. `std::volatile` and `std::host` are low-level unsafe/foreign
boundaries. Their physical signatures and resource facts come from CFDL; they
are not Chtholly source-level FFI declarations.

`std::error::ErrorCode` is an ordinary value. Error-domain conversion is
explicit; no implicit mapping or Component ABI transport is provided. Runtime
failures from `std::sync` are classified as `Domain::Sync`, while IO, network,
timeout, and cancellation constructors preserve their respective domains.
The `is_sync`, `is_cancelled`, `is_invalid_handle`, and `is_out_of_memory`
predicates are stable helpers for recovery without matching platform numbers.

`std::text::as_bytes` returns a read-only, non-owning `const slice<u8>` whose
provenance is tied to the source string. The `slice_data` and `slice_data_mut`
operations are narrow unsafe CFDL bridge projections used by `std::fs`; they
return only the physical pointer lane and never extend a view lifetime or
transfer ownership.
`std::hash` never treats `==` as an implicit equality witness.

Generic containers use the separate `chtholly_next_container_v1` native bridge.
SemIR records stable typed-vtable descriptors and native lowering validates the
target epoch and pointer width. The frozen reference ABI remains a single
pointer; static provenance plus a defensive table-generation check protect
`get`/`get_mut`. Verified custom nominal witnesses are materialized into
target-local LLVM callback thunks and included in the specialization/artifact
closure. Recursive-by-value nominal definitions remain rejected before native
linking; they are never treated as byte or memcpy witnesses. The bridge is not
part of legacy runtime v1 or Component ABI-1.

For a custom nominal key, provide complete `std::hash::Hash` and
`std::hash::Equal` implementations together with a valid move/drop lifecycle.
The compiler materializes these witnesses before LLVM lowering; a missing or
stale witness is a diagnostic error, not an implicit bytewise fallback. For
example:

```chtholly
import std::collections;
import std::hash;

lifecycle(copy = delete, move = default, drop = custom)
struct Key { value: i32; }
impl Key { fn drop(self: Key&): void { return; } }
impl std::hash::Hash for Key {
  fn hash(self: const Self&, seed: u64): u64 {
    return (self.value as u64) ^ seed;
  }
}
impl std::hash::Equal<Key> for Key {
  fn equal(self: const Self&, rhs: const Key&): bool {
    return self.value == rhs.value;
  }
}

// A nominal key must expose Hash, Equal, and a complete lifecycle.
var map = std::collections::HashMap<Key, i32>::make();
let result = map.insert(Key { .value = 7i32 }, 42i32);
```

`get` and `get_mut` return borrowed `Option` payloads. Keep the option and its
borrow within the owner generation; any structural mutation (insert, erase,
clear, or successful reserve) invalidates earlier borrows. Failed allocation
or failed rehash does not advance the generation. The native runtime enforces
the generation as a defensive check in addition to compile-time provenance.

## Synchronous application slice

The preview's supported synchronous path is intentionally small and
composable. `std::fs::open` returns an owned `File`; `read` borrows a mutable
byte slice; and `close` consumes the handle. `std::net::listen` and `accept`
return owned opaque sockets, while `std::net::close` consumes one. The current
network surface is loopback TCP only and does not include DNS, TLS, timeouts,
or asynchronous socket operations.

`std::sync` exposes move-only mutex and condition-variable handles. Locking,
unlocking, waiting, notification, and close return `Result<void, ErrorCode>`;
condition variables must not be closed while a waiter is active. The public
guard-token and task-suspension integration remains experimental. `std::log`
writes an already-formatted message to stderr and reports sink failure through
`Result`; structured fields and formatting builders are deferred.

These wrappers hide raw CFDL carriers but do not hide ownership: failed open,
listen, initialization, lock, read, or write operations leave the caller's
existing objects and initialization state subject to the same Result and
cleanup rules as ordinary Chtholly code.

All filesystem and host-file entry points use the same length-delimited path
boundary. Windows converts strict UTF-8 to UTF-16 before opening, while POSIX
preserves non-NUL bytes; embedded NULs, empty paths, invalid UTF-8, and size
overflow are rejected before allocation or native I/O.

## Failure and runtime policy

Recoverable IO/filesystem/conversion failures use the canonical `Result` type.
Bounds failures, invalid allocation requests, OOM, malformed foreign outcome
counts, and runtime protocol violations use the documented unrecoverable trap
policy. `std::sync` blocking primitives, detached tasks, user-selected
executors, and generic `buffer_prefix` remain deferred preview work.

Container allocation failures are reported as the documented `Result` status
of the operation. The testing build exposes deterministic allocator controls
for fault-injection tests only; those symbols are not available to published
components and do not change the stable container ABI.

The reference is checked against the manifest by:

```powershell
python scripts/reference-doc-audit.py --source-dir . --check
```

## Stage-two closure evidence

The synchronous API described above is part of the current preview closure,
not an ABI-only prototype. The acceptance path is exercised by
`chtholly_std_safe_e2e_tests`, `chtholly_host_runtime_tests`,
`chtholly_stdlib_llvm_fixture_tests`, `chtholly_component_e2e_tests`,
`chtholly_telemetry_ingest_tests`, and the complete generated
`chtholly-test` manifest. These tests cover successful and failed open/listen,
read/write and EOF, mutex exclusion, condition-variable predicate waits,
channel backpressure and close wakeups, typed-channel prepare/commit/cancel,
container allocation rollback, and exactly-once move/drop behavior.

The closure deliberately keeps the public boundary narrow. Component ABI-1
and runtime ABI v1 remain compatible; physical foreign calls and resource-flow
facts remain in CFDL; and no implicit prelude, hidden allocation, or ownership
conversion is introduced. Typed channels, task handles, guard tokens, and
registry distribution remain subject to the experimental policy in
`support/chtholly-product-status.toml` even though their synchronous evidence
is available.

## File processing and explicit error mapping

`std::fs::open_read(path)` opens an existing file read-only and never creates
it. `create(path)` creates or truncates a file. `write_bytes(file, bytes)`
returns a byte count; `write_all(file, bytes)` handles partial writes and fails
on zero progress. Existing `open` retains its read/write-or-create behavior.
All new wrappers return `Result<..., ErrorCode>`. CFDL reference adapters pass
handle contents to the native APIs while preserving source borrow authority.

`std::result::map_error(value, transform)` consumes the result and maps only
its error via `InvokeOnce`; the success payload is transferred unchanged.
A `Result<void, E>` overload produces no success payload. Error conversion is
explicit; postfix `?` continues to require the enclosing error type.
