# Compiler Architecture v2

This document defines the replacement compiler architecture. It is an internal
implementation contract for the Next pipeline and its module surface. It does
not change the native ABI.

## Pipeline

The replacement pipeline is:

```
SourceBuffer -> TokenBuffer -> ParseTree -> SemIR -> LowIR -> LLVM
```

Each compilation unit owns every stage result. Cross-unit references use typed
IDs tagged with the producing unit. A stage may refer to an earlier immutable
stage, but it may not write semantic results back into tokens or parse nodes.

SemIR is the only semantic fact source. LowIR expands control flow, storage,
initialization, ownership transfer, and cleanup for LLVM lowering. Stable ABI,
cache, and interface identities are deterministic projections of canonical
SemIR records rather than a second semantic model.

## Compiler Techniques

| Chtholly use |
| --- |
| prevent accidental cross-store and cross-stage handles |
| dense ownership and explicit interning |
| variable operands with compilation-unit lifetime |
| one closed list for enum, spelling, dispatch, and coverage |
| predictable memory and cache behavior |
| reject compiler invariant failures at their owner |
| typed production with sortable text/JSON output |
| make allocation regressions reviewable |
| canonical identifiers, decoded strings, and integers across units |
| parse all units, validate imports, then check providers first |
| keep session-local IR identity out of public and persistent identities |
| map imported bindings to session-level public entity IDs instead of rebasing foreign local IDs |
| retain a facade lookup edge while following multi-hop exports to one canonical nominal and relocation closure |
| keep callable identity independent from workspace source availability, between semantic entities and `AbsoluteNodeRef` locations |
| enumerate the resolved nominal member table, `NameScope` discipline instead of scanning source text or rebuilding lookup in the language server |
| derive public entity and interface identities from canonical semantic content |
| propagate whole imported interfaces while preserving one canonical entity identity |
| invalidate consumers from semantic facts actually read instead of a producer-wide timestamp |
| distinguish the requested root's transient IR outputs from dependency-only units |
| link every selected package module while keeping root output selection explicit |
| serialize public template signatures separately from definition instructions and resolve each specific region independently |
| reuse one session-local specialization for each canonical generic argument list |
| publish a specific declaration before queueing its definition so recursive calls reuse the in-progress entity |
| drain a deterministic, bounded queue after source bodies are checked instead of recursively cloning definitions on the call stack |
| serialize mutually recursive concrete specifics as one immutable component and fingerprint its callee components bottom-up |
| reconstruct verified concrete SemIR bodies from portable artifacts while retaining package-local IDs and LLVM contexts |

The instruction registries also define each operand's ID domain. Typed
instruction views are generated from those registries and are the only
business interface used by semantic construction and lowering. The compact
16-byte records remain an internal storage, verifier, printing, and metrics
format; consumers must not decode their raw integer operands.

Semantic checking is organized around a per-unit check context and typed node
handlers. Lowering contexts isolate cross-function declarations from
per-function CFG state, slots, and emitted values.

Arena allocation is not a general ownership substitute. It is limited to data
whose lifetime is exactly the owning compilation unit and whose destruction is
trivial. Fixed-size records remain in contiguous stores.

## Compilation Sessions

`CompilationSession` owns cross-unit compilation. It assigns each registered
source a `CheckIRId`, owns the shared canonical value stores and
`PublicInterfaceRegistry`, validates module names and direct imports, and
checks units in stable topological order. A `CheckIRId` is meaningful only
inside that session and is never serialized or hashed. Every source declares
its module identity with a leading `module path;`; filenames and driver
arguments do not synthesize module names.

The command-line Next pipeline accepts either a direct source or a package graph
selected from a project or workspace. It uses the common build-plan resolver,
collects each package's `.cns` and `.cfdl` files from its module roots, and creates one
isolated session per package. Direct dependencies are visible to source import
lookup; transitive manifests are implementation-only template closure inputs.
Incompatible artifact stores remain rejected at this boundary. The manifest
`build.entry` selects the root module whose LLVM, object, and debug dumps are
requested. Executable builds link native objects from the selected package
closure and require `main` only in the requested root module.

The public interface registry assigns dense `PublicInterfaceId` and
`PublicEntityId` handles for the session. A `PublicInterface` is a set of
public name bindings; each binding maps directly to its canonical public
entity. Import tables contain `PublicInterfaceId` handles and binding IDs,
never pointers into another compilation unit and never raw foreign SemIR IDs.
The old declaration-oriented interface names have no compatibility aliases,
because a re-export binding is not a declaration owned by the forwarding
module.

Public entities and interfaces carry SHA-256 fingerprints over versioned,
length-delimited canonical byte streams. Entity fingerprints include the
canonical module, entity kind, name, parameter types, and return type. A public
generic entity additionally commits to its portable template definition and
canonical callee identities because consumers instantiate that body.
Interface fingerprints sort exported bindings by name and include their
canonical entity identity and fingerprint. Session IDs, local declaration
order, source locations, non-generic implementation bodies, addresses, and
container iteration order are excluded.

The initial import contract is deliberately narrow:

```cns
import dependency;

fn main(): i32 {
  return dependency::add(20, 22);
}
```

An entire public interface can be forwarded explicitly:

