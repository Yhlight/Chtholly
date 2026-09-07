# Build, Cache, And Cross-Package Performance

The reproducible baseline command is:

```powershell
python scripts/build-performance-baseline.py `
  --chthollyc build-ninja/tools/chthollyc/chthollyc.exe `
  --source-dir . `
  --output build-ninja/tests/build-performance-baseline.json `
  --repetitions 3 --jobs 1 2 4
```

The report schema is `chtholly-build-performance-baseline-v1`. Each point
records cold, warm, and source-mutated incremental builds, compiler wall time,
artifact-load metrics, package scheduling, specialization closure, artifact
I/O, and cache verification. The package-diamond fixture provides a stable
cross-package workload; `warm-artifact-sample.py` remains the broader fanout
and parallel-closure experiment.

The report records `source_commit`, `target`, and `cache_mode` with the
observations so measurements can be compared only across the same compiler
revision, host target, and cold/warm/incremental protocol.

Each build sample also records optional `peak_rss_bytes` and
`peak_rss_source`. The source is `proc-status` on Linux or
`get-process-memory-info` on Windows; hosts without either platform API emit
`peak_rss_bytes: null` with `peak_rss_source: "unsupported"`. The baseline
driver samples the compiler child every 20 ms and retains the maximum. RSS is
observational evidence, not a hard threshold.

The report's `diagnostic_counters` are derived from the existing JSONL check
records: `related_note_count`, `unavailable_location_count`, and
`quick_fix_count`. Counting these records does not alter diagnostic codes or
semantic behavior. Keep the counters with the same compiler version, source
commit, target, and cache mode when comparing runs.

These reports are descriptive evidence, not machine-specific absolute gates.
Compare medians on the same host/toolchain and retain the compiler version,
target, source commit, and cache configuration with the report. A failed build
must never be admitted as a cache hit or publish a partial artifact.
