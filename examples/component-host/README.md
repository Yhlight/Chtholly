# Component Host

This example is the second maintained vertical application (alongside the
Telemetry pipeline): an external C++ host for two independently built
Chtholly Component ABI-1 modules. The host includes only the installed public C
loader header. Both components export the same scalar, synchronous
`plugin::process` and `plugin::hold` functions, while their deployment
identities keep export IDs and calls isolated.

The test orchestrator copies alpha and beta into a clean temporary source tree,
so no repository-local `.chtholly` cache or lockfile is required. It records
each identity/contract-digest pair, rejects cross-component export IDs, and
runs repeated concurrent load, invoke, close, release, delete, and reload
cycles. It then mutates only beta's `plugin::process` body (`+2` to `+3`),
rebuilds beta, and invokes the new generation through the same ABI-1 host to
prove that application behavior changes without an ABI or syntax change.

Use `tests/chtholly_component_host_soak_tests.py --duration-seconds 120` for
the long soak mode, together with the configured `chthollyc` and
`chtholly_component_host_example` paths. The normal generated test manifest
runs the same workflow for 15 seconds.

This vertical intentionally exposes current library/CFDL gaps: deployment
identity and lifecycle management still require a small native host, and the
sample uses scalar calls rather than a richer resource or asynchronous
protocol. Those are follow-up library/ABI design inputs, not new source syntax
or a widened Component ABI-1 transport.