```cns
export import dependency;
```

This adds the dependency's bindings to the forwarding module without creating
wrapper functions or new public entities. Diamond paths to the same name and
canonical entity coalesce. The same exported name resolving to different
canonical entities is an interface construction error. A plain `import`
never forwards names.

Only explicitly `pub` functions and nominal definitions are visible. SemIR calls use a
`FunctionRefId`; imported records contain an `ImportIRInstId`, canonical
`PublicEntityId`, and a structurally mapped local function type. LowIR preserves
that reference unchanged. Verifiers require an external producer to be a
direct import and check visibility and structural signature compatibility against
the registry entity.

LLVM definitions and imported declarations use the same length-delimited,
hex-encoded package/module/function symbol. Public definitions have external linkage;
private definitions remain internal. Imported functions are declarations in
the consumer object. Source `main` still owns hosted entry wrappers and is
never importable.

## Incremental Compilation Artifacts

Every successful package session produces an immutable
`NextPackageArtifactManifest`. Stable module and entity identities are
`(package_name, module_name)` and `(package_name, module_name, entity_name)`.
Each package module artifact owns its source
fingerprint, a complete `PublicInterfaceArtifact`, semantic observations,
and an object fingerprint bound to the compilation configuration. Native
object bytes are stored separately in a content-addressed object store and are
never embedded in the state. The state contains stable strings, enum values,
and fingerprints only; no session-local `CheckIRId`, arena allocation, pointer, or
transient compiler IR crosses the boundary.

Artifact decoding uses an explicit Next-only resource context shared by the
package manifest, its embedded nominal definitions, and nested foreign-resource
protocol decoders. It bounds input bytes, cumulative logical records,
cumulative and individual string allocation, and active type recursion before
container allocation. bounded state is owned by the operation rather
than inferred from the native call stack. The binary formats, fingerprints,
and cache namespace do not vary with machine-local configuration.

The driver captures one immutable `NextRequestSnapshot` before package
scheduling begins. Its build-control half owns exact bytes or missing markers
for workspace/package manifests, the lockfile, and runtime toolchain manifests,
plus fingerprints for the resolved package graph/features, compile toolchain,
and link toolchain. Its source half owns every normalized Next source path and
shared read-only bytes. Domain-separated, length-delimited fingerprints commit
to both halves. Package workers never read source files; they construct
compilation units from snapshot-backed `SourceInput` values.

The input filesystem is injected through `NextCompilerEnvironment`. `NextOverlayInputFileSystem` accepts `.cns` and `.cfdl` additions,
replacements, and tombstones. Each edit returns a new immutable generation, so
concurrent daemon/LSP requests can retain their prior generation. Build-control
files are not overlayable and continue to come from the real filesystem. 

Standard-library resources are a format-1 source distribution under
`share/chtholly/stdlib`. The strict
manifest owns package `std`, compiler-contract/API epochs, and the complete
module inventory. Parsed explicit `std::*` imports add a toolchain-owned `std`
package query; no prelude is injected. Manifest paths and source content form
the distribution fingerprint persisted as `CHNXTPK42` toolchain provenance.
The manifest and sources participate in the same immutable request barrier as
workspace controls and sources.

The package manifest format has a deterministic `CHNXTPK42` binary encoding
(format version 42). Each module records its compilation-unit kind, and source
fingerprints use separate domains for Chtholly and foreign-binding input.
It records the package name, package provenance and contract fingerprint,
target/configuration, compile-toolchain
fingerprint, resolved feature set, direct dependency names and exact manifest
fingerprints, module artifacts, package-qualified observation edges, and sorted
concrete specialization request-to-component references, nominal
specific/layout request-to-result references, and declared callable
definition/foreign state, unsafe requirements, foreign ABI, ownership effects,
and return provenance. The
compile-toolchain fingerprint commits to the compiler/codegen revision, target,
ABI, optimization/debug policy, sysroot, and object format; a change invalidates
all units in that package. Dependency fingerprints make every root manifest the
root of an immutable package artifact DAG. Decoding recomputes entity,
interface, and configuration fingerprints, rejects duplicate or malformed
records, verifies local observations, and can verify dependency observations
against an explicit set of immutable direct dependency manifests. Older Next
formats are deliberately not migrated. File publication uses a unique temporary
path, stages and verifies the complete encoding, and then atomically replaces
the destination. It does not read or translate incompatible cache data.

The driver stores immutable manifests and objects plus one mutable root
reference per build identity:

```text
.chtholly/cache/next-v26/
  .lock
  refs/<session-key>.ref
  leases/<lease-id>.lease
  manifests/<prefix>/<manifest-fingerprint>.manifest
  objects/<prefix>/<object-fingerprint>.<extension>
  specializations/<prefix>/<component-fingerprint>.specific
  specialization-index/<prefix>/<request-fingerprint>.ref
  type-specifics/<prefix>/<result-fingerprint>.type
  type-specific-index/<prefix>/<request-fingerprint>.ref
  nominal-semantic-witnesses/<prefix>/<result-fingerprint>.witness
  nominal-semantic-witness-index/<prefix>/<request-fingerprint>.ref
  type-layouts/<prefix>/<result-fingerprint>.layout
  type-layout-index/<prefix>/<request-fingerprint>.ref
  gc/state
  trash/<sweep-id>/
```

