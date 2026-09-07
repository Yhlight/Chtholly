# Chtholly Foreign Definition Language

Status: Next-only redesign. CFDL is an independent binding language for C ABI
declarations and resource-flow facts. It is compiled to an immutable binding
artifact. Chtholly consumes that artifact through an ordinary module import;
Chtholly source does not contain CFDL syntax.

## Boundary

The compilation pipeline is:

```text
CFDL source
  -> lex
  -> parse
  -> elaborate/check
  -> normalize
  -> publish binding artifact
  -> ordinary Chtholly import
```

CFDL owns physical ABI declarations, resource-flow facts, loans, obligations,
events, and external linkage. Chtholly owns ordinary module lookup, ordinary
types and calls, and generic language semantics. ABI lowering and callback or
completion plans are compiler-internal Interop projections of the artifact.

## Raw CFFI mode

CFDL also supports a deliberately minimal, traditional CFFI form. A foreign
callable whose parameters and result have no flow qualifier, and which has no
`where`, `error`, or `outcome` clause, is a **Raw CFFI callable**:

```cfdl
module native_api;

foreign fn version() -> c_int link "library_version" call c;
foreign fn reset() -> void;
```

Raw CFFI validates the physical signature, target layout, external symbol, and
calling convention, then publishes the function as an ordinary imported
callable. It does not infer or publish ownership, borrowing, escape, cleanup,
error-code, buffer-initialization, or completion facts. Internally it has one
canonical `value.result` resource-flow fact so that artifact normalization and
fingerprinting remain total; this fact is not a resource obligation and is not
an implicit contract.

The absence of a contract is intentional. A raw foreign handle or pointer is
not automatically owned and is not automatically released. A binding author
who wants Chtholly to check a resource protocol must add the appropriate flow
qualifiers and `where` facts, and must add `error` or `outcome` clauses when a
C return value carries a recoverable failure or initialized-buffer meaning.
Raw mode therefore provides the ABI safety of conventional CFFI while leaving
the C library's undocumented resource assumptions outside the checked model.

## Source Surface

```text
cfdl-file       := module-decl import-decl* foreign-decl*
module-decl     := "module" module-path ";"
import-decl     := "import" module-path ";"
foreign-decl    := foreign-type-decl | foreign-struct-decl
                 | foreign-union-decl | foreign-enum-decl
                 | foreign-const-decl | foreign-fn-decl
foreign-type-decl := "foreign" "type" identifier
                     (":" physical-type invalid-clause?)? ";"
foreign-struct-decl := "foreign" "struct" identifier
                       "{" foreign-field+ "}" ";"
foreign-union-decl  := "foreign" "union" identifier
                       "{" foreign-field+ "}" ";"
foreign-enum-decl   := "foreign" "enum" identifier ":" physical-type
                       "{" enum-constant+ "}" ";"
foreign-field     := identifier ":" physical-type ";"
enum-constant     := identifier "=" signed-integer ";"
invalid-clause    := "invalid" ("null" | signed-integer)
foreign-const-decl := "foreign" "const" identifier ":" physical-type
                      "=" (signed-integer | "true" | "false") ";"
foreign-fn-decl := "foreign" "fn" identifier "("
                    parameter-list? ")" "->" flow-type
                    foreign-fn-clause* ";"
foreign-fn-clause := link-clause | call-clause | outcome-clause
                   | error-contract | where-clause
link-clause      := "link" string-literal
call-clause      := "call" ("c" | "win64" | "sysv64")
outcome-clause   := "outcome" "posix_read" "<" physical-type ">"
                    "(" identifier "," identifier ")"
                  | "outcome" "win32_read" "<" physical-type ">"
                    "(" identifier "," identifier "," identifier ","
                    identifier ")"
parameter       := identifier ":" flow-type
flow-type       := flow-qualifier? physical-type
flow-qualifier  := "owned" | "ref" | "ref_mut" | "view" | "view_mut"
                  | "move" | "out" | "inout"
where-clause    := "where" where-item ("," where-item)*
where-item      := where-fact
error-contract  := "error" ("code" | "errno" | "win32")
                   "when" "result" error-predicate
error-predicate := ("==" | "!=" | "<") error-value
                 | "==" ("null" | "invalid")
                 | ("in" | "not" "in") "{" error-set-item
                   ("," error-set-item)* "}"
error-set-item  := error-value | error-value "through" error-value
error-value     := signed-integer | identifier
```

