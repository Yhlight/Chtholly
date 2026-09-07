# Chtholly Test Framework

`chtholly-test` is the only test orchestration entry point for Chtholly. The
generated TOML manifest supports isolated process tests, registered C++ tests,
labels, capability selection, stable ordering, parallel jobs, timeouts, retries,
and text/JSON/JUnit reports.

Long-running or native-boundary runs can opt into progress events with
`--progress`. Events are written to stderr so JSON and JUnit stdout contracts
remain machine-readable:

```text
RUN chtholly_cffi_tool_tests
DONE chtholly_cffi_tool_tests code=0
```

On Windows, process tests are assigned to a Job Object. A timeout therefore
terminates compiler, linker, and other descendants as one unit instead of
leaving helpers that can hold build artifacts or stall later tests.

Validate a generated or hand-written manifest before running an expensive
subset:

```powershell
chtholly-test validate --manifest .\\build-ninja\\tests\\chtholly-tests.generated.toml
```

Validation checks the manifest schema, duplicate names, process commands,
in-process registry names, and positive timeouts without launching tests. This
keeps new fixtures cheap to register and makes the generated manifest the
single source of test selection.

## Failure artifacts

Use `--artifact-dir` to retain stdout/stderr for every selected process test:

```powershell
chtholly-test run `
  --manifest .\build-ninja\tests\chtholly-tests.generated.toml `
  --label cffi `
  --artifact-dir .\artifacts\chtholly-test
```

Each test receives deterministic `<test-name>.stdout` and
`<test-name>.stderr` files. Test names are sanitized to filesystem-safe names.
Artifact retention is best effort and never changes a test's exit status; JSON
and JUnit reports remain the authoritative result contracts.

## Migration rule

New tests must be registered in the generated manifest. Reuse the runner's
working-directory, environment, timeout, retry, and artifact options instead of
duplicating subprocess and log-capture code in individual tests. Existing
Python/C++ fixtures are migrated incrementally while preserving their public
test names and expected diagnostics.

The CFFI tool and manifest tests use `tests/chtholly_test_support.py` for their
shared command assertion contract. New Python fixtures should reuse this module
instead of defining another `subprocess.run` wrapper.

The same module exposes host-neutral target triples, executable discovery, and
native exit-code normalization. Fixtures must use these helpers rather than
assuming `*.exe`, Windows archive names, or a 32-bit process exit status;
Windows preserves full exit values while Unix reports the low eight bits.

The current Tier-1 CFFI inventory is itself a manifest-driven test subject;
`chtholly_cffi_tier1_audit` verifies that every declared provider header and
evidence marker remains present before the expensive native CFFI suite runs.

The product compatibility baseline is also a manifest test subject:
`chtholly_product_status_audit` validates
`support/chtholly-product-status.toml` against compiler constants, the
standard-library manifest, and documented evidence files. Run it directly when
changing release metadata or capability status:

```powershell
python scripts/product-status-audit.py --source-dir . --check
```

## Compiler artifact-cache evidence

`scripts/report-artifact-cache.py` is a read-only diagnostic for the compiler
artifact store.  It reports deterministic byte totals and reachability for
each CAS family, active/stale lease counts, and quarantine/reclaimed bytes.
Pass `--verify-references` to validate session-root/lease envelopes, canonical
sharded paths, and typed-index envelopes. Malformed or missing references fail
closed with `valid = false` and leave the namespace untouched so it can be
retried or recovered separately. This is an observational evidence scanner:
it does not duplicate the compiler's full binary manifest decoder or semantic
dependency verifier; authoritative GC remains responsible for that closure.

Use the same command after a cold build, a warm build, and an explicit GC run
to compare cache hits and unreachable/reclaimed bytes.  The report is
observational: it never deletes artifacts or treats source text as a fallback.