`--cache-dir` replaces the `.chtholly/cache` root. Session keys identify the
normalized project or workspace, selected package, and target. Object fingerprints
include the target triple, so the object directory can be shared across
sessions. Missing or corrupt objects invalidate and rebuild only their owning
units and are republished. An unreadable root reference or any invalid manifest
in its recursive closure fails closed.

Before compilation, the store briefly takes its cross-process lock, loads the
previous closure, publishes an immutable `CHNXTLEASE1` record, and holds a
shared operating-system lock on that lease file. The global store lock is then
released for the complete package compilation. Process termination releases the
lease lock without relying on a heartbeat or wall-clock timeout.

For an unlocked workspace build, resolution first suppresses implicit lockfile
writes. The driver atomically updates the lockfile from that provisional graph,
discards the provisional state, captures the resulting controls, resolves
again, and requires a locked verification to converge. At most one lockfile
update is attempted by a request.

The complete request snapshot is checked twice. The driver re-enumerates every
package source root, rereads every captured source and control input, and
recomputes the derived configuration immediately after capture and again before
artifact publication. An inventory, content, or configuration change is a
snapshot conflict: the build fails with an explicit retry diagnostic, releases
its lease, and publishes no object, manifest, or root reference. The combined
request fingerprint is an in-memory scheduling barrier and is not added to
`CHNXTPK42`, so link-only controls and unrelated package inputs do not invalidate
reusable package artifacts.

Publication validates and encodes candidate artifacts before reacquiring the
store lock. It compares the current session root with the exact root observed by
the lease, publishes CAS objects and manifests, atomically replaces the root,
and retires the lease. Any intervening root change is an optimistic-publication
conflict: the stale build writes no candidate artifacts and must be retried by
the caller. A failed build therefore cannot replace the previous root.

Mark-and-sweep garbage collection starts from every session root and every
active lease. It distinguishes active leases from crash leftovers by attempting
a non-blocking exclusive file lock, removes unlocked stale lease files, walks
all retained manifest dependency fingerprints and specialization component
DAGs, quarantines unreachable CAS
files, and then removes the quarantine. Any corrupt root, active lease, or
retained closure aborts collection without deleting artifacts. Automatic
collection is limited to once per 24 hours; session roots are not expired by
age.

Direct imports observe module presence. Imported calls observe the exact
`lookup module + public name -> canonical entity fingerprint` binding that
resolved successfully. An `export import` additionally observes the complete
provider export set because any added, removed, or changed binding changes the
forwarding interface. Repeated reads are canonicalized and all records are
sorted before verification.

Imported nominal reads observe the source-visible provider and binding together
with the canonical package/module/name and definition fingerprint. Foreign
resources add a relocation-closure observation derived from the canonical
nominal encoding, including hidden handles, invalid sentinels, protocol roles,
and canonical hidden operation targets. A provider closure change therefore
rebuilds an unchanged facade and exact consumers, while an unrelated hidden
callable does not. Diamond facades coalesce only when canonical identity agrees.

A build creates one isolated session per source package. A public
`NextPackageQueryGraph` owns stable `PackageQueryId` nodes, dependency counts,
reverse edges, and explicit `Pending`, `Ready`, `Running`, `Succeeded`, `Failed`,
and `Blocked` transitions. Its ready set is ordered by package name and releases
dependents only after all imports succeed, following worklist model. The driver owns a
fixed worker pool (`--jobs`, default one) and the execution policy; each worker
still gives a package its own `CompilationSession` and LLVM context. A failure
stops new work and blocks unscheduled queries. Compilation work and artifact
publication remain separate phases.

Artifact reuse has a second driver-owned bounded executor shared by every
package worker. With `--jobs=1` it executes inline; otherwise it uses at most
`min(4, jobs)` workers and a queue bounded to four tasks per worker. Object and
nominal-witness batches perform immutable store I/O and decode concurrently,
then `CompilationSession` consumes the result vector in dependency-stable unit
or `(package, request fingerprint)` order. Dynamic concrete-specialization
requests share one in-flight result per fingerprint, while component-closure
DFS remains inside one load task to avoid nested-pool starvation.

Load completion never mutates registries. Missing or corrupt objects become
local rebuild decisions only when their stable unit is reached; fatal errors
are likewise selected at stable consumption time rather than completion time.
Cancellation rejects queued work and lets already-started file reads drain,
then discards their results.
The executor is drained and destroyed before artifact publication, garbage
collection, or lease retirement. 

Artifact loading has an optional Driver-owned observation sink. The executor
collects bounded-pool contention and stable-consumer waits, while each
specialization store DFS builds a request-local topology and timing record and
merges it once. Disabled observation is a no-clock fast path. Enabled reports
use `chtholly-compiler-artifact-load-metrics-v1` and are written after executor drain
and destruction, before publication or lease retirement; they do not alter
semantic or artifact identities.

The synchronous specialization DFS remains the production algorithm. A
non-blocking dependency graph is eligible for implementation only when at least
two representative warm incremental workloads show both: closure DFS consumes
at least 30 percent of artifact-load wall time, and weighted component work over
critical path exposes at least 1.5x parallelism. Scaling must also stall while
artifact workers are underused. Otherwise optimization follows the measured
read, decode, queue, or package-scheduling bottleneck.

