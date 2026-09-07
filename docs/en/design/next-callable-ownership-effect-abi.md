# Callable Ownership And Effect ABI

## Boundary

Every callable with a body has one inferred `CallableOwnershipSummary`. The
summary is a semantic ABI fact, not an optimization hint. It is stored for
ordinary public functions, generic templates, concrete generic specifics, and
canonical imported functions. No unrelated artifact record is consulted when
the fact is missing or malformed.

The summary contains:

- `Read`, `Write`, `Take`, `BorrowShared`, `BorrowMutable`, and `Initialize`
  effects;
- a parameter root followed by logical field, static element, or dynamic
  element steps;
- an optional half-open bit range for bit-packed logical fields;
- either an owned return or a canonical set of parameter-region return
  provenance sources.

Paths use logical nominal field indices. They do not serialize storage offsets,
session-local IDs, physical GEPs, or source parameter names. Canonicalization
sorts and deduplicates facts and removes a child when an equal-kind parent fact
already covers it. Dynamic array elements use one wildcard step.

## Inference

The compiler builds an instruction CFG for each callable. A forward fixed point
propagates parameter regions through SSA values, reference local bindings,
reference assignment, dereference, field/index projection, and borrowed call
results. This makes `let alias = parameter; return alias` equivalent to a direct
return without relying on source-order recursion.

Local call edges are decomposed with Tarjan SCCs. Components are solved
callee-first; members of a recursive component iterate over the monotone summary
lattice until stable. Imported summaries are immutable boundary facts. Region
paths are limited to 256 steps, matching artifact validation, so a recursive
projection cannot allocate an unbounded path sequence. Overflow or failure to
stabilize is a semantic error.

Direct operations and callee summaries contribute to the caller summary. A
callee region is composed onto every provenance region of its actual argument.
Return provenance is similarly composed, so an erased reference return may
retain multiple possible sources.

## Call-Site Consumption

Summary inference runs before final place-state construction. Effects cross a
call boundary only through reference parameters; effects on a by-value
parameter describe the callee's local copy and never consume or reinitialize
the caller a second time. `Take` moves the mapped caller region. `Write` adds an
initialized possibility without erasing an already possible moved state,
because the effect set records possible accesses rather than path-sensitive
postconditions. `Initialize` accepts only an uninitialized caller place and
requires an exact initialized postcondition; it cannot reinitialize live
storage. Read and borrow facts become explicit observations.
Dynamic-element effects conservatively update the containing tracked array
while retaining an exact wildcard in the portable ABI. Fixed-array static
elements and tuple positions are independent leaves. `Slice + AnyElement` is
the only valid slice projection and denotes a borrowed view; slice `Take` and
`Initialize` effects, and initialized postconditions, are invalid. This keeps
the slice ABI non-owning without adding a new wire field or semantic epoch.

After summaries converge, loan analysis propagates a call result only from
`return_provenance`; the older single parameter encoded in a reference type is
not consulted. Every source is retained. Projected return sources create a loan
over the mapped physical carrier path and optional bit range.
Derived call-result loans are interned by canonical physical region. Projection
chains longer than 256 steps collapse to the conservative owner root, keeping
loop fixed points finite without dropping an overlap.

Live-loan conflicts consume all six call effects. The loan supplied as the
callee's reference capability is excluded from self-conflict, while other live
overlapping loans remain visible. LLVM receives only the already validated
place-state and LowIR decisions.

Postconditions are persisted separately from access effects. `Write` remains
conservative, while `Initialize` carries the exact `CallableOutcomeInitialize`
postcondition required to establish a live place. Current source-body inference
does not manufacture this authority from an ordinary assignment; verified CFDL
and imported artifact contracts provide it. Ordinary source inference now
composes `SemForeignOperationCall` summaries through local wrappers using the
same SCC fixed point. Place-state analysis tracks a pending initialization
capability for wrapper reference parameters and rejects reads or copies before
the initialized postcondition is observed. This extension requires no new
source syntax, ABI lane, or artifact field.

## Persistence And Verification

`CHNXTPK17` state version 17 and `CHNXSCC10` component version 8 encode the
summary, its declaration-level contract, and its controlled helper contract.
The specialization component
fingerprint domain is version 4. Older
versions fail closed.

The summary contributes to public entity fingerprints and therefore to import
observations, package invalidation, specialization requests, component
fingerprints, and native object identity. Decoders reject non-canonical facts,
bad indices, invalid wildcard steps, malformed bit ranges, and reference/owned
return disagreement.

Public registration then performs a typed region pass over the complete
nominal closure. A field step must resolve the referenced nominal definition
and its exact fingerprint, generic field types are substituted before the next
step, array steps check their kind and static bound, and a terminal bit range
must exactly match the selected bit-packed field. This prevents an imported
artifact from encoding a field on a scalar and relying on call-site mapping to
silently drop the effect. Concrete components reject independently decidable
scalar and array errors during decoding; cache loading repeats the complete
registry-aware check before materializing a specific.

Compiler-reserved representation, projection, object-shell, and lifecycle
callables use a separate logical-owner region domain. Their summaries are
checked while SemIR still retains the semantic owner and role, then persisted
under reserved canonical identities. They are deliberately excluded from the
ordinary public-signature typed pass: a private owner is not part of the public
nominal closure, and treating its logical field path as a projection of the
helper's physical carrier parameter would be incorrect. This separation does
not make a reserved helper callable from source.

A cached concrete specific retains its decoded summary as an expected fact.
Before inference, all local summaries are reset to the lattice bottom. The body
is reanalyzed and the converged result must equal the persisted summary. This
prevents a tampered summary from seeding a fixed point that validates itself.
`SemFunction` remains a 40-byte relocatable record; owning summary vectors live
in aligned SemIR side tables.

Generic specialization uses the template summary only as a contract-shaped
source fact. Concrete type arguments are substituted before the specific body
is analyzed; the concrete summary is then inferred from the materialized body.
The persisted node summary must equal that result, including an `Initialize`
effect and its initialized postcondition. The request fingerprint commits to
the public template entity fingerprint and concrete arguments, while the
component fingerprint commits to the encoded concrete summary and downstream
component fingerprints. A cross-package generic wrapper therefore cannot reuse
a component after its initialization behavior changes. The focused interop
fixture covers a generic-wrapper-to-generic-wrapper-to-CFDL chain, a warm-cache
hit, and a structurally valid but summary-tampered component rejected after
reanalysis.

## Deferred Surface

There is no explicit effect declaration syntax in this milestone. Bodies are
inferred and artifacts are verified. A later declaration-only or foreign ABI
surface must describe the same canonical facts rather than introduce a second
model.

Ordinary calls to representation helpers remain closed. The completed
role-restricted helper ABI is specified in
`next-controlled-representation-helper-call-abi.md`; it consumes this summary
without granting source access to `pack`, `init`, projector, shell, or raw
carrier operations.
