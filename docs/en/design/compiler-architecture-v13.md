# Compiler Architecture V13: CFFI Artifact Identity Closure

Status: implemented by the 1.9.9 CFFI artifact-identity wave.

## Boundary

`CHCFFI1` is parsed once into a canonical `CFFIReceiptIdentity`. Its target and
Clang, libclang, C compiler, configuration, header closure, CFDL, probe, and
mechanical-fact digests are the single identity used by `chtholly-cffi`, the
Next build plan, package check/build artifacts, lockfiles, and published
Package Artifacts. Missing, duplicate, reordered, unknown, non-canonical, or
oversized receipt records fail before semantic compilation.

The receipt file is a required build-control snapshot input. Its canonical
fingerprint participates in driver-session resolution and the Next compilation
configuration, so replacing a receipt invalidates provider and consumer reuse.
The resolved graph target must exactly match every provider receipt, including
dependencies which do not repeat a target in their own manifest.

## Artifact Closure

Next package state is `CHNXTPK71` format 68 and cache namespace `next-v40`.
Package Artifact v14 persists the complete CFFI identity plus the receipt
closure file and verifies both representations agree. Package Artifact v13 and
`CHNXTPK70` state fail closed and must be rebuilt. `CHCFFI1`, CFDL epoch 9,
`CHNXIOP6`, semantic artifact epoch 17, Component ABI epoch 1, and runtime ABI
v1 are unchanged.

The executable gate runs `chtholly-cffi generate` and `verify`, compiles the
generated CFDL as a provider package, imports its public and Interop artifacts
from an independent consumer package, links the native C provider, executes
the result, repeats a locked warm build, and rejects receipt and target drift.