The default C ABI is used. Target-specific layout, calling convention, symbol
identity, and linkage are artifact facts rather than Chtholly source syntax.
Physical types use fixed-width scalars, pointers, `array<T,N>`, foreign
identities, and C function-pointer types. The target-aware scalar set is
`c_bool`, `c_char`, `c_schar`, `c_uchar`, `c_short`, `c_ushort`, `c_int`,
`c_uint`, `c_long`, `c_ulong`, `c_long_long`, `c_ulong_long`, `c_size`,
`c_ptrdiff`, and `c_wchar`. Width, alignment, and signedness come from the
selected target contract, never from the compiler host.

## Foreign Type Carriers

`foreign type T;` establishes only the canonical package/module/name identity.
It is incomplete and has no default `void*` representation. It may occur behind
an explicit pointer or in `ref`, `ref_mut`, `view`, and `view_mut` lanes, but it
cannot be stored, copied, moved, returned by value, or used by `out`/`inout`.

`foreign type T: P;` attaches an integer or non-const pointer carrier. This is
not an alias: two foreign identities with the same `P` remain distinct. An
optional `invalid null` or integer literal records the exact invalid
sentinel. A non-zero integer on a raw-pointer carrier is a target-width pointer
bit pattern, supporting values such as `INVALID_HANDLE_VALUE`; zero must be
spelled `null`. No sentinel is inferred when the clause is absent.

A foreign record body rebuilds an ordered C ABI carrier from ordinary fields,
nested completed records, and fixed arrays. Carrier fields participate in
layout and ABI classification but are not Chtholly members, constructors, or
projections. Recursive value shapes, empty records, bit-fields, flexible arrays,
and packed records are rejected.

A foreign enum has an explicit integer carrier. Its constants are imported as
module-level typed constants such as `api::STATUS_OK`; the type remains an open
C enum, not a closed Chtholly enum, so unknown integer values remain valid ABI
inputs. A foreign union uses overlapping ABI-only members and exposes no field,
constructor, or active-member operation to Chtholly.

`foreign const` publishes one typed target constant. Its type must be `c_bool`
or a target C integer type and its value must fit that type. It is a value fact,
not a preprocessor facility: CFDL has no macro expansion or conditional
compilation.

`link "symbol"` separates the external C symbol from the CFDL lookup name.
`call c` selects the target C convention and is normalized to Win64 on Windows
x64 and SysV64 on Linux x64. Explicit `win64` and `sysv64` are accepted only on
their matching targets. The same convention spelling follows `c_fn`, for
example `c_fn win64(c_int)->void`; it participates in function-pointer type
identity.

## Flow Qualifiers

`owned T` is an obligation-bearing value. It means that the consumer receives a
value together with one or more terminal obligations; it does not assert that
the value owns a particular allocation.

`move T` transfers the obligation carrier into the foreign call. `ref T` and
`ref_mut T` borrow an existing resource identity. `view T` and `view_mut T`
borrow a data projection or memory range and do not grant resource operations.
`out T` is initialized by the callee. `inout T` is initialized by the caller
and may be mutated by the callee. In a published artifact these qualifiers
also determine the semantic function type: `ref` is a read-only checked
reference, while `ref_mut`, `out`, and `inout` are mutable checked references;
the physical ABI classification remains a pointer/reference lane.

An `out T` lane publishes an exact callable `Initialize` effect and initialized
postcondition. Chtholly callers do not repeat the qualifier: they declare
typed storage without an initializer and pass the ordinary place name. The
compiler alone materializes the initialization projection after verifying the
artifact contract. For an `i32` status-plus-output signature, status zero is
the success outcome and every published return outcome must leave each output
lane initialized, allowing providers to use a valid inert value on failure.

