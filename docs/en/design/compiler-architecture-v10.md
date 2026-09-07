# Compiler Architecture V10: CFDL Carrier Separation

Status: implemented by the 1.9.6 CFDL epoch-8 wave.

## Boundary

CFDL compilation now owns three independent records:

- identity: canonical package, module, and foreign type name;
- carrier: absent, integer/pointer, or an ABI-only record type tree;
- protocol: callable-derived loans, obligations, events, and predicates.

Parsing creates identity shells. A completion pass resolves target-aware C
scalars, forward references, nested records, and fixed arrays before callable
elaboration. Value recursion, incompatible sentinels, and unsized `out` lanes
fail before publication.

## Representation

Record carriers use a structural physical tuple internally but do not populate
the public nominal field list. Public and imported identities therefore expose
no field lookup or construction capability. LowIR retains the semantic nominal
type on each foreign layout while recursively classifying its completed carrier.
LLVM consumes only the verified lanes, byval plan, or sret plan.

## Version Closure

- CFDL semantic epoch: 8
- Interop bundle/reference: `CHNXIOP5`, schema 4
- Package Artifact: v11
- foreign function/call layout: epoch 10
- nominal layout: epoch 8
- Component ABI/runtime ABI: unchanged at epoch 1/v1

Older artifacts fail closed. No Chtholly language-version surface changed.
