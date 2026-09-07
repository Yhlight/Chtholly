# Next Artifact Hardening And Canonical Relocation Observations

## Boundary

`CHNXTPK44` version 44 extends public generic templates with ordinary function
types and structural concrete callee type arguments. Version 43 added the
explicit instance/associated member-function kind to the canonical nominal
member-owner identity. Canonical nominal and relocation-closure observations
remain intact. The package-check fingerprint is v8, the package-manifest
fingerprint is v11, and the cache namespace is `next-v26`. `CHNXTPK40`, unknown
future versions, malformed records, and incompatible artifacts fail closed. Public
function entity and interface fingerprints are v27 and v28. `CHNXSCC28`
version 26 and specialization component fingerprint v16 persist structural
concrete arguments for canonical public function-value targets. Nominal
artifact wire formats are unchanged. The inherent member-function surface uses
the ordinary function ABI; associated functions add no receiver and no CFDL
ABI.

Every Next artifact root decode now owns one explicit resource context. The
production limits are 64 MiB input, 1,000,000 cumulative logical records,
32 MiB cumulative string payload, 8 MiB per string, and 128 active
`PublicType` recursion levels. Container counts consume the node budget before
allocation. Manifest-embedded nominal definitions and embedded foreign resource
protocols inherit the parent context, so entering a nested decoder cannot reset
the budget. Exhaustion reports the stable `input-limit`, `node-budget`,
`string-budget`, or `recursion-budget` category before the surrounding
malformed-record diagnostic.

Package artifacts use the same explicit persistence boundary for CFDL Interop.
`interop-bundle` is optional for packages without foreign bindings, but when
present its SHA-256 and safe relative path are part of the canonical package
identity. Closure construction and archive extraction treat it as an ordinary
verified payload file. The compiler verifies the digest, decodes `CHNXIOP6`,
and atomically registers the bundle before semantic import resolution.

## Observation Model

A nominal observation stores the source-visible provider and binding used by
lookup plus the canonical package/module/name and definition fingerprint. A
foreign-resource relocation observation uses the same identities and commits to
the canonical nominal encoding. That closure includes hidden handle types,
invalid state, normalized protocol, ordered operation roles, and each target's
canonical package/module/raw C name and entity fingerprint.

Facade-local and session-local IDs never enter the artifact. Multi-hop exports
are compared by canonical identity. Diamond paths
to the same canonical nominal coalesce; equal visible names with different
canonical origins fail closed.

## Corruption Testing

Normal MSVC Next tests apply deterministic full-prefix truncation, magic and
version mutation, trailing bytes, oversized lengths, and stale/future schema
checks to valid package manifests. Valid specialization component, nominal
definition, specific, witness, and layout artifacts receive full-prefix,
trailing-byte, and magic mutation sweeps.

The optional Clang `chtholly_artifact_fuzz` target dispatches arbitrary
bytes across all six decoder families. A successful decode must verify,
re-encode deterministically, decode again, and preserve the canonical encoding.
Failed decodes must return without crashes or exceptions. The existing
`runtime-hardening` fuzz job runs short pull-request smoke sessions and 300-second
scheduled sessions, retaining failing inputs as CI artifacts.

Scheduled and manually dispatched fuzz jobs merge the checked-in corpus and
fresh six-family compiler-generated seeds with libFuzzer `-merge=1`, run the
result under the decoder-aligned maximum input size and an RSS cap, and upload
the minimized corpus even on success. Pull requests retain the short smoke run
and pass both seed sets directly. CI never writes the minimized result back to
the source tree; reviewed inputs can be promoted manually. A local merge uses:

```sh
chtholly_artifact_seed_generator generated
chtholly_artifact_fuzz -merge=1 minimized \
  tests/fuzz/corpus/chtholly_artifact generated
```

Use `-minimize_crash=1 minimized-crash crashing-input` to reduce a reproducer.

## Bounded Parallel Loading

Next artifact reads are coordinated by one driver-owned executor for the whole
build. `--jobs=1` preserves inline loading; parallel builds use at most four
artifact workers and a bounded queue. Independent object and nominal-witness
reads may overlap, and identical specialization request fingerprints share one
in-flight result. Specialization closure traversal remains synchronous inside
its owning task so a worker never waits for nested work in the same pool.

The executor returns batch results in request order. Registry insertion,
materialization, invalidation decisions, diagnostics, and cache statistics stay
on their existing stable worklists. Missing and corrupt entries retain their
family-specific rebuild behavior; fatal errors are observed by the first
stable consumer rather than whichever I/O finishes first. Cancellation rejects
queued tasks; active reads drain and their results are discarded before
teardown.

An artifact lease outlives every executor task. The driver drains and destroys
the executor before publication, garbage collection, or lease reset, so lease
activity remains single-owner state. No artifact wire version or fingerprint
encoding changed for parallel loading.

## Loading Metrics And Baselines

`--dump-artifact-load-metrics <path>` enables the versioned
`chtholly-compiler-artifact-load-metrics-v1` report. It is separate from
`--dump-metrics`, whose session-memory schema remains unchanged. When the
option is absent, the executor and store do not read clocks or allocate
per-request observations.

The report records the configured jobs, worker and queue bounds, terminal task
counts, queue and active-worker high-water marks, backpressure, aggregate queue,
execution and stable-consumer waits, and specialization in-flight/result reuse.
It also records the first-submit-to-last-terminal artifact span and package-query
worker count, active high-water, wait, execution, wall, and dependency critical
path. Package scheduling uses the dependency DAG's exact maximum antichain as
an upper worker bound; a single-worker graph runs inline, and one caller
participates in wider graphs.
Specialization closure observations include outcomes, component and edge counts,
maximum depth, bytes, exists/read/decode/fingerprint-verify time, total DFS time,
component work, critical path, and integer-scaled available parallelism. Timing
is observational only and never contributes to manifests, fingerprints,
diagnostics, or cache decisions.