An ordinary Chtholly function with a mutable reference parameter may forward
that parameter to an `Initialize` callable. Ownership summary inference then
publishes the same parameter-root `Initialize` effect and initialized
postcondition for the wrapper. The source surface remains `T&` plus an ordinary
place argument; no source-level `out` qualifier is introduced. The forwarded
capability is pending for the duration of the call chain, so reads, copies,
borrows, storage, and returns before the inner initialization are rejected.

The default loan lifetime is the call return. An input may escape or be stored
only when a `where` fact states the longer endpoint. Raw pointer adoption,
aliasing, and external cleanup must be stated by resource-flow facts or the
binding is rejected as incomplete.

## Where Facts

The `where` clause is a finite relation syntax, not a protocol-definition DSL.
It admits only these fact forms:

```text
place escapes endpoint
place stores holder
result derives source
place invokes endpoint
place obliges action
place discharges action
action requires predicate
```

`endpoint` is `call_return` or a resolved event name such as
`complete`. `predicate` is a compiler-defined boundary predicate such
as `valid`, `initialized`, `quiescent`, or `same_thread`. The clause cannot
contain control flow, arbitrary calls, nested state declarations, or user
defined fact bundles.

Examples:

```cfdl
foreign fn session_open(url: view string) -> owned Session
where result obliges close;

foreign fn request_start(
    file: ref File,
    buffer: ref_mut Buffer
) -> owned Request
where
    buffer escapes complete,
    result derives file,
    result invokes complete,
    result obliges resolve;

foreign fn request_wait(request: ref_mut Request) -> Result
where request discharges resolve;
```

The compiler normalizes these facts into a resource state containing identity,
validity, owner, loans, obligations, protocol constraints, and event edges.
Protocol automata are inferred from facts across ordinary foreign declarations;
there is no source-level `protocol`, `state`, or `operation` declaration.

An event name is resolved in the endpoint-typed object position and is owned
by the declaring foreign callable. Its published identity still includes the
canonical package, module, resource, callable, and event name, so shortening
the source spelling does not merge events across callables or packages.

Imported protocols use qualified object names. `module::action` references an
action identity from an imported CFDL module, while
`module::callable::event` references an event owned by a specific imported
foreign callable. Consumer artifacts retain only the referenced canonical
fingerprint; the session registry must load the provider artifact and resolve
the edge before semantic import succeeds.

Imported nominal types use the direct spelling exported by a unique `import`
source. For example:

```cfdl
import provider;
foreign fn observe(session: ref_mut Session) -> i32;
```

`Session` is materialized from the provider public interface only when its
canonical entity and fingerprint verify. A local declaration or another import
with the same direct name makes the spelling ambiguous and is rejected; import
order never selects a winner. Qualified nominal spelling is reserved for a
future syntax extension.

The normalized representation is typed before artifact publication. Places,
endpoints, actions, and predicates are canonical IDs; relation operands are not
serialized as free-form strings. A package must close each obligation action
with exactly one discharge declaration before its CFDL interfaces are
published. Quiescent cleanup requirements are checked at the same boundary.

## Semantic Artifact

CFDL publication produces an immutable binding artifact containing:

- ordinary public type and function identities;
- target-neutral ABI signatures and external symbols;
- normalized flow, loan, derive, obligation, event, and quiescence facts;
- typed relation payloads, including subject/target places, endpoints,
  actions, and predicates;
- generated callback, completion, and ABI lowering plan references;
- canonical fingerprints and compatibility epochs.

## Interop Artifact Boundary

The operation family is represented in the compiler as
`chtholly::next::interop` artifacts. `PublicInterface` no longer owns a
`ForeignOperation` nested type; semantic and lowering code consume the
namespaced `ForeignOperationArtifact` and its callback/completion descriptors.
The artifact has an explicit canonicalization and verification step. Its
session-local `InteropArtifactId` is never serialized. Cross-artifact loading
uses the separately defined `interop::ArtifactReference`, whose schema epoch,
canonical callable identity, and stable fingerprint must all agree before the
artifact is accepted.