Dependency `build.entry` files are excluded; their library module roots are
compiled. The root package includes its requested entry, and only that entry may
define `main`. Incompatible precompiled artifacts are not accepted.

Within each package, local modules win import resolution. Otherwise only direct
dependency manifests are searched; duplicate providers are ambiguous and
transitive dependencies are not visible unless re-exported. A session parses
current sources to establish its local import graph, then processes units in
dependency order. For each unit it compares the source and
compilation configuration and resolves the previous observations against the
current local or direct-dependency provider interfaces. A reusable unit loads its public interface into
the new session registry and loads its native object from the content-addressed
store without constructing SemIR, LowIR, or LLVM IR. An invalidated unit runs
the complete check and lowering pipeline and publishes a replacement artifact.
Requests for transient LLVM or IR dumps explicitly invalidate only the root
unit because those compiler-owned structures are not persistent artifacts. The
resulting `IncrementalCompilationPlan` records deterministic rebuild, reuse,
and removal decisions with their reasons. `--explain-invalidation` exports
those decisions through the human and JSONL driver channels.

An unrelated new provider export does not rebuild an ordinary consumer that
observed a different binding. It rebuilds a module that forwards the complete
provider export set. A canonical entity rebind rebuilds only consumers that
observed that public name. Configuration changes conservatively rebuild every
unit because cached native objects are target-specific.

## Generic Artifacts And Specifics

`GenericTemplateArtifact` is the portable public definition of a generic
function. It contains canonical public types, integers, declaration and
definition evaluation regions, value blocks, and calls expressed as
`(package, module, entity, expected fingerprint)`. It contains no `CheckIRId`,
`FunctionRefId`, `GenericId`, `SpecificId`, source node, or arena address.
Artifact closure registration first predeclares every canonical public entity,
then registers interfaces. This two-phase load permits templates to refer to
entities in later or transitive artifacts without making those packages source
imports.

Chtholly uses a smaller portable
instruction vocabulary, including named aggregate construction and field
projection, and persists that vocabulary directly. Declaration
evaluation creates a typed placeholder. Concrete definition evaluation is
queued in stable discovery order and begins only after all source function
bodies are checked. A call through a dependent declaration-specific reference
is canonicalized back to the generic's definition template before a concrete
specific is queued; this prevents an empty declaration body from becoming an
instantiation source.

Every definition request transitions through `Queued`, `Evaluating`, and
`Ready` or `Failed`. The placeholder is installed before `Evaluating`, so a
recursive request finds the same specific and emits a recursive reference
instead of growing the worklist. The queue has a hard limit of 4096 requests.

Each concrete specific records calls discovered while cloning its definition.
After the queue drains, Tarjan decomposition groups canonical public specifics
into deterministic strongly connected components. Nodes are sorted by request
fingerprint; calls inside the SCC use node indexes, calls to completed SCCs use
request plus component fingerprints, and ordinary calls use canonical public
entity references. Components are built callee-first, so their fingerprints
commit to the complete transitive specific closure.

The persistent cache uses two identities. A request fingerprint commits to the
canonical template entity fingerprint, structural concrete argument
fingerprint, and target-independent semantic options. Its mutable, repairable
index points to an immutable `CHNXSCC28` component CAS entry. The component
fingerprint additionally commits to downstream component fingerprints. Only
specifics whose template has a canonical `PublicEntityId` and whose concrete
ABI types are portable are cached. Private and definition-session-local
specifics retain the session fingerprint path.

On a hit, the loader verifies the complete component DAG and canonical entity
fingerprints, installs placeholders for every node, then materializes bodies.
This supports direct and mutual recursion without source bodies and lets
different consumers reuse the same checked SemIR specific while preserving
their isolated sessions and LLVM contexts. Corrupt derived components or
indexes are cache misses and are repaired by publication; real filesystem
errors abort. The module specific fingerprint is stored beside its object
fingerprint, and the object CAS key commits to it.

## Place-State And Cleanup-Path Query

Next keeps ownership state in SemIR instead of introducing a parallel HIR/MIR
semantic track. A canonical `SemPlace` is a function-local identity followed by
ordered field or static-element projections. `PlaceStateQuery` evaluates each
non-template function after all local and artifact-loaded specifics have been
materialized. It stores immutable read/borrow/move observations and cleanup
plans keyed by return instruction or lexical block.

Cleanup registration is heterogeneous. Owned locals and `SemDefer` bodies are
recorded in one lexical sequence, and immutable edge plans contain ordered
`Destroy`, `DestroyIfInitialized`, `RunDefer`, and `EndLifetime` actions.
Planning walks the sequence backward and analyzes a deferred body at its
execution position, so a move inside defer changes the cleanup obligations
that follow it. Return, break, continue, and block-fallthrough plans therefore
share one place-state model instead of reconstructing scopes during lowering.

The analysis represents every tracked leaf as an initialized/moved possibility
set. A destructive `move` clears the selected subtree. `if` joins union the
possibilities of the taken and fallthrough paths; a terminating taken path does
not contribute to the continuation. Whole-place use rejects moved, maybe-moved,
and partially moved states, while a disjoint initialized projection remains
usable. Locals introduced by a nested block are cleaned at that block boundary
and removed before the outer state join.

