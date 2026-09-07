# Chtholly Component And FFI ABI

Status: Next-only normative boundary. C ABI declarations are authored in CFDL
and consumed by Chtholly through verified binding artifacts.

## Component Interfaces

A public Chtholly interface publishes canonical module/entity identity, type and
call signatures, visibility, cleanup facts, and ABI epoch. Imported CFDL
functions use the same ordinary public entity lookup. Their external ABI and
resource facts live in the associated CFDL Interop artifact rather than in
Chtholly source declarations.

Owned values crossing a component boundary carry an obligation to perform their
terminal action. Borrowed values remain valid only for the declared loan
region. Derived views remain valid only while their source resource is valid.
Reloadable components exchange owned values, serialized state, or versioned
opaque handles; native addresses are not durable identities.

### Dynamic Component ABI Epoch 1

The first public component ABI is selected only through `[component]` manifest
metadata. It does not change Chtholly source syntax. A component publishes one
fixed C query symbol whose immutable descriptor carries exact identity,
contract, target, and runtime ABI digests plus a sorted export table. Export
IDs and signature digests are SHA-256 identities; Chtholly mangled names are
not public component ABI.

Epoch 1 exports synchronous safe non-generic public free definitions. Its
transport vocabulary is void, bool, concrete-width integer, `f32`, `f64`, and a
call-scoped read-only byte view. Results are void or scalar. Raw pointers,
references, nominal values, `repr(C)` aggregates, async functions,
members, and mutable or returned slices are rejected.

The C loader never returns native export addresses. It invokes by stable export
ID, validates every uniform value record, and retains an internal call lease.
Unload rejects new calls and waits for existing leases before releasing the
module. Epoch 1 has no hidden instance state, reload, or state migration.

An external host constructs its handshake through the public requirement
initializer using a deployment-pinned identity and contract digest plus the
normalized target and runtime ABI. Hosts do not need to parse `CHNXCMP1` or
link compiler libraries. Empty, invalid UTF-8, zero-digest, unknown-target, and
unknown-runtime inputs fail closed.

Concurrent hosts use two-phase `close` and `release`. Close rejects new calls,
waits for call leases, and unloads code while retaining a closed opaque handle;
calls then return `CLOSING`. Release destroys only a closed handle. The combined
unload operation is valid when the host has already synchronized all accesses.

## CFDL C ABI

CFDL uses the verified target-neutral C ABI model. The current source grammar
declares physical scalars, pointers, opaque foreign types, and C function
pointers. Variadic and aggregate layouts exist in the compiler ABI model, but
the frozen CFDL source surface cannot yet name a Chtholly `repr(C)` nominal or
declare an aggregate shape. Target-aware lane classification is performed once
by Interop lowering and reused by declarations, calls, attributes, callback
thunks, and return reconstruction when such a verified type is present.

The CFDL source form is:

```cfdl
foreign fn read(
    file: ref File,
    buffer: ref_mut Buffer
) -> owned Request
where
    buffer escapes call_return,
    result obliges resolve;
```

`owned` creates an obligation-bearing result. `move` transfers an obligation
carrier. `ref` borrows a resource identity. `view` borrows a data projection.
`derive` records dependent validity and is not an exact-storage alias promise.

The C ABI does not infer ownership, readable/writable ranges, borrowed returns,
callback invocation, external cleanup, quiescence, or completion. These facts
must be expressed by the finite CFDL `where` relations or the binding is
rejected as incomplete.

## Chtholly Source Boundary

The following are not Chtholly syntax:

```text
extern
unsafe extern "C"
bind
foreign fn
foreign type
inline FFI contract blocks
```

Chtholly imports an ordinary module and calls its published entities. The
compiler driver loads the CFDL artifact before semantic import resolution and
passes an opaque binding reference to the Interop backend.

## ABI And Artifact Safety

Every ABI and artifact schema is versioned. Fingerprints include canonical
module/entity identity, normalized ABI facts, resource-flow facts, and epoch
numbers. Decoders reject unknown fields, incompatible epochs, malformed flow
facts, unresolved endpoints, and ambiguous obligation discharge. No old
Operation Object or Chtholly `bind` artifact is migrated.

Foreign unwind may not cross a Chtholly boundary. External failures must use
declared value/status channels. Callback, completion, quiescence, and
exactly-once release plans remain compiler/runtime implementation details.
