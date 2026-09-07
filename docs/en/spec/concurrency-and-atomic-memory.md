# Chtholly Concurrency And Atomic Memory

Status: normative design for the Chtholly 1.1 scalar atomic and volatile-memory
core. Implementation status is tracked by `support/chtholly-v1.1.toml`.

## Memory Model

A data race is two conflicting accesses to one memory location from different
threads, at least one a write, without an ordering relation and without all
conflicting accesses being atomic. A data race is undefined behavior. Safe
Chtholly code cannot create one without crossing an unsafe or foreign boundary.

Atomic operations follow the target-independent LLVM and C++20 model. The
admitted orders are `Relaxed`, `Acquire`, `Release`, `AcqRel`, and `SeqCst`;
consume ordering is not admitted. Volatile access is an observable device or
foreign-memory access only. It is neither atomic nor synchronization.

Types have compiler-derived `transferable` and `shareable` facts. Transferable
values may change threads. A shared reference may cross a thread boundary only
when its referent is shareable. Unknown imported or foreign facts fail closed.
Atomic storage is both transferable and shareable for its admitted scalar
specializations.

## Source Surface

The API is available only after explicit `import std::atomic;` from a 1.1
package. `std::atomic::Ordering` has exactly five variants: `Relaxed`,
`Acquire`, `Release`, `AcqRel`, and `SeqCst`.

`std::atomic::Atomic<T>` accepts `bool`, every fixed-width integer, `isize`, and
`usize`. It is affine, movable, non-copyable, trivially destroyed, and may be
constant-initialized. It has opaque Chtholly representation and is invalid in
`repr(C)`, C parameters, C results, and C aggregate transport.

The canonical operations are `init`, `load`, `store`, `exchange`, strong
`compare_exchange`, and integer-only `fetch_add`, `fetch_sub`, `fetch_and`,
`fetch_or`, and `fetch_xor`. Every access method receives `const Self&` and may
perform only its compiler-verified atomic interior mutation.

Every operation takes an explicit `Ordering` argument which must be a constant
after generic specialization. Load admits Relaxed, Acquire, and SeqCst. Store
admits Relaxed, Release, and SeqCst. Read-modify-write admits all five orders.
Compare-exchange takes success and failure orders; failure rejects Release and
AcqRel and cannot be stronger than success.

Compare-exchange returns
`CompareExchange<T> { observed: T, exchanged: bool }`. A failed comparison is a
synchronization result, not a recoverable language error and not a `Result`.

## Volatile Foreign Memory

`std::volatile::load<T>(const T*)` and
`std::volatile::store<T>(T*, T)` accept integer scalars and require unsafe
authority. Their caller promises a live, correctly aligned, externally valid
location of the exact pointee type for the complete access.

Volatile operations do not create provenance, extend lifetime, grant mutable
authority, make an access atomic, or order another access. There is no volatile
reference, qualifier, aggregate access, read-modify-write operation, or
bitfield operation in 1.1.

## Semantic And Artifact Contract

Only canonical toolchain-standard-library entities may carry compiler intrinsic
roles. A package cannot acquire a role from its module or function spelling.
Role, signature, scalar kind, operation, and ordering are rechecked before
SemIR construction, generic artifact publication, concrete materialization,
LowIR acceptance, and LLVM lowering.

Atomic and volatile operations are not constant-evaluator operations. Public
generic templates persist typed operations and ordering facts rather than an
ordinary call to a replaceable library body. Transfer/share facts participate
in public nominal witnesses and dependency fingerprints.

LLVM lowering uses native atomic load/store, atomic read-modify-write, strong
compare-exchange, and volatile load/store instructions. A target limitation is
a lowering error; it does not silently replace atomic semantics with ordinary
memory access.
