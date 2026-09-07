# Compiler Architecture V4

Status: superseded by `compiler-architecture-v5.md`.

Architecture V4 retains the phase boundaries of V3 and closes three protocol
and ownership gaps exposed by collection composition.

## Dependency-Owned Witness Resolution

`SemanticObligationWorklist` owns deterministic Deferred, Resolved, and Failed
states for dependent witness constraints. Constraint-list parsing temporarily
owns the active query node; an associated projection can observe inherited and
already resolved constraints without recursively parsing the same list.

## Structured Ownership CFG

Callable ownership includes Switch arm bodies in the function CFG and carries
provenance through Yield, ScopedBlock, If, and Switch results. This makes
value-producing blocks and continuation-based iterators part of the same
interprocedural analysis instead of treating nested arms as opaque.

Projection paths are bounded at 256 components. Reaching the bound widens to
the parameter root. Postcondition fixed points use the same bounded budget and
widen an unstable parameter to all outcomes. The widening is conservative: it
cannot manufacture an Initialize guarantee.

## Projection-Sensitive Loans

PlaceState stores the return carrier path beside every derived loan. Aggregate
and enum projections consume matching path components, so sibling return
carriers no longer coalesce by physical owner region alone. Mutable return
provenance is checked as borrow formation while supplied continuation loans are
excluded only from their own operation.

No runtime field, machine ABI lane, source lifetime, or new SemIR opcode is
introduced. The semantic epoch changes because existing carrier-path facts now
have stronger compiler-enforced meaning.
