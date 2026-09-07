# Compiler Architecture V12: CFFI Generate And Verify

Status: implemented initial standalone tooling wave.

## Boundary

`chtholly-cffi` is an independent tool. `chthollyc` does not link libclang and
does not implicitly regenerate or verify bindings. A versioned TOML config
selects headers, roots, target, Clang arguments, C compiler, and probe links.

`generate` builds a Clang 18 declaration model and emits a deterministic CFDL
draft. It never overwrites an existing output and never edits human protocol
facts. `verify` compares the supplied CFDL against the current C declaration
model, validates its resource protocol, builds a native C probe, and runs it.

## Trust Boundary

Clang supplies mechanical declaration facts: names, type closure, enum values,
record/union shape, function pointers, calling convention, and mangled/linkage
symbol. It does not supply ownership, invalid sentinels, cleanup, callback
lifetime, or error protocol. Those remain explicit CFDL facts.

The target C compiler and native probe are the physical ABI authority. A
`CHCFFI1` receipt records target, Clang/libclang, compiler, config, header
closure, CFDL, probe, and fact digests. Receipts are deterministic. Package
Artifact v13 may carry the receipt as a verified closure payload; the Next
lockfile/build validation remains the pre-compilation gate and does not link
libclang.

Architecture V13 closes receipt identity across internal and published
artifacts with Package Artifact v14; this document remains the original
generator/verifier boundary.

## Closed Surface

The initial tool rejects bit-fields, flexible arrays, packed records, variadic
functions, global/TLS objects, unsupported calling conventions, cross targets,
and bare anonymous roots without a stable carrier identity. Anonymous records
named by a typedef are accepted as stable carriers. Union carriers remain
ABI-only. SQLite is the first real library validation alongside the dedicated
enum/union/callback fixture.
