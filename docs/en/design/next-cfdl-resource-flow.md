# Next CFDL Resource-Flow Architecture

Status: active Next design and implementation status. This document is
subordinate to the normative source and artifact contract in
`docs/spec/cfdl.md`; it records closure work and evidence rather than adding
source syntax.

## Current Baseline

Implemented and covered by focused tests:

- phase-owned parsing, carrier completion, callable validation, resource-flow
  normalization, and immutable `CHNXIOP6` artifact registration;
- stable `ArtifactReference` identity with session-local payload resolution;
- fail-closed rejection of unknown qualifiers, malformed places, duplicate
  relations, and incompatible artifact bundles;
- ordinary Chtholly imports over the Interop artifact boundary.
- source-driven Interop publication for each foreign callable and nominal
  foreign identities with incomplete, integer/pointer, or record carriers.
- complete typed relation payloads retained as canonical Interop capability
  literals, with publication keyed by the session package/module/name.
- canonical cross-package action/event identities and a finite protocol-edge
  graph verified by the session-owned registry.
- qualified imported protocol references: `module::action` and
  `module::callable::event` resolve through verified provider artifacts, while
  consumer artifacts retain external fingerprints rather than copying
  provider identities.
- imported foreign nominal types resolve by direct name from an imported
  public interface and are materialized through the SemIR public-type bridge;
  canonical provider identity and fingerprint remain authoritative.

The Interop reference schema is version 5 and the bundle is `CHNXIOP6`. The
CFDL semantic epoch is version 9. Package Artifact v12 and foreign layout epoch
11 close the same representation boundary; older sidecars fail closed.

Completion operations now carry a canonical event set in their Interop
artifact. Every locally declared event must have an `Invokes` edge, while
cancel and wake events are projections of that same set. A quiescent
requirement is accepted only when the artifact records a discharge; stale
epoch-4 sidecars are rejected before import. The surface remains typed
`where` relations, so a callable may compose `complete`, `cancelled`, `wake`,
and quiescence facts without introducing a second protocol DSL.

The current closure includes canonical typed relations, separated foreign
identity/carrier ABI, package-level obligation/discharge validation,
quiescent-action validation,
and a source-driven `.cfdl -> Interop -> Chtholly import -> LLVM` regression.
Ordinary source calls now consume the artifact as `SemForeignOperationCall`;
`let place: T;` or `var place: T;` plus an ordinary `place` argument is tracked
by PlaceState, while compiler-internal `InitializePlace` carries the ABI
address. The provider contract initializes the place on every published
outcome. The std::host driver fixture executes IO, time, and task operations
through this path.

Initialization effects now compose through ordinary source wrappers. A mutable
reference parameter forwarded to a foreign `Initialize` callable publishes the
same parameter-root effect after the local SCC summary converges. PlaceState
tracks the forwarded parameter as pending until the inner postcondition is
observed; pre-initialization reads and reference copies fail closed. The
physical reference lane is unchanged; carrier publication uses the epoch-8
artifact contract.

Provider queries publish a provider-filtered `CHNXIOP6` sidecar to the Next
artifact store. Its path is derived from package contract, target, and the
current semantic epoch; its digest is checked on every load. A dependent query
uses the completed provider result in the same graph, and a later invocation
reuses the deterministic sidecar before semantic compilation. The sidecar is
not a substitute for the provider public-interface artifact.

No item in the closure list licenses operation-object syntax or a new FFI
contract surface.

## Phase-Owned Compilation

CFDL uses explicit phase-owned state:

```text
lex -> parse -> elaborate/check -> normalize -> publish
```

Each source file has a stable compilation-unit ID and a checker context. The
parser produces syntax data without resolving Chtholly entities. Elaboration
resolves physical types and names through an injected binding environment.
Normalization converts source flow facts into canonical resource, loan,
derive, obligation, event, and ABI facts. Publication writes immutable,
fingerprinted artifacts and never serializes session-local IDs.

## Resource/Obligation Facts

The normalized resource state is:

```text
Resource = {
  identity,
  validity,
  obligation_carrier,
  loans,
  derives,
  protocol_constraints,
  events,
  executor_affinity,
}
```

`owned` creates an obligation carrier. `move` transfers the carrier. `ref` is
a loan of resource identity. `view` is a loan of a data projection. `derive`
creates a dependent validity edge and does not imply exact storage aliasing.
An obligation is discharged by a later foreign declaration or by generated
cleanup. Quiescence is a requirement on a discharge action, not a resource
kind.

## Artifact Boundary

Chtholly source imports the ordinary public signature from a CFDL artifact.
The artifact's Interop section owns ABI layouts, external symbols, and resource
facts. A compilation session constructs an `ArtifactRegistry` before semantic
checking; public and semantic records retain stable `ArtifactReference`
identity, and LowIR resolves the session payload before LLVM lowering. The
reference wire form contains no session-local ID or operation payload. No
source-level `extern`, `bind`, or FFI contract is needed.

## Rejection Policy

The frontend fails closed for unknown flow qualifiers, unsupported physical
types, unresolved endpoints, incomplete obligation discharge, ambiguous raw
pointer adoption, and artifacts from incompatible epochs. It never silently
chooses a lifecycle interpretation from a C symbol name.