LowIR consumes the query through place-addressed `MoveOut` and `Destroy`,
value-addressed `DestroyValue`, and root `EndLifetime`. It never derives cleanup
by scanning every function slot. Conditional destruction uses explicit
`IsInitialized` and `BranchIf` operations over logical path flags, and LLVM
only executes that control-flow decision. Public generic templates and
`CHNXSCC28` components serialize lifecycle, conversion, carrier-view,
union construction/access, enum construction/payload access, and canonical
object-role opcodes but not session-local
place IDs; canonical places and cleanup paths are reconstructed after artifact
materialization.

Default nominal destruction expands the verified field body in reverse order.
Each owning field re-enters the ordinary value-cleanup dispatcher, so callback
registration, completion, and wake fields reach their explicit
`FinishCallback*` plans instead of becoming inert aggregate destruction.

Chtholly makes cleanup planning a query now so later coalescing and custom-destroy witness calls do not
require a second semantic ownership model.

Basing separate loop target depths,  Chtholly extends
that model with heterogeneous registrations and interned LowIR cleanup suffixes
keyed by action, place, defer block, local, and successor. Chtholly
shares identical tails now. Ordinary synchronous functions additionally write
non-in-place return values to one hidden result slot and converge on one return
terminal. Coroutine scaffolds and task drivers retain their frame-specific
return lowering until suspension edges join this cleanup model.

## Interprocedural Ownership And Effect ABI

Callable ownership is now a first-class SemIR and public-artifact fact. A
compact `SemFunction` keeps only IDs and flags; an aligned side table owns its
canonical effect and return-provenance summary. Generic templates and concrete
specific components carry the same fact, and imported calls read it from the
canonical `PublicEntity` rather than a forwarding binding.

Inference builds function CFG provenance, decomposes the local call graph into
Tarjan SCCs, and solves components callee-first. Place-state consumes converged
`Read/Write/Take/BorrowShared/BorrowMutable` regions as may-access facts, then
applies a separate Preserve/Initialize/Invalidate normal-return lattice to
caller state. Explicit CFG edges carry canonical bounded DNF path conditions.
A live-loan pass substitutes callee conditions through caller Boolean
expressions and removes arms whose condition is known false. Cached specific
summaries are reset, recomputed from their bodies, and compared with the
persisted expectation before publication.

The same rule applies across generic substitution: reference provenance bits
are excluded from generic type identity during inference, while reference
mutability and pointee structure remain checked. This keeps session-local
provenance out of specialization identity and permits a concrete generic
wrapper to forward an initialization capability without weakening the later
ownership analysis.

Aggregate place state follows the same canonical-store discipline. Fixed-array
and tuple element projections are independent leaves, while dynamic array and
slice indexing use a bounded `AnyElement` wildcard for conservative overlap.
Slices remain borrowed views: verifier layers reject element `Take` and
`Initialize` authority, so no container-specific ABI lane or artifact schema
extension is needed.

Foreign calls persist a target-neutral `ForeignAbiSignature` and become an
explicit LowIR `ForeignCall`. One verified target-aware LowIR query classifies
scalar and `repr(C)` aggregate parameters/results and drives declarations,
calls, marshalling, `byval`, and `sret`; LLVM only materializes its lanes. 

C function-pointer type identity also owns an alpha-erased callable ownership
contract. Explicit adapters lower to `ForeignAbiThunkPlanId`, separating the
ordinary semantic source from C parameter lanes, object copies, and return
slots. LowIR recomputes and verifies behavioral compatibility and every
conversion strategy; LLVM uses the plan without target or field
reclassification.

Capturing callback types add a second immutable plan boundary. SemIR owns the
entry/context/release contract and move-only release obligation; LowIR owns one
epoch-9 `CallbackAdapterPlan` with physical context order and both call layouts.
The verifier recomputes that plan, and LLVM only executes the call and cleanup.
These callback plans are compiler-internal Interop projections.

Foreign registration adds a third immutable plan boundary. SemIR owns the
structural callback/handle/register/unregister/cancel type, exact physical
entry/userdata/release positions, labeled bound-parameter names and positions,
and retained or transferred release
authority. LowIR recomputes one epoch-10 `CallbackRegistrationPlan` from those
facts and the three target-aware C call layouts. Source operands are
canonicalized to physical bound-parameter order, while callable ownership
remaps register effects back to labeled source operands. Qualified imported C
function values reuse canonical ImportIR entities across re-exports.
Cleanup-path lowering selects
unregister, cancel, or ordinary finish before LLVM. LLVM only extracts the
versioned registration aggregate, emits the planned calls and handle guards,
and follows the precomputed release decision. Source bindings are normalized
by CFDL before reaching this backend boundary.

Completion-token cancellation adds a fourth plan boundary without changing
the third. A seven-field registration type retains the epoch-10 synchronous
unregister/cancel plan and optionally links its cancel-request layout to one
epoch-12 `CallbackCompletionPlan`. The completion plan owns the wait layout,
adapter plan, and release authority. SemIR `DiscardValue` and place cleanup
make every owned temporary and local completion explicit. LLVM emits only the
verified null-handle, null-token, wait, and release branches. These plans are
not a Chtholly source feature.

