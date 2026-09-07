# Applications

Chtholly Next can produce native command-line programs from a source file, a
project manifest, or a workspace. The maintained executable evidence lives in
the remaining artifact and driver checks under `tests/`. Legacy frontend/FFI
suites were removed during the CFDL boundary migration.

The preview synchronous standard library is implemented over Runtime ABI v1.
The telemetry application intentionally keeps its native ingress host as the
file/TCP boundary; the Chtholly `std::net` module is covered independently by
the verified loopback socket lifecycle tests. Runtime ABI v2 and the richer
async resource surface remain future work.

## Supported Application Range

Maintained coverage includes direct compilation, multi-package build/run,
workspaces, cross-package generics, associated and instance functions,
ownership and cleanup, payload enums, `Result` propagation, safe console
output, UTF-8 process arguments, filesystem, loopback networking, mutex/
condition-variable, byte/typed channels, logging, and task execution.
C interoperability is authored by CFDL and consumed through artifacts;
cancellation-aware async channel wrappers remain design-pending.

The maintained application-driven Component ABI-1 evidence includes both the
Telemetry workspace and the component-host vertical. Telemetry independently
builds `telemetry-component`, verifies contract identity and digest, consumes
file and localhost TCP input, exercises typed channel ownership, and validates
deployment activation, upgrade rejection, and rollback. See
`examples/telemetry-pipeline/README.md`.

The component-host sample is deliberately orthogonal: two independently
compiled providers expose the same scalar synchronous exports under distinct
deployment identities. Its clean-source soak copies both packages into a
temporary tree, records identity/digest pairs, rejects cross-component export
IDs, and repeats concurrent load/invoke/close/release/reload. Rebuilding only
beta after changing `plugin::process` makes its result change from `value + 2`
to `value + 3` through the unchanged Component ABI-1 host. See
`examples/component-host/README.md`.

The sample makes the next standard-library/CFDL work concrete: deployment
identity and lifecycle orchestration still need native glue, and the current
ABI-1 surface intentionally remains scalar and synchronous. Rich resource,
timeout, and asynchronous protocols remain deferred design work rather than
implicit additions to the language or ABI.

Release stage-boundary evidence treats the two maintained verticals as
application reports, not as language or ABI proposals. Their reports carry the
same source-commit/target provenance as package-size, install-lifecycle, cache,
and build-performance evidence; diagnostic counters remain observational and
do not affect semantic or artifact identities.
