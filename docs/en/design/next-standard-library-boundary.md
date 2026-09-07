# Next Standard Library Boundary

Status: migration target for the Next-only standard-library distribution.

## Boundary

The standard-library toolchain distribution lives under
`share/chtholly/stdlib` and contains a strict manifest plus the complete
declared source inventory. It packages `std::result`, `std::convert`,
`std::compare`, operator support, atomic/volatile helpers, and pure time value
types. CFDL-backed task cancellation, scheduler, and hosted async facade
modules are not part of the current standard library.

The manifest owns language version, imports, runtime symbols, and compiler
intrinsics. Unknown or duplicate keys, unsupported epochs, non-canonical
closures, duplicate modules or paths, escaping paths, missing files, and
compiled artifacts that disagree with the manifest fail closed. All record
fields and source fingerprints form a domain-separated distribution
fingerprint.

`std::host` declarations use the same status-plus-out-handle ABI as
`next_host_v1.h`: handle creation returns an `i32` status and initializes an
`out` foreign nominal, while close/cancel/wake/join return status values.
This keeps the CFDL physical contract independent of platform calling
conventions and makes ownership obligations explicit in the binding artifact.
Chtholly callers declare typed uninitialized storage and pass its ordinary
place name; `out` remains CFDL-only spelling.

## Loading And Packaging

The driver uses the Next lexer and parser to collect each package's explicit
imports before scheduling. Workspace imports seed the manifest dependency
graph; the driver compiles only the transitive module closure and adds one
direct dependency on the toolchain-owned `std` package. Packages without such
an import do not load or observe the distribution. The standard library is
built with injection disabled, so its internal imports resolve within its own
session rather than creating a self edge.

The manifest and sources are build-control and source-snapshot inputs. CMake
stages and installs this tree as the sole standard-library resource. The
distribution ships source, not target-specific prebuilt objects; ordinary Next
compilation materializes target objects into the existing CAS.

## Executable Evidence

The standard-library boundary has three checks. The provider fixture executes
the Windows `next_host_v1` ABI for file IO, monotonic time, and task
poll/cancel/join. The LLVM fixture compiles `std::host` CFDL, exports and
decodes its epoch-7 Interop sidecar, and requires canonical completion, cancel,
and wake event sets plus the status-plus-out contract before accepting the
generated LLVM module. A driver Build/Run fixture imports `std::host` directly
and executes source calls using typed uninitialized `let`/`var`, ordinary place
arguments, `&`, and `move`: it
opens/writes/reads/closes a real temporary file, checks the failure sentinel
path, obtains monotonic time, and drives deterministic task
poll/wake/cancel/join states. No separate probe binding exists.

Runtime mappings are session-level target inputs. LLVM foreign declarations
replace a source name only when the verified runtime manifest contains that
name; ordinary user FFI remains source-named. Mapping changes participate in
the compile fingerprint, and a standard-library runtime symbol without a
manifest mapping fails before linking.

## Supported Surface And Direction

The inventory additionally contains `std::option`, `std::env`, `std::io`, and
`std::text`. Environment access returns immutable runtime-owned UTF-8 `string`
values, while safe console writes accept the same pointer-plus-length value and
never expose a host handle. `std::vec` now provides runtime-backed aligned
dynamic storage with compiler-generated element relocation/drop and
`Option<T>` removal results. The runtime allocator intentionally receives only
byte size and alignment; generic lifecycle operations remain in verified LLVM
lowering. The task/cancellation modules are
removed because public async
source syntax is not part of the current roadmap and their implementation was
an FFI binding surface rather than a language capability.

`std::result::Result<T, E>` is the compiler-known v1 failure carrier with
`Ok { T }` and `Err { E }` payload variants.

`std::convert::CastError` is the compiler-known checked-conversion error enum
with unit variants `Inexact`, `OutOfRange`, and `NonFinite`.

`std::compare::Ordering` is the compiler-known three-way comparison result with
unit variants `Less`, `Equal`, `Greater`, and `Unordered`. Source using `<=>`
must explicitly import `std::compare`; no comparison prelude is injected.

`std::time` provides the 16-byte `repr(C)` value types `Duration` and `Instant`
plus checked affine constant methods. The hosted `monotonic_now` FFI query is
deferred to a separate CFDL/Interop package. Structured timeout and deadline
syntax remain outside this standard-library milestone.

Compiler-internal task-frame runtime support may remain hidden in the runtime;
it is not a public `std` module or source-level async API.