The interop registry is session-owned and phase-owned. It stores the verified
`ForeignOperationArtifact` payload in a `ValueStore` and publishes only an
`ArtifactReference` containing schema epoch, canonical package/module/name,
and stable fingerprint. References have no session-local payload pointer or
implicit dereference operation. Every consumer must call
`ArtifactRegistry::resolve(reference)` explicitly before inspecting the
payload; a missing registry entry is a hard import error.

Interop payloads are persisted in the versioned `CHNXIOP11` bundle format. The
same bytes may be written as an independent `.interop` sidecar or declared by
the optional package manifest record `interop-bundle`. Its SHA-256 and path
participate in package identity and archive closure verification. Bundle
records are canonicalized and verified before being registered atomically;
schema, identity, fingerprint, digest, ordering, and input-size mismatches
fail closed.

The current CFDL semantic epoch is 15. Epoch 15 adds explicit POSIX `fread`
accessor outcomes while retaining the physical/public lane split. Epoch 14
added explicit physical/public argument sources and the checked synchronous
Win32 read projection. Epoch 13
and older sidecars
are rejected before source import.

The standalone `chtholly-cffi` tool may mechanically generate epoch-14 carrier,
constant, and callable declarations, but generated output is only a draft. It
does not infer ownership or error `where` facts. `verify` requires a native
Tier-1 C compiler probe and emits a separate canonical `CHCFFI3`
receipt. Its target and Clang, compiler family, toolchain, SDK, configuration,
header, CFDL, probe, and fact digests form one identity shared by Next package
check/build artifacts, lockfiles, and Package Artifact v20. Next manifests may
use the receipt as a pre-compilation package gate; the compiler still does not
link libclang. Invalid, stale, non-canonical, or target-mismatched receipts
fail before semantic compilation.

`chtholly-cffi generate` additionally emits standalone `CHCFFIS5` maintenance
state. `regenerate` uses that mechanical baseline to update a human CFDL file
without treating flow qualifiers, `where` facts, invalid sentinels, imports, or
manual declarations as Clang-owned facts. The state is not CFDL syntax, an
Interop artifact, a receipt input, or a compiler ABI. Missing state is adopted
only when the supplied binding already matches the configured C declarations.

CFFI config v3 resolves one effective Tier-1 C compiler/SDK contract before
generation. `CHCFFI3` and `CHCFFIS5` persist its compiler-family, toolchain, and
SDK identities. Older config, receipt, and state formats are not accepted.

An error contract retains the raw C return layout and adds one typed Interop
predicate, extractor, and success payload. `error code` returns
`Result<void, Code>` and moves the failed integer result into `Err`; open foreign
enums use their physical integer carrier. `error errno` returns
`Result<Raw, i32>` and supports integer predicates or `result == null` for raw
pointers and pointer-backed foreign handles. `error win32` is Windows-only,
returns `Result<Raw, u32>`, and calls `GetLastError` only after an integer
failure predicate. Every accessor is read in the failure block immediately
after the foreign call. `error code` additionally admits `out`/`inout` lanes
for target C integer status results (`c_int`/`c_long`); those lanes publish
verified initialization effects while the status projects to `Result<void,
Code>`. Other error contracts continue to reject `out` and `inout` lanes.
These projections change neither the C ABI nor the Component ABI.

`outcome posix_read<T>(buffer, capacity)` is Linux-only and requires an exact
`error errno when result == -1` clause. The buffer must be a mutable raw-pointer
view, capacity an unsigned integer, and the signed result must have the same
width. The element is currently restricted to an 8-bit integer. The public
result is `Result<ReadOutcome<slice<T>>, i32>`: positive counts produce `Data`
over the initialized prefix, zero produces `Eof` when capacity is nonzero, and
zero capacity plus zero produces `Data` with an empty slice. A count beyond
capacity, an unexpected negative count, a positive count with a null buffer,
or capacity beyond the signed result domain traps as a violated foreign
contract.

