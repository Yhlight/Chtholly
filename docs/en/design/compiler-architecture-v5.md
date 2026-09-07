# Compiler Architecture V5

Status: superseded by `compiler-architecture-v6.md`.

Architecture V5 retains V4's phase and carrier-path contracts while replacing
ownership full scans with deterministic sparse worklists.

## Analysis Metrics Boundary

`--dump-analysis-metrics` emits the versioned
`chtholly-compiler-analysis-metrics-v1` schema. Each source unit reports callable
ownership CFG size, work-item and state-change counts, SCC evaluations,
widening, queue depth, PlaceState work, unique widened loan regions, and
elapsed phase time. Counts are deterministic correctness and performance
evidence. Timings are diagnostic and never participate in artifact identity or
CI correctness comparisons.

## Sparse Ownership Solvers

Callable provenance records both CFG successors and SSA/control-result
consumers. A changed instruction result enqueues only its consumers; changed
flow state enqueues only CFG successors. Postconditions use a separate forward
queue and canonical monotone state. Return states are merged after convergence.

Callable SCCs use reverse call edges. Every definition is initially dirty, and
a summary change enqueues only callers in the same SCC. Imported summaries are
immutable. Queue storage uses stable integer IDs and an `in_queue` bitset; code
does not retain references across an enqueue that can reallocate storage.

PlaceState loan propagation uses the same producer/consumer and CFG-successor
separation. Liveness runs backward and enqueues only predecessors of changed
state. Projection-sensitive carrier loans and mutable-return conflict rules are
unchanged. Repeating the same projected region segment on a loop widens once to
an absorbing root region instead of rebuilding the segment until the general
path limit is reached.

## Convergence And Widening

Callable solvers use the deterministic safety budget
`max(4096, 64 * (nodes + edges + 1))`. PlaceState liveness uses
`max(4096, 64 * (nodes + 1))`; loan propagation also includes the bounded
region lattice height in its budget. Callable analysis reaches a fixed point
without widening on the 1.9 and telemetry gates. PlaceState reports its
conservative absorbing loan-region widenings explicitly. Region paths retain
the 256-component root fallback. Provenance, SCC, and PlaceState budget
exhaustion is a compiler diagnostic rather than silently accepting partial
analysis; postcondition exhaustion retains the existing conservative
all-outcomes widening and is rejected by the 1.9.1 performance gates.

No language syntax, SemIR opcode, machine ABI, component ABI, or runtime ABI is
added. Inferred ownership summaries changed, so semantic artifact epoch 16,
compiler contract 13, and cache namespace `next-v37` fail closed on old facts.
