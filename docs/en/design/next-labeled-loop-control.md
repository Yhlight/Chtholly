# Next Labeled Loop Control

Labeled loop control is a post-v1 structured-control extension. The source
spelling is `label: while/for/do` with `break label;` and `continue label;`.
Labels are lexical loop bindings only; they are absent from public interfaces,
callable contracts, CFDL artifacts, and ABI records.

The parser emits a source-only `LoopLabel` node. Semantic checking resolves
each transfer before creating SemIR. `SemBreak` and `SemContinue` carry a
session-local non-negative loop distance: zero is the nearest loop and larger
values select enclosing loops. This keeps source names out of persistent IR
while making target resolution explicit and verifiable.

Ownership CFG, place-state analysis, and LowIR all consume the resolved target
distance. Each phase keeps an explicit loop-target stack, selects the matching
cleanup depths and branch destinations, and verifies that the target is an
enclosing loop. `for` continue targets its step block; `while` and `do` target
their condition blocks. Reverse-order lexical cleanup and task-scope draining
therefore apply identically to labeled and unlabeled transfers.

Because older generic artifacts encoded loop control without a resolved target,
package state advances to `CHNXTPK66`, concrete components to `CHNXSCC43`, and
the driver cache namespace to `next-v34`. The wire layout and public ABI fields
are unchanged; the epoch change prevents stale target-less records from being
consumed.