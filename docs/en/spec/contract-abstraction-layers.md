# Chtholly Contract Abstraction Layers

Status: Next-only boundary direction.

Chtholly has one generic semantic model for values, calls, cleanup, and
obligation-bearing values. CFDL is a separate binding language that supplies
irreducible external ABI and resource facts. It is not a Chtholly source mode
and does not add an opaque contract escape hatch to ordinary Chtholly code.

## Layers

| Layer | Owner | Source visibility | Responsibility |
| --- | --- | --- | --- |
| Chtholly language | Chtholly author | ordinary syntax | values, modules, calls, generic resource semantics |
| CFDL binding | binding author | CFDL only | C ABI, flow facts, loans, obligations, external events |
| normalized artifact | compiler | hidden | canonical facts, identities, fingerprints, epochs |
| Interop backend | compiler/runtime | hidden | ABI classification, callbacks, completion, linkage |

The Chtholly layer does not contain `extern`, `bind`, CFDL operation objects,
or FFI-specific callable contracts. Imported CFDL functions appear as ordinary
module entities. The compiler attaches an opaque imported-binding reference and
consults the CFDL artifact only at the Interop boundary.

The Interop boundary is implemented by the `next::interop` artifact namespace.
Foreign operation, callback, and completion metadata is canonicalized and
verified there. `ArtifactRegistry` owns payload storage for a compilation
session, while `PublicInterface` and SemIR retain only stable
`ArtifactReference` identities. Session-local interop IDs are compiler
implementation details and cannot enter serialized fingerprints.

## Generic Chtholly Semantics

Ordinary Chtholly definitions may continue to infer generic callable effects
and cleanup facts. Those facts are language semantics, not FFI contracts. A
CFDL artifact may supply an obligation-bearing imported value, but Chtholly
source cannot replace or weaken the artifact's external facts with an inline
contract. For a callable whose implementation is supplied elsewhere, a
bodyless Chtholly declaration may use the existing `contract {}` block to
publish compiler-checked reads, writes, takes, borrows, postconditions, and
borrowed-return provenance. This block is declaration-side ownership metadata;
it is not a second effect statement language inside a function body. The
source spelling remains deliberately separate from CFDL's `where` relations.

## CFDL Facts

CFDL source supplies only finite `where` relations:

```text
escapes, stores, derives, invokes, obliges, discharges, requires, until
```

The compiler normalizes these into resource, loan, derive, obligation,
protocol, event, and ABI facts. Source cannot define new lifecycle proof rules,
state-machine syntax, or named fact bundles. Repeated patterns are represented
by repeated ordinary foreign declarations and normalized by the compiler.

## Artifact Rules

Artifacts store canonical type and function identities, target-neutral ABI
signatures, external symbols, normalized resource facts, and compatibility
epochs. They do not store source spelling, session-local IDs, or raw Chtholly
AST references. Unknown or incompatible artifact epochs fail closed.

The artifact boundary follows the same construction-then-verification policy as
the rest of Next. Callback, completion, quiescence, and exactly-once cleanup
plans remain backend projections; they are never exposed as Chtholly syntax.