The same projection may be used by a POSIX callable with additional ordinary
input lanes, such as `recv(sockfd, buffer, capacity, flags)`. Those lanes remain
part of the public and physical signature; only the named buffer and capacity
lanes are interpreted as the initialized prefix. The projection does not infer
socket flags or other ordinary inputs.

`outcome win32_read<T>(buffer, capacity, count, context)` is Windows-only and
requires the exact `error win32 when result == 0` contract. Its result and
capacity are 32-bit integers, count is an unsigned 32-bit `out` lane, context
is a pointer lane, buffer is a mutable raw-pointer view, and `T` is currently an
8-bit integer. The public call omits count and context; the compiler allocates
count storage and passes null context. This contract is synchronous only.
Success classifies count with the same Data/Eof/empty rules as POSIX and checks
the initialized prefix. Failure reads `GetLastError` and never reads count.

`outcome fread<T>(buffer, element_size, element_count, stream)` is Linux-only
and requires explicit `eof "feof"(stream)`, `ferror "ferror"(stream)`, and
`error errno when ferror != 0` clauses. The physical ABI remains the C
`fread` call; size and count are unsigned, stream is a pointer, and `T` is
currently restricted to an 8-bit integer. `ferror` is queried first and has
precedence; an error returns `Err(errno)` without publishing a partial prefix.
The normalized projection aliases `element_count` as its capacity lane. The
independent `ferror` accessor has a signed `i32` physical carrier and is kept
separate from the unsigned `fread` result in the LowIR outcome plan.
With no error, a full count returns `Data`, a short count with `feof != 0`
returns `Eof`, and a short count without either condition traps. Published
data length is returned-count times element-size; overflow traps before the
call. Zero size or count returns empty `Data` without reading accessors and
allows a null buffer. Accessor symbols are explicit artifact facts and are
never inferred from symbol names.

`result == invalid` resolves the invalid sentinel declared by the nominal
result type and is rejected for raw values or types without such a declaration.
Integer predicates may use local integer `foreign const` and foreign enum
constant names. `through` is an inclusive range; `in` and `not in` sets contain
at most 64 items. Elaboration checks every value against the physical result
carrier, then sorts, merges, and persists target-width intervals. LLVM never
resolves constant names or reconstructs this normalization.

Provider queries write a provider-filtered `.interop` sidecar to the Next
artifact store. Its deterministic path includes the package contract, target,
and current semantic epoch. A dependent query reuses the completed provider
sidecar, and a later invocation probes that path before compiling; a digest or
epoch disagreement is rejected rather than silently recompiling an old artifact.

The Chtholly public interface contains ordinary imported signatures. Interop
facts are loaded from the CFDL artifact by the compiler boundary and are not
written as `extern`, `bind`, or inline FFI contracts in Chtholly source.

`foreign type T` always establishes a stable nominal identity. Carrier
completion is independent: it may remain incomplete, select an integer or
pointer, or publish an ABI-only record. No carrier field is exposed as a
Chtholly operation. Protocol identities and edges are derived only from
callable `where` facts and never from the selected carrier.

Each published CFDL capability retains the complete normalized relation shape
as canonical key/value literals. This keeps target places, endpoints, action
identities, and predicates available to artifact consumers; consumers must not
reconstruct them from a relation name or C symbol spelling.

Protocol actions and events also carry structured canonical identities. An
identity contains the provider package, module, foreign resource, protocol
kind, and stable name fingerprint. Event and obligation edges reference those
identities directly; package closure never matches actions or events by a
local ordinal or a bare spelling.

## Removed Surface

The following forms are not part of Next CFDL:

```text
operation { ... }
capability paths such as resource.* or callback.bind
foreign contract declarations
Chtholly fn ... bind ... declarations
Chtholly unsafe extern "C" declarations
```

Old operation-object artifacts and source files are incompatible and fail
closed. They are not migrated by the new frontend.