Nonblocking readiness adds a fifth plan boundary without moving terminal
authority. A poll-capable eight-field registration produces a five-field
completion. LowIR recomputes one epoch-13 `CallbackReadinessPlan` containing
only the poll call layout and a reference to the epoch-12 completion plan.
LLVM's null-token fast path returns true; only the non-null path calls C. The
probe never emits wait, release, or place-state transitions. A runtime bridge
therefore exposes `probe(completion&) -> bool` and consuming
`finish(completion) -> void`, with cleanup equal to finish. Readiness facts are
represented by CFDL event relations.

Explicit wake registration adds a sixth plan boundary while preserving the
previous five. A wake-capable ten-field registration produces a seven-field
completion and a two-field runtime wake value `{completion, initial_ready}`.
LowIR recomputes one epoch-14 `CallbackWakePlan` containing arm, detach, and
wake-release call layouts plus the physical role arrays, and references the
epoch-12 completion and epoch-13 readiness plans. LLVM emits the atomic arm
decision, local null/ready wake release, foreign ownership transfer, blocking
finish, and nonblocking detach exactly as planned. Wake plans are generated
from published Interop artifacts.

The safe task-runtime boundary is deliberately above those six plans. Private
fields in move-only `Subscription`, `PendingCancellation`, `Waker`, and
`Cancellation` nominal types retain the raw values. Ordinary public functions
perform the state transitions; consumers contain no raw callback operations.
Implicit drop keeps epoch-12 blocking finish, while only explicit `abandon*`
selects epoch-14 detach. The removed task/cancellation library is outside the
current standard-library boundary.

The library task/scheduler boundary is above that safe bridge. A move-only
generic `Task<T>`, private query-based
suspension result, and structured cancellation scope retain stable runtime
identities while the epoch-14 wake value owns terminal release authority.
Runtime helpers implement one `Idle/Queued/Running/Terminal` transition with a
coalesced rerun debt; scheduler and application SemIR contain ordinary calls
and no callback instructions. See
`docs/design/next-library-task-scheduler.md`.

The independent task runtime ABI adds a compiler-only plan without changing
the source surface. Typed SemIR coroutine operations identify explicit
checkpoints in an internal scaffold function. LowIR owns the version-11
`CoroutineFramePlan` typed store, canonicalizes suspension markers by
LowIR instruction order, builds typed predecessor/successor nodes, and computes
per-edge SSA/place liveness with a deterministic fixed-point worklist. It
verifies selective frame lifting, field-sensitive persisted bitmap bits, per-state
reverse cleanup and wake ownership, resume block/continuation identity,
path-specific no-borrow-across-suspension, projected partial aggregate state,
and the no-ordinary-call boundary. LLVM consumes that plan to split real
ready/pending/resume CFG regions, reconstruct place initialization after every
resume, and emit type-specific cleanup, runtime wake adapters, descriptor
thunks, and an epoch-1 hidden status/out-task constructor keyed by the
canonical public async entity fingerprint. A compiler-only runner
provides native multi-suspension branch completion and cancellation evidence
without exposing a source constructor. Compiler-only typed task drivers use
canonical `CoroutineTask<T, E?>`, `CoroutineTaskOutcome<T, E?>`, and
`CoroutineChecked<T>` identities for root/child creation, join, query, and
refined payload movement. LowIR binds every creation to one immutable target
plan containing the canonical constructor entity, ABI epoch, and optional local
scaffold. LLVM emits a hidden producer definition or imported declaration from
that entity and releases task handles on structured exits. Typed completion
operations arm, probe, suspend with, detach, and release a nonblocking runtime
completion whose callback authority transfers exactly once. 

Completion composition remains above runtime ABI v1. Typed SemIR set and
selection identities carry static capacity; compiler-private storage carries
the ordered completion array and extensible active/armed bitmaps. Immutable LowIR
set/combine plans freeze canonical order, lowest-index winner policy,
wait-all/select/race loser ownership, suspension identity, continuation, and
ABI epoch. Phase-end verification reconstructs the plans from SemIR and frame
regions. LLVM incrementally consumes ready wait-all entries, transfers select
remainders, releases race losers, and handles empty operations synchronously.
Selection transfers both bitmaps so a remaining foreign completion cannot be
armed twice. No parser, public task type, or runtime entry is added.

The source-independent async contract is frozen in SemIR, LowIR frame plan v11,
public artifacts, and runtime ABI v1. Async function identity contains ordered
parameters, success type, and optional error type; cancellation remains a
separate payload-free outcome. Evaluation is left-to-right exactly once,
creation is eager with the current scope and executor, switching persists, and
entry, suspension commit, post-resume, explicit check, and executor switch are
the only fixed cancellation points. See `docs/design/next-async-semantics.md`.
Source async entry tests now continue past LLVM inspection: they emit, link,
and execute success, handled failure, and cancellation through the generated
root runner. The cancellation fixture uses an isolated test task runtime, so
the execution evidence does not create a public current-task capability.
Implicit structured cancellation reuses the typed root/child create mode
already reconstructed at the LowIR phase boundary. Runtime attachment records
a lightweight ancestry node and linearizes creation against propagation under
the scope lock. 
Lexical task-scope composition uses distinct normal, selected-error, and
selected-cancellation drain instructions. Frame verification reconstructs the
accepted cancellation causes, child escalation policy, and inner-to-outer
acknowledgement boundary instead of trusting adjacent instruction order or
lowering booleans. Runtime cancellation remains one sticky payload-free bit;
see `docs/design/next-structured-cancellation-composition.md`.
Payload enums are implemented as a complete vertical slice. Generic template
and concrete-specialization evaluation blocks preserve the identity of the
instructions also owned by executable CFG blocks; enum payload materialization
must reuse those instructions rather than clone a detached value region.

