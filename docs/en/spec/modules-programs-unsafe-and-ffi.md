# Chtholly Modules, Programs, And Unsafe Authority

Status: Next-only Chtholly source boundary. Foreign definitions are owned by
CFDL and are consumed as immutable artifacts; this document does not define a
foreign declaration syntax for `.cns` files.

## Unsafe Authority

`unsafe { statements }` and `unsafe expression` grant lexical authority for a
compiler-defined operation. Authority does not flow through callable values,
names, imports, return values, or artifacts. Unsafe code still undergoes type,
initialization, ownership, cleanup, provenance, visibility, representation,
and artifact verification.

The authority catalog covers raw-pointer access and other explicitly admitted
low-level operations. It does not create a foreign ABI or a resource contract.
Raw-pointer arithmetic, general representation casts, and undocumented host
effects have no source spelling.

## Module Aliases

Imports may introduce a controlled local lookup name with
`import package::module as alias;`. Exported imports use the same form. The
alias is not part of the provider's canonical identity or public entity
fingerprint; it only selects the local module key used by qualified lookup.
Duplicate aliases and ambiguous providers are rejected.

## Foreign Boundary

Chtholly source has no `extern`, `bind`, callback-adapter declaration, or
inline FFI contract. A binding author writes a separate `.cfdl` unit using
`foreign type`, `foreign fn`, flow qualifiers, and the finite `where` facts in
`docs/spec/cfdl.md`. CFDL publishes an immutable Interop artifact containing
the physical ABI, external linkage, resource loans, obligations, and event
facts. Chtholly imports only the ordinary public entities from that artifact.

The Interop backend is the only layer that may construct ABI layouts,
callback/completion plans, or external symbols. Chtholly source cannot weaken,
replace, or extend those facts. ABI unwinding and target layout rules are
verified at artifact and lowering boundaries.

## Program Model

Chtholly v1 defines hosted programs with one synchronous root entry:

```cns
fn main(): i32 { ... }
```

It is non-generic and parameterless. Startup and runtime bridge behavior are
implementation details of the hosted driver, not source-level FFI contracts.
No prelude is injected; standard-library modules enter the graph only after an
explicit import.
