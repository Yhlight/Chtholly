# Telemetry Pipeline

This Chtholly 1.10 workspace validates collection protocols in a realistic
multi-package data-processing kernel. It generates deterministic telemetry,
normalizes and filters cross-package struct samples with mutable callables,
collects a cross-package summary, builds a Vec through a second concrete
`MapMut`, and validates aggregate-array return and indexing.

The workspace also contains `telemetry-wire`, which defines the version-1
little-endian 36-byte frame used by file and localhost TCP input. The native
ingress host consumes either source, validates sequence gaps and truncation,
flushes batches of 128 frames through the checksum component, and writes a
deterministic JSON report.

Input buffering uses the typed runtime channel between the producer and the
frame-processing consumer. The host models the payload as `Channel<Sample>`
(`Sample` is the decoded frame), so backpressure, move/drop callbacks, and
channel shutdown are exercised in the same path as normal ingestion. The host
also accepts a strict
`key = value` configuration file for `file`/`listen` and `output`:

```text
file = telemetry.bin
output = telemetry.json
```

Reports include input byte count and elapsed milliseconds in addition to frame,
batch, checksum, value, and sequence-gap totals. The semantic fields are
deterministic for identical input and deployment; `elapsed_ms` is an
observational timing field and is not part of deterministic report comparisons.

For installation-tree validation, activate a generation with
`chtholly-component-deploy` and pass `--deployment-root <root>` to the ingest
or component host. The active generation is verified on each process start,
so upgrades and rollback exercise the same runtime path.

The Chtholly application probes the hosted monotonic clock through
`std::host::monotonic_now` and propagates failure as `Result<bool, i32>` before
running the cross-package analysis.

```powershell
chtholly_telemetry_ingest_host.exe LIB CONTRACT TARGET RUNTIME `
  --file telemetry.bin --output telemetry.json
```

```powershell
chtholly_telemetry_ingest_host.exe --deployment deployment.toml `
  --config telemetry.conf
```

```powershell
chthollyc check --workspace .
chthollyc run --workspace .
```

The executable returns zero after processing 50,000 summary samples, 1,000 Vec
samples, and a cross-package `Sample[2]`. No file or network API is required,
so the example isolates language, artifact, aggregate representation,
ownership, warm specialization, and native execution boundaries.

The `telemetry-component` package is the first Component ABI vertical slice
for this application. It exports `telemetry::component::checksum`, accepts a
call-scoped `slice<u8>`, and returns a scalar aggregate. A native host validates
the contract handshake, export identity, non-empty and empty inputs, and the
close/release lifecycle:

```powershell
chthollyc check --project .\telemetry-component
chthollyc build --project .\telemetry-component --out-dir .\out\telemetry-component
..\..\build-ninja\examples\telemetry-pipeline\host\chtholly_telemetry_component_host.exe `
  (Resolve-Path .\out\telemetry-component\telemetry-component.dll) `
  (Resolve-Path .\out\telemetry-component\telemetry-component.dll.chcomponent) `
  x86_64-pc-windows-msvc v1
```

The component deliberately uses only the existing ABI-1 scalar/byte-view
transport. Nominal aggregate transport and persistent component state remain
future design work rather than being hidden behind an example-specific ABI.
