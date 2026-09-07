# Compiler Architecture V8

Status: implemented for the Chtholly 1.9.4 component ABI wave.

Architecture V8 introduces the first public dynamically loaded component ABI
without exposing Chtholly native symbols or internal specialization artifacts.

## Component Contract Ownership

`[component]` is a deployment manifest boundary, not source syntax. The driver
resolves its export whitelist to unique public entities. Each owning unit
builds an immutable export lowering plan before LLVM; a package-level
`CHNXCMP1` contract canonicalizes exports by stable export ID and recomputes
identity, signature, and contract digests.

LLVM consumes the checked plan and emits one hidden uniform thunk per export.
A separate descriptor object references thunks across modules and exports only
`chtholly_component_query_v1`.

## Epoch 1 Boundary

Epoch 1 admits synchronous safe non-generic public free definitions. Parameters
are booleans, concrete-width integers, `f32`, `f64`, or read-only `slice<u8>`;
results are void or scalars. The uniform value record is 32 bytes on the
supported x64 targets. Byte views remain valid only for one call.

There is no hidden component instance state, constructor, destructor, reload,
state migration, dynamic interface value, `repr(C)` aggregate transport, or
component-owned cross-boundary allocation. A trap does not unwind through the
boundary.

## Loader And Unload

The stable C loader requires an absolute path and exact identity, contract,
target, and runtime ABI digests. It structurally verifies the descriptor and
recomputes identity and contract digests before accepting the module. Calls use
an opaque module handle plus export ID; raw thunk and descriptor addresses are
never returned.

Unload marks the module closing, rejects new calls, waits for all acquired call
leases, and unloads exactly once. Windows uses restricted `LoadLibraryExW`;
Linux uses `dlopen` with `RTLD_NOW | RTLD_LOCAL`.

## Compatibility

Component ABI epoch 1 and `CHNXCMP1` are new public compatibility axes.
Package artifact encoding v10 adds an optional component contract file record;
archives include and digest that sidecar as part of their verified closure.
Semantic artifact epoch 17, compiler contract 14, cache `next-v38`, concrete
component `CHNXSCC46`, runtime ABI v1, and all CFDL epochs remain unchanged.
