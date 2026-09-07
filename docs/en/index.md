# Chtholly Documentation

`spec/` is the sole normative language specification. Design documents explain
the current compiler architecture, while `compiler-status.md` reports
implementation boundaries without extending the language.

## Normative Specification

1. `spec/README.md`
2. `spec/language-charter.md`
3. `spec/abstract-machine-and-memory-model.md`
4. the domain specifications listed by `spec/README.md`
5. `spec/core-language.md`
6. `spec/component-and-ffi-abi.md`
7. `spec/cfdl.md`
8. `spec/language-surface.md`
9. the generated versioned surface tables

## Project References

- developer preview quickstart: `quickstart.md`
- concrete/package/artifact compatibility: `../support/chtholly-compatibility.toml`
- Chtholly test framework: `testing.md`
- standard library reference: `stdlib-reference.md`
- diagnostics reference: `diagnostics.md`
- project and workspace manifests: `manifest-reference.md`
- CFFI generation and regeneration: `cffi.md`
- signed installation, upgrade, and rollback: `toolchain-management.md`
- frozen surface manifests: `support/chtholly-v1.toml` through
  `support/chtholly-v1.10.toml`
- release supply-chain evidence: `supply-chain.md`
- SQLite safety-wrapper application vertical: `../examples/sqlite-safety/README.md`
- custom container witness closure: `design/next-custom-witness-closure.md`
- typed channel runtime prototype: `design/next-typed-channel-runtime.md`
- recursive Send/Sync summary: `design/next-type-concurrency-summary.md`
- typed-channel descriptor artifact: `design/next-typed-channel-descriptor.md`
- typed-channel SemIR/LowIR transitions: `design/next-typed-channel-transitions.md`
- async channel API design decision: `design/next-async-channel-api.md`
- Component ABI-2 resource protocol: `design/next-component-abi-2-resource-protocol.md`
- compiler source layout: `design/source-layout.md`
- ownership performance: `performance/1.9.1-ownership-baseline.md`
- compiler/build/cache performance: `performance/build-baseline.md`
- architecture and feature lowering: `design/`
- registry and release operation: `registry/` and `release/`
- executable evidence: `tests/chtholly_*`