## Required Invariants

- Process-local IDs are never serialized or used as cache keys.
- `CheckIRId`, `PublicInterfaceId`, and `PublicEntityId` are session-local;
  only their canonical fingerprints may cross session boundaries.
- Parse nodes contain syntax only; semantic decisions exist only in SemIR.
- SemIR and LowIR records contain interned IDs, not owning strings or AST
  pointers.
- LLVM lowering consumes explicit LowIR decisions and does not reinterpret
  ownership, provenance, mutation, failure, or cleanup rules.
- Public semantic fingerprints are length-delimited, canonical, and independent
  of insertion order and source-local handle values.
- Incremental states form a closed graph: every observation resolves to a
  matching provider artifact in the same state.
- Reused units expose their parse tree, public interface, and native object;
  they never synthesize fake SemIR, LowIR, or LLVM state.
- Incompatible artifacts and caches are not fallback inputs to the pipeline.
- A producer-local ID is interpreted only through its explicit producer unit;
  no consumer may index a local store with a foreign raw ID.

## Nominal Representation Artifacts

A public nominal definition has a canonical `PublicEntityId` during a session
and a portable `(package, module, name, definition fingerprint)` identity in
artifacts. `CHNXTYPE24` definitions encode per-field visibility, nominal
export visibility, foreign handle representation and sentinel facts, and
foreign resource protocols plus hidden target descriptors. A public resource
may retain a private handle definition in its artifact closure without adding
that handle to public name lookup. Hidden operation bindings retain a canonical
raw C entity name and a fingerprinted unspellable interface name. Consumers
materialize those entities only when the public resource is referenced. The nominal
struct/union/enum kind, generic arity, ordered named fields and enum payloads,
normalized storage
paths, explicit opaque/C policy, and optional generic custom value/object
carrier patterns.
They do not contain
layout because one semantic definition can have different concrete type
arguments and target layouts.

`CHNXSPE26` evaluates a definition for structural concrete arguments. Its
request fingerprint commits to the canonical definition, arguments, and
semantic options. Its immutable result additionally commits to substituted
fields, the ordered child nominal-specific closure, and exactly one semantic
witness fingerprint. Representation and lifecycle facts are not duplicated in
the specific. Recursive by-value definitions are rejected before artifact
construction.

`CHNXLAY18` is a separate target query. Its request commits to the type-specific
result and a `TargetLayoutConfig` fingerprint containing normalized target
triple, pointer width, and nominal-layout ABI epoch (currently 6). The result
stores the
nominal kind, carrier size and alignment, plus a tagged logical-field layout:
stable struct fields have offsets, union fields overlap at zero, opaque fields
have no fabricated byte location, bit fields identify a carrier word and
half-open bit range, and enums store a fixed `u32` tag, payload offset, and
per-variant field offsets. LowIR freezes those enum facts and LLVM validates
them against target `DataLayout` rather than recomputing layout. Request and
result fingerprints are recomputed during
decoding. The package manifest retains both request and result identities; the
store publishes repairable request indexes to immutable CAS results, and
lease/GC closure tracing covers both layers.

`CHNXWIT23` is the canonical semantic completion result for a nominal specific.
Its request commits to the nominal entity, arguments, semantic options, and
structural specific fingerprint. Its immutable result additionally commits to
object/value/init/ownership/copy/move/destroy facts, optional canonical public
copy/destroy and pack/init targets, optional custom value/object carriers,
stable/computed/bit-packed projector records, shell targets, the nominal kind,
compiler-derived transferable/shareable facts, and a sorted unique transitive
specific closure. The artifact also stores
canonical field-indexed copy/drop bodies and projector capabilities. Each
operation binds a field to a primitive witness identity or child
nominal-specific witness; the result fingerprint therefore covers the exact
generated work. `CHNXTPK61` version 61 retains each
witness through its own content-addressed result and request index; active
leases and GC trace both.

Default nominal witnesses select `NominalAggregate / Pointer / InPlace`.
`repr(value = T)` definitions retain their declared fields and require exact
canonical `pack(const Self&) -> T` and `init(Self&, T) -> void` targets. After
generic substitution, a witness accepts any concrete non-void, non-function
carrier and selects `NominalAggregate / Custom(T) / ByConversion`. Carrier
nominal specifics and canonical conversion targets enter the transitive
closure. A separate dependency stack rejects representation-carrier cycles
without rejecting legal recursive field witness SCCs.