Warm object and specialization loads use a single-open artifact read. Its
private result distinguishes found bytes, a missing path, and every other I/O
error; an empty found buffer still reaches artifact validation and is corrupt
rather than missing.

The compatible v1 report includes an `artifact-io` object with read attempts,
found/missing/error outcomes, bytes, read time, and metadata probes. Object,
specialization-index, and specialization-component reads contribute to these
artifact-wide counters. The older specialization `exists-nanoseconds` field is
retained and is zero on the single-open path. Publication conflict checks,
lease creation, and garbage collection remain later store phases and are not
counted as warm artifact loads.

The executor is cancelled when package-query execution fails, then drained and
destroyed before either successful or failed metrics are written. Publication,
GC, and lease retirement remain later phases. This preserves deterministic
first errors while retaining failed-build contention evidence.

`chtholly_artifact_load_benchmark` supplies fixed independent, wide, deep,
shared-dependency, and recursive-SCC workloads at jobs 1, 2, 4, and 8. It emits
raw observations plus minimum, median, and maximum values. `chtholly-test` runs one schema
and determinism smoke repetition; it has no wall-clock pass threshold. Use
`--repetitions 7` or more for local measurement.

`warm-artifact-sample.py` drives the v1 CLI against three checked-in
Next-only workspaces: a multi-module generic package, a four-package diamond,
and a seven-package shared-component fanout with a package frontier of four.
Every observation starts from an
isolated copy, performs a cold build and unchanged warm build, changes the root
implementation, then verifies the emitted program observes the change. The
default matrix uses jobs 1, 2, 4, and 8 with seven repetitions and reports
medians plus raw CLI wall samples.

The first real-workspace matrix showed that short object batches spent more
time waking artifact workers than reading their objects. Object batches no
larger than the configured worker count therefore use a stable work-first path
on the package worker. Larger batches retain bounded parallel loading. This
removed the measured short-batch queue bottleneck without changing diagnostic
order, cancellation behavior, artifact schemas, or lease ownership.

The subsequent single-open matrix removed four metadata probes from every
sampled closure: one request index and three unique component files. The two
workspaces performed eight and nine total artifact reads per incremental build,
respectively, with zero missing/error outcomes and zero metadata probes.
Comparable specialization I/O medians (old `exists + read` versus the new
single read, in microseconds) were:

| workspace | jobs 1 | jobs 2 | jobs 4 | jobs 8 |
| --- | ---: | ---: | ---: | ---: |
| multi-module | 578.8 -> 368.1 | 669.1 -> 549.2 | 607.5 -> 602.1 | 680.4 -> 411.0 |
| package-diamond | 1018.2 -> 377.8 | 540.0 -> 494.7 | 740.6 -> 493.1 | 667.1 -> 429.2 |

The new artifact-wide I/O time also includes object reads and therefore must
not be compared directly with the earlier specialization-only `io` column.
The deterministic probe reduction and seven improved-or-flat comparable
medians justify the change without a wall-clock threshold. Every closure still
has three unique components and 1.0x available parallelism. An unbounded byte
cache, mapped-file layer, packed closure format, and non-blocking closure graph
remain deferred until a representative workload demonstrates repeated paths
or enough closure parallelism to pay for those contracts.

The seven-package fanout then supplied representative repetition. Twelve
distinct `relay<i32>` requests share one `identity<i32>` component across four
leaf packages. A no-cache seven-repetition matrix observed 24 logical component
requests, 13 unique fingerprints, and 11 duplicates at every jobs point. The
45.8% duplicate share passes the explicit cache gate of at least eight repeated
requests and at least 20%. The original workspaces each have only one repeated
request and do not independently pass the gate.

Each `NextArtifactLease` now owns a single-flight cache of immutable verified
component bytes keyed by component fingerprint. Only a complete read, decode,
and fingerprint match is admitted. Hits and coalesced waiters decode and verify
again, so mutable semantic objects and decode budgets are never shared. Missing,
corrupt, and I/O-error results are not cached. LRU admission is bounded to 128
verified entries and 16 MiB of encoded bytes; capacity evictions and bypasses
are observable. Publication and GC still occur only after executor drain and
lease retirement, preserving the lease's immutable store view.

The post-cache seven-repetition matrix reduced the fanout workspace from 43 to
32 artifact reads at jobs 1, 2, 4, and 8. All 11 duplicate requests became cache
hits, duplicate disk reads remained zero, and component misses remained the 13
unique fingerprints. Median artifact I/O time changed from 3649.9/4307.2/
4256.2/4476.1 microseconds to 2782.3/4160.0/3579.8/3218.6 microseconds for jobs
1/2/4/8. Artifact span remained noisy and did not improve at every point; the
sampler still classifies I/O as dominant. The non-blocking closure graph gate
therefore remains closed. Next measure the remaining specialization-index and
object small-file reads before considering a versioned packed closure.

Concrete specialization loading now has an explicit ownership consistency
check in addition to byte decoding and component fingerprint verification.
Generic nodes are reanalyzed from bottom and their persisted initialization
summary is treated as an expected fact; a decodable summary mismatch is a
failed cache load, not a cache hit. The request fingerprint still derives from
the public template identity and concrete arguments, while the component
fingerprint includes the concrete ownership summary and transitive component
closure. Cross-package generic wrappers, warm-cache reuse, and summary
tampering are covered by the package artifact interop test.

This milestone changes no supported syntax, ABI, artifact schema, or
fingerprint encoding.