`repr(object = T)` selects a nominal carrier independently from the value
carrier. A field selects a stable path, an opaque projector, or a bit range.
Stable paths retain exact endpoint types. Opaque projectors persist explicit
load/store/take/init targets plus optional stable-reference borrow targets.
Bit ranges persist a carrier path and non-overlapping interval and never expose
a proxy reference. Unclaimed carrier storage is hidden state owned by the
object shell.

The `init` body is checked as destination initialization, not assignment to an
already live object. Reads from destination fields before their first write are
invalid. Every field must be definitely initialized on all returning paths;
branch results are intersected and a loop does not prove a write after it.

Generic lifecycle impls retain separate copy/drop generic regions and owner
patterns. Artifact construction structurally matches each pattern under the
concrete nominal arguments, including repeated binding equality. Applicable
templates contribute their canonical entity fingerprint to the witness
closure. A deterministic, bounded witness work queue installs stable SCC
backedge seeds before recursively completing field witnesses.

Before LowIR construction, verified witnesses are bound to their concrete
session-local `TypeId`. `SemIR::typeRepresentation` reads that immutable
binding. LowIR snapshots terminal object types, concrete value types, physical
field paths, and complete place projections: slots and places use object
representation, while instruction values use value representation.
Concrete source declarations can override copy/move/drop policy through the
Chtholly `lifecycle(...)` prefix and lifecycle-only impl forms. Semantic
resolution converts custom copy/drop declarations to hidden canonical public
entities. Imported nominal specifics load their immutable witness through the
active artifact lease; re-export consumers retain the visible provider edge
while the witness commits to the canonical target and transitive closure.
LowIR records `ObjectAddress`, `DereferenceObject`, `PackValue`, `UnpackValue`,
`InitializeFromValue`, `InitializePlaceFromValue`, and `ParameterValue` in
addition to `Initialize`, `Transfer`, `MoveOut`, `CopyValue`, `LifecycleCopy`,
`InitializePlace`, `TransferReturn`, place-addressed `Destroy`, value-addressed
`DestroyValue`, `LifecycleDestroy`, `LifecycleDestroyValue`, logical
`IsInitialized`/`MarkInitialized`/`MarkMoved`, and `EndLifetime` actions derived from those
facts. Custom objects add `MakeObject`, `MakeObjectCopy`, `MakeObjectMove`, and
field-indexed `ProjectionLoad/Store/Take/Init/Borrow/BorrowMut`. Four shell
targets initialize, copy-initialize, move-initialize, and drop hidden state.
LLVM consumes the verified LowIR table and performs no lifecycle or projector
lookup.

## Function ABI

Next semantic checking registers all function signatures before checking any
body. Direct calls therefore resolve forward declarations and recursive
references without source-order coupling. SemIR records a typed function
reference and lexical argument block; LowIR separately records
parameter-to-slot binding and preserves direct calls; LLVM lowering creates all
prototypes before definitions.

The internal calling convention passes `i32`, `bool`, references, and custom
carriers directly. A reference is one pointer carrying read-only/mutable and erased/parameter
provenance in SemIR and public artifacts. Default nominal parameters are
pointers to their object representation and default nominal results use a
leading result-slot pointer. Custom-carrier nominal parameters and results use
the carrier type directly; parameter binding and local initialization invoke
the snapshotted init target. Canonical pack returns its carrier directly even
when that carrier normally uses an in-place result ABI. `TransferReturn` remains
exclusive to `InPlace` results, so later custom destruction cannot invalidate a
return value. Aggregate construction initializes field storage, field
projection uses the verified nominal field index, and indirect copies use the
target data layout. Public generic specifics use the same ABI whether built
locally or materialized from a persistent component.

The source `main` remains parameterless and returns `i32` so the host and
embedding entry wrappers keep a stable ABI. Strings and arrays
are supported as representation-conversion carriers but remain outside ordinary
source function signatures; function values, methods, overloads, raw pointers,
and indirect calls are also outside this contract. Nominal values have explicit
per-specific lifecycle facts. Trivial destroy lowers to no operation plus
lifetime end. Destructive move, explicit and custom copy, assignment
reinitialization, custom destroy, path-sensitive cleanup, and loop fixed-point
analysis are implemented. LLVM never infers those choices from layout.

## Lifecycle Query And Assignment

Assignment is a statement, not a value expression. The place-state query first
evaluates the right-hand side, obtains a canonical mutable target place, emits
cleanup for any initialized old leaves, and records a reinitialization plan
that restores the target subtree to initialized. `while` evaluates condition
and body regions repeatedly in the abstract `{initialized, moved}` possibility
lattice until the header state stabilizes. LowIR consumes the final plans as
explicit cleanup plus `InitializePlace`; LLVM updates runtime initialized flags
without reconstructing semantic state.

Projector methods are a canonical contract: signatures and capability presence
are verified, and explicit `core::carrier(self)` provenance is independently
replayed by the SemIR and LowIR verifiers. Observable carrier endpoints must
remain inside the declared region, while object-shell roles receive the whole
carrier. Computed fields are atomic in compile-time place state and own
independent runtime state cells. `take` followed by `DestroyValue` handles
owning fields without a physical address. Canonical carrier paths and
path-sensitive CFG loan dataflow provide the alias boundary.
