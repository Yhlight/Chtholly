# Chtholly v1 Language Roadmap

Status: frozen normative scope catalog for the Chtholly 1.0 baseline. Every
1.0 entry is normative and complete under `support/chtholly-v1.toml`; there are
no remaining 1.0 closure waves. Entries marked `post-v1` are relative to the
1.0 source version and reserve neither syntax nor behavior in 1.0. The later
`v1.1` through `v1.10` manifests layer additional frozen source versions on
this baseline. 

## Explicitly Deferred

The frozen 1.0 language does not include foreach or iterator-loop syntax,
public asynchronous or coroutine syntax, macros, compile-time reflection, a
separate class keyword or inheritance hierarchy, implicit exception unwinding,
or unrestricted jumps. Compile-time value parameters for generics require
their own design review and have no admitted spelling. Adding any of these to
1.0 requires a new versioned surface rather than an implementation-only
change. Bound method values, environment-bearing callable values,
operator-overloading, module aliases, labeled loop control, and the
concurrency memory boundary are post-1.0 entries; the later frozen roadmaps
record which of them are implemented in 1.1–1.10.

## 1 Conditional Expressions

Status: `normative`; implementation complete.

- Require parentheses around `if` and `switch` controlling expressions.
- Preserve exhaustive, non-fallthrough `switch` semantics and payload patterns;
  only the control-expression exterior becomes C-shaped.
- Define expression-valued `if` and `switch`, branch type convergence, required
  `else` coverage, temporary cleanup, and `never` convergence.
- Expression `if` requires value blocks and a final `else`. Reachable branches
  initialize one unobservable result; divergent branches initialize nothing.
  The result is a temporary value and never a place.

## 2 C Style Loops

Status: `normative`; implementation complete.

- `for (initializer; condition; step)` admits one declaration, expression, or
  assignment initializer and one expression or assignment step. Every clause
  may be omitted; an omitted condition is `true`. Comma lists are not admitted.
- `do { ... } while (condition);` executes its body before its first condition.
- Unlabeled `break` and `continue` target the nearest loop and execute all
  exited lexical cleanup. `for` continue enters step; other continue edges
  enter their condition. Foreach, iterator syntax, and loop labels are outside
  this completed entry.
- Assignment in a `for` clause remains a `void` statement form rather than a
  general assignment expression.

## 3 Implicit Coercions

Status: `normative`; the v1 scalar and authority matrix is implemented.

- Admit only value-preserving automatic conversions, capability narrowing, and
  contextual literal fitting that can be decided mechanically.
- Signed integers widen to the same signedness. Unsigned integers widen to the
  same signedness or to a strictly wider signed integer. `f32` widens to `f64`.
  Integer-to-float conversion is implicit only when the destination precision
  represents the complete source type domain exactly. No float-to-integer or
  signed-to-unsigned conversion is implicit.
- Reference and raw-pointer authority may narrow without changing pointee
  identity. Nominal and callable values have no implicit cross-type conversion;
  exact identity remains admissible.
- Contextual integer literals fit an integer target by mathematical value and
  a float target only when exact. Contextual decimal float literals may round
  directly to `f32`; otherwise their default type is `f64`.
- Keep implicit coercion distinct from explicit conversion and checked
  conversion, including during overload resolution.

## 4 Temporaries And Value Model

Status: `normative`; implementation complete for synchronous expressions and
structured cleanup edges.

- Complete the value, object, storage, place, and temporary model in the
  abstract machine.
- Define materialization, full-expression boundaries, lifetime extension,
  result slots, reverse cleanup, final-use transfer, and borrow escape.
- Require identical behavior for local, generic, imported, and lowered code.
- Control-expression results use an unobservable result slot initialized once
  by the selected reachable branch. Binding transfers the selected temporary;
  branch-local cleanup cannot destroy the transferred result.
- Materialization, full-expression cleanup, and direct read-only reference
  lifetime extension use distinct semantic operations. Storage owns no cleanup;
  materialization registers it, the full-expression edge consumes it, and a
  direct reference binding transfers it to lexical cleanup.
- A stable field borrow extends its complete temporary root. Temporary
  references cannot escape by return. Statement, call-argument, condition,
  exact-error residual, generic, artifact, and native lowering paths preserve
  reverse-order, exactly-once cleanup.
- Coroutine suspension and cancellation cleanup remain post-v1 work and do not
  weaken this completed synchronous model.

## 5 Result

Status: `normative`; the complete v1 rules are frozen in
`failure-lifecycle-and-storage.md`.

- Freeze the standard `Result<T, E>` representation contract and construction
  surface owned by the standard library.
- Formally define single evaluation, payload transfer, residual early return,
  exact error identity, cleanup, temporary handling, and borrowed payloads.
- V1 admits exact error identity only, with no implicit conversion or
  user-programmable residual protocol.

## 6 Interfaces

Status: `normative`; implementation complete for the static v1 model frozen in
`interfaces-and-specialization.md`.

- Define interface declarations, requirements, default definitions,
  conformance, visibility, coherence, associated members, and artifact identity.
- Separate static conformance from explicitly erased dynamic dispatch.
- Define object-safety and ABI rules before admitting interface values.

## 7 Explicit And Checked Casts

Status: `normative` for numeric conversion and authority narrowing;
implementation complete for that subset. Foreign ABI adaptation belongs to
CFDL and is outside this source entry.

- Define `as` for explicit, deterministic conversions and `as?` for checked
  conversions with a typed failure result.
- Keep numeric conversion, interface conversion, ABI adaptation, pointer
  conversion, and bit reinterpretation as distinct semantic categories.
- Require unsafe authority for conversions that depend on external validity,
  provenance, or representation assumptions.
- `as?` returns
  `std::result::Result<T, std::convert::CastError>` and requires explicit
  imports of both modules. `CastError` distinguishes `Inexact`, `OutOfRange`,
  and `NonFinite`. Interface downcasts, provenance-changing pointer casts, and
  bit reinterpretation remain closed until their owning entries are normative.

## 8 Function Overloads And Arguments

Status: `normative`; implementation complete for free and inherent function
calls.

- Free, associated, and instance callables form owner-qualified overload sets;
  return type alone never distinguishes an overload. Constructor overloads
  remain owned by entry 9.
- Trailing defaults are concrete constant-evaluator results. Named arguments
  use `.name = value`; positional arguments must precede them, and public
  parameter names are source-interface facts.
- Generic specialization precedes conversion ranking. Candidates compare
  worst rank, total rank, then prefer a non-generic candidate; an equal best
  tuple is ambiguous.
- Artifact-only calls, owner-qualified tooling, incremental observations, and
  native symbols retain exact overload identity. The complete rules are in
  `declarations-callables-and-generics.md`.

## 9 Nominal Construction And Destruction

Status: `normative`; implementation complete for the canonical constructor,
compiler-only destructor, and placement lifecycle boundary. Nominal type means
`struct`, `enum`, `union`, or a later explicitly admitted nominal family, not
a separate class hierarchy.

- Define one canonical initialization role and one destruction role per
  applicable concrete nominal type.
- Require complete initialization on normal construction, reverse cleanup on
  failure, exactly-once destruction, and compiler-only destructor invocation.
- Keep allocation separate from construction and named factories separate from
  the canonical constructor role.
- `impl T { fn init(...): Self | Result<Self, E> }` is the canonical source
  constructor and is called only as `T::init(...)`. Representation `init` is a
  separate role. `drop(self: Self&)` is compiler-only.
- Constructor ABI epoch 1 uses a caller success slot. The fallible lane also
  uses a `Result<void, E>` outcome slot; this result kind is part of public
  callable identity.

## 10 Builtin Operators

Status: `normative`; implementation complete for the v1 builtin scalar set.

- The admitted set is prefix `+ - ~ !`, arithmetic `+ - * / %`, shifts,
  bitwise operators, scalar comparisons, `<=>`, short-circuit logic, and the
  corresponding compound assignments.
- Checked integer failure, IEEE floating behavior, precedence, result types,
  and left-to-right sequencing are frozen in
  `expressions-and-control-flow.md#builtin-operators`.
- `<=>` returns explicitly imported canonical `std::compare::Ordering`.
  Pointer arithmetic, `++`, `--`, comma, ternary, assignment expressions, and
  open user-defined lookup are not part of the builtin core.

## 11 Constants And Constant Evaluation

Status: `normative`; declarations, the closed evaluator, local and imported
`const fn`, public evaluator/value artifacts, and diagnostics are implemented;
entry 11 is complete for the admitted concrete v1 evaluator.

- Define `const` declarations, `const fn`, permitted operations, evaluation
  limits, diagnostics, caching, target dependence, and artifact identity.
- Keep compile-time values distinct from immutable runtime storage.
- Treat value parameters for generics as a separate design question. Reserve
  the capability need without selecting another language's surface.
- The frozen rules and explicit remaining boundary are in
  `constant-evaluation-and-layout.md`.

## 12 Evaluation Order

Status: `normative`; implementation complete for every expression and cleanup
form admitted by v1.

- Operands, ordinary call arguments, aggregates, and conversions evaluate
  left-to-right exactly once. Conditional and logical forms evaluate only the
  selected arm. Compound assignment uses place, read, RHS, checked operation,
  commit order.
- Defaults, named arguments, constructors, `defer`, and exact-error residual
  flow use this order. Every future expression form must supply its own
  conformance evidence without reopening this entry.
- Optimization may not change observable evaluation or cleanup order.

## 13 Defer

Status: `normative`; implementation complete for synchronous structured exits.

- The only source form is `defer { statements }`. Executing it registers the
  body with the nearest lexical block; it does not execute the body immediately.
- Deferred actions and owned-local lifetime registrations form one timeline.
  Leaving the block executes that timeline in strict reverse registration order.
  Captured places are read at cleanup execution time, not copied at registration.
- Normal fallthrough, `return`, exact-error `?` residual return, `break`,
  `continue`, and direct or fallible constructor exits run the applicable
  deferred actions. A return value is transferred to its result slot before
  cleanup begins.
- A defer body must fall through. Nested `defer`, `return`, `?`, and a
  `break` or `continue` that would escape the defer body are rejected. Loops,
  conditionals, and control targeting a loop declared inside the body remain
  valid.
- `defer` supplements nominal destruction; it does not replace object
  lifecycle. Trap/assert termination and coroutine suspension cleanup are not
  structured defer edges in this version.

## 14 Aliases And Interface Constraints

Status: `normative`; implementation complete for the v1 source and identity
rules frozen in `interfaces-and-specialization.md`.

- Use `alias Name = Type;` and allow `pub alias` across module boundaries.
- Define generic alias substitution, recursion, identity transparency,
  diagnostics, and component fingerprints.
- Define the canonical interface constraint and projection syntax without
  introducing alternate compatibility spellings.

## 15 Layout Queries

Status: `normative`; `sizeof(T)` and `alignof(T)` are implemented for the
closed queryable object-representation set.

- Define `sizeof`, `alignof`, and any admitted controlled layout query as
  compile-time values.
- Respect incomplete, opaque, generic, custom representation, and `repr(C)`
  boundaries.
- Prevent a query from exposing private physical layout not committed by the
  semantic interface.
- The target-dependence and opaque/representation boundary are frozen in
  `constant-evaluation-and-layout.md`.

## 16 Bound Methods

Status: `post-v1`; no source form is reserved by v1.

- Define whether and how an instance method may form a callable value with a
  shared, mutable, or owned receiver.
- Specify receiver evaluation, borrow duration, move capture, callable
  capability, cleanup, equality, and component representation.
- Keep unbound instance-method references separate from associated functions.

## 17 Structured Destructuring

Status: `normative`; local, parameter, and assignment forms are complete for
nominal fields and fixed-array/tuple positions, including nested paths and
strict rest validation.

- Use braces for local, assignment, parameter, and pattern destructuring. The
  implementation accepts local `let`/`var` declarations, parameters, and
  assignment destinations. Assignment validates all destinations before it
  commits stores.
- Each binding spells `move`, `copy`, or `&`; omission is diagnosed. Projection
  sources are named fields or array/tuple positions, and are evaluated from
  one source expression in source order. A move consumes a place once, copy
  requires the normal copy proof, and `&` creates a checked borrow.
- Partial binding uses the normal placement/cleanup graph; no implicit copy or
  source lifetime extension is introduced by destructuring.
- Tuple destructuring uses explicit positional sources such as
  `{ first = .0, second = .1 }`; parenthesized destructuring is not admitted.
- Projection paths may contain multiple nominal-field or fixed-array steps.
  Tuple positions use the same canonical path. A selected path covers its
  complete terminal subtree; two identical paths or paths where one is a
  prefix of the other overlap and are rejected. Without rest, the selected
  paths must cover every logical leaf of the source aggregate. A single `..`
  may appear only as the final item and ignores all uncovered leaves; it never
  introduces a remainder binding, performs a transfer, or extends a lifetime.
  Borrowed-slice indexing and slicing are available as expression sources with
  checked source provenance; runtime-length slice destructuring is not
  admitted. Neither form acquires an implicit ownership conversion.

## 18 Null And Raw Pointers

Status: `normative`; contextual null, typed-null artifact replay, and
raw-pointer safety checks are implemented for the admitted v1 subset.

- Restrict null to raw-pointer and foreign domains; checked references remain
  non-null.
- `null` is contextually typed as a raw pointer and has the native zero-pointer
  representation. It has no dereferenceable provenance and cannot be converted
  to a checked reference.
- Equality and inequality comparisons with null are admitted. Pointer
  arithmetic remains outside the builtin arithmetic set; dereference and
  member access require unsafe authority, and constant null dereference is
  rejected even inside an unsafe region.
- Formation, provenance, alignment, aliasing, one-past values, and foreign
  pointer contracts remain explicit ABI/runtime responsibilities; no hidden
  nullable-reference type is introduced.
- Public generic templates encode a typed `NullPointer` operation. Concrete
  cross-module components must materialize it to a raw-pointer result before
  verifier publication; null never acquires checked-reference provenance.

## 19 Callables

Status: `post-v1` for environment-bearing callable values. V1 retains free and
associated function values; foreign ABI callable values belong to CFDL and no
closure-environment source form is reserved.

- Define environment-bearing closures and their relationship to bound methods
  and explicit adapters without changing the v1 non-capturing callable model.
- Specify environment ownership, shared/mutable/consuming invocation,
  conversion, capture, cleanup, generic identity, effects, and ABI erasure.
- Do not assume a pre-existing callable-interface hierarchy or closure
  punctuation.

## 20 Operator Overloading

Status: `post-v1`; no operator declaration syntax is reserved by v1.

- Define which operators are overloadable and their required interface
  protocols, arity, receiver capabilities, return rules, and lookup.
- Builtin candidates and user candidates participate in one deterministic
  resolution process.
- Assignment, address formation, ownership transfer, unsafe authority, and
  short-circuit control flow are not overloadable unless separately justified.

## 21 Numeric Model

Status: `normative`; implementation complete for the v1 builtin scalar numeric
model.

- Scalar widths, signedness, literal typing and suffixes, lossless conversions,
  common types, overflow, division, remainder, shifts, floating-point behavior,
  and casts are frozen by the normative type and expression specifications.
- Language behavior must not change between debug and optimized builds.
- Checked, wrapping, saturating, and explicitly unchecked operations require
  stable language or standard-library contracts.
- Unsuffixed integers select `i32`, then `i64`; values beyond `i64` require an
  unsigned suffix. Unsuffixed decimal floating literals are `f64`. Integer
  arithmetic admitted by this subset traps on overflow in every build mode;
  constant overflow is diagnosed.

## 22 Aggregate And Text Types

Status: `normative`; fixed arrays, tuples, borrowed slices, aggregate
projection, UTF-8 literals, and public generic aggregate materialization are
implemented; library-owned text remains outside the builtin ABI.

- Fixed arrays use explicit element type and length, checked indexing, and
  aggregate construction. String literals are decoded UTF-8 values; `cstring`
  literals remain explicit raw-pointer values.
- `slice<T>` is a builtin read-only non-owning pointer-plus-length view and
  `slice_mut<T>` is its explicit mutable counterpart. Both are constructed by
  slicing an addressable array or slice, retain source provenance through
  tuples and projections, and introduce no ownership transfer or source
  lifetime extension. Owned strings remain library types.
- Ownership analysis tracks fixed-array elements and tuple positions as
  independent leaves. Static projections support element-level move, borrow,
  initialization, and postconditions. Dynamic array projections use a single
  wildcard overlap, so a dynamic access is checked against every element.
  Slice projections use the same wildcard representation but are borrowed-only:
  move and initialization effects/postconditions are rejected at the artifact
  boundary as well as during body checking.
- FFI text conversion and C-string contracts remain explicit at the foreign
  boundary.
- Tuple types and construction use `(A, B, C)` and `(a, b, c)` respectively;
  destructuring follows the brace rule in entry 17. Public generic templates
  and concrete specialization components preserve tuple literals containing
  borrowed slices, string/cstring literals, fixed-array literals, and typed
  nulls across module boundaries. Aggregate ABI facts are structural and do
  not add hidden ownership or pointer metadata.
- Keep dynamic collections as standard-library nominal owners rather than
  special tracing containers.

## 23 Never And Unreachable

Status: `normative`; implementation complete for the admitted divergence
sources. Explicit unreachable assertions remain with entry 24.

- Define `never` as the type of computations that do not produce a value.
- Integrate return, termination, proven infinite loops, unreachable code,
  branch convergence, overload resolution, and generic substitution.
- Specify diagnostics and optimization authority for explicit unreachable
  assertions.
- `never` has no value or object representation. It is admitted as an ordinary
  Chtholly return type and generic argument, rejected in storage and parameter
  positions, and rejected throughout C/component transport.
- Calls returning `never`, all-diverging control expressions, and proven
  constant-true loops without a reachable current-loop `break` do not fall
  through. Unreachable source is warned and still checked.

## 24 Unrecoverable Failure

Status: `normative`; the complete v1 rules are frozen in
`failure-lifecycle-and-storage.md`.

- Define assertion failure, bounds failure, failed mandatory checks, arithmetic
  traps, and violated unreachable assumptions.
- Select termination and optional cleanup policy without implicit exception
  unwinding.
- Forbid unwinding across C or component ABI boundaries unless a later ABI
  explicitly admits it.

## 25 Storage Duration

Status: `normative`; v1 consists only of the constant-initialized readonly
`static` subset frozen in `constant-evaluation-and-layout.md` and
`failure-lifecycle-and-storage.md`.

- Distinguish automatic objects, compile-time constants, read-only statics,
  mutable statics, thread storage, and externally owned storage.
- Define initialization dependencies, concurrency, destruction, module unload,
  and visibility.
- Require explicit authority or synchronization for mutable global state.
- Readonly statics have no run-time initialization or destruction. Mutable,
  thread-local, dynamically initialized, and destructible statics are post-v1.

## 26 Allocation And Placement Construction

Status: `normative`; implementation complete for local moved destinations,
direct and fallible constructor slots, commit/rollback, and cleanup transfer.

- Direct placement returns `void`. Fallible placement returns
  `Result<void, E>` and uses the ordinary postfix `?` flow.
- Keep heap allocation in explicit standard-library owner and allocator types;
  the core language does not add implicit allocation to construction.
- The admitted spelling is `T::init(args) in destination`. Arguments evaluate
  before the destination, which must be a mutable local place already in the
  moved state.
- `Ok` commits one cleanup obligation to the destination. `Err` leaves it
  moved and non-live. Constructor-owned locals clean up in reverse order, and
  placement performs no implicit allocation.

## 27 Names Scopes And Shadowing

Status: `normative`; implementation complete. The complete v1 rules are frozen
in `names-and-scopes.md`.

- Define value, type, module, label, member, overload-set, and generic name
  spaces.
- Freeze declaration point, lexical scope, shadowing, duplicate declarations,
  pattern bindings, loop scopes, default-argument scope, and visibility.
- Ensure tooling and imported artifact lookup use the same canonical rules.

## 28 Generic Inference And Specialization

Status: `normative`; implementation complete. Generic inference, constraint
witnesses, associated alias substitution, and portable specialization identity
are implemented. The complete v1 rules are frozen in
`interfaces-and-specialization.md`.

- Define inference inputs, explicit argument forms, constraint checking,
  coercion timing, overload interaction, recursion, and diagnostics.
- Preserve one canonical concrete specialization identity across modules and
  artifacts.
- Constant value parameters remain a separately reviewed extension of this
  model.

## 29 Module Aliases

Status: `implemented in Next`; module aliases are a closed post-v1 source
capability. Aliases affect only consumer lookup and never change canonical
provider identity.

- Define module aliasing and controlled imported-name exposure without wildcard
  ambiguity.
- Preserve canonical producer identity through aliases and re-exports.
- Specify conflicts, visibility, dependency observations, and tooling behavior.

Next implements the conservative spelling `import module as alias;` and
`export import module as alias;`. Aliases are local lookup keys, wildcard
imports remain forbidden, and provider fingerprints continue to use the
original package/module/entity identity.

## 30 Labeled Loop Control

Status: `post-v1`; normative spelling and implementation complete.

Labels use `name: while (...)`, `name: for (...)`, or
`name: do { ... } while (...);`. A transfer may use `break name;` or
`continue name;`; the unlabeled forms retain nearest-loop behavior. A label
binds exactly one enclosing structured loop and is visible lexically through
that loop's nested control-flow regions. Labels are not ordinary names and do
not enter artifacts or ABI identity. Duplicate active labels, unknown labels,
and labels on non-loop statements are errors.

`break` enters the selected loop's exit edge. `continue` enters its condition
edge, or the `for` step edge before the condition. All exited lexical cleanup
and `defer` scopes run in reverse order. A labeled transfer that escapes a
`defer` body is rejected; a transfer to a loop declared inside that defer body
remains local to the body. Labels do not introduce unrestricted jumps.

## 31 Core And Library Protocols

Status: `normative`; the compiler-owned and ordinary-library protocol boundary
is frozen in `interfaces-and-specialization.md`; implementation complete.

- Inventory equality, ordering, conversion, indexing, callable, copy, drop,
  failure, and other protocols required by language features.
- Keep compiler knowledge limited to semantic facts ordinary safe code cannot
  express; place reusable names and behavior in the standard library.
- Version compiler-known protocol identity in semantic artifacts.

## 32 Concurrency Memory Boundary

Status: `post-v1`; the abstract memory model already fixes the data-race-free
v1 baseline, but no atomic or volatile source surface is reserved.

- Define transferable/shareable capabilities, atomic access, ordering,
  synchronization, volatile device access, and foreign-memory interaction.
- Do not treat volatile access as synchronization or general type mutability.
- Keep low-level operations behind narrow library or intrinsic unsafe
  boundaries.

## 33 Public Callable Identity

Status: `normative`; implementation complete for the v1 free, associated,
instance, constructor, C-pointer, and explicit callback-adapter families.

- Public entity compatibility includes owner/member kind, source overload
  pattern, generic shape, result, safety, execution and semantic contracts,
  parameter names, and typed defaults.
- Native symbols combine a source-overload fingerprint with a concrete
  callable-ABI fingerprint. Authority remains in semantic/artifact identity,
  never in symbol parsing.
- Parameter names and defaults are source-interface facts but do not create a
  distinct source overload or native parameter lane. The encoding is frozen in
  `../design/next-call-resolution-and-callable-abi.md`.
- Post-v1 bound methods and environment-bearing callable values must extend
  these identities; their absence does not keep the admitted v1 families open.

## 34 Language Versioning

Status: `normative`; implementation complete. The complete v1 rules are frozen
in `language-versioning.md`.

- Define how packages select a language version and how artifacts record it.
- Keep source-language, semantic artifact, component ABI, runtime ABI, and
  standard-library epochs distinct.
- Specify candidate admission, freeze, replacement, migration, and cross-
  version dependency rules.

## 35 Lexical Grammar

Status: `normative`; implementation complete. The complete rules are frozen in
`lexical-and-grammar.md`.

- Source files are well-formed UTF-8 without a byte-order mark. V1 identifiers
  deliberately remain ASCII-only under `[A-Za-z_][A-Za-z0-9_]*`; Unicode
  identifiers, normalization, and confusable policy are post-v1.
- Comments, escapes, scalar literals, punctuation, maximal-munch behavior, and
  byte-based source locations are normative. Block comments do not nest.
- Decimal floats require digits on both sides of `.`, so `.5` is not a float
  and compact postfix `.0` remains a tuple projection.
- `Grammar.def` is the shared typed token-admission and precedence catalog used
  by the parser and machine surface audit. A reserved token is not an admitted
  production.

## 36 Nominal Data And Patterns

Status: `normative`; implementation complete for nested projection paths,
strict rest validation, projection artifacts, and cross-module generic
evidence.

- Struct and payload-enum aggregate construction, named/positional payload
  projections, explicit payload transfer, and exhaustive `switch` are checked
  in one pattern ownership model. Private fields and invalid/duplicate
  projections are rejected before code generation.
- A payload pattern first selects a statically known variant. Its binding
  sources may then use the same multi-step tuple, struct, and fixed-array paths
  as structured destructuring. V1 does not admit recursive variant subpatterns:
  entering another enum payload requires a separate `switch`.
- Patterns are validated completely before transfers or bindings are emitted.
  Their canonical generic-artifact form is the ordered executable projection
  chain: `EnumPayloadAccess` for the selected payload field,
  `StructFieldAccess` for tuple/struct positions, and `StaticIndex` for a fixed
  array. No second pattern-path metadata representation is persisted.
- Public enums are closed. Their `u32` discriminants are declaration-order
  indices, and adding, removing, reordering, or renaming a variant, or changing
  any payload field, changes the nominal definition fingerprint and is ABI
  incompatible. `..` and wildcard arms can preserve source compatibility only
  after recompilation; neither accepts an unknown runtime discriminant.
- Enum discriminant/representation, union validity, and lifecycle policies stay
  explicit. An open or non-exhaustive enum ABI requires a later language-
  boundary design and has no v1 spelling.
- Keep representation and lifecycle policies explicit and reject a generic
  attribute escape hatch that could bypass their verification.

## 37 Contextual Typing

Status: `normative`; implementation complete for all contexts admitted by the
current v1 expression and type surface.

- Local initializer and return expectations, literal target typing, callable
  context, branch convergence, null raw-pointer context, and nonempty array
  element convergence use one conversion query.
- Specify when context flows inward and when an expression must determine its
  type independently.
- Coordinate with coercion, overload resolution, generics, `never`, and
  diagnostics.
- The implemented subset propagates expected types into local initializers and
  `if`/`switch` arms, treats unsuffixed numeric literals as inference
  constraints, and otherwise requires one symmetric least-cost target under
  the normative entry-3 conversion matrix. `never` arms are excluded from
  candidates. The same rule is used for builtin numeric `+`, numeric equality,
  and array elements.
- Empty aggregates, tuples, slices, and environment-bearing callable literals
  remain owned by entries 22 and 19 rather than reserving hidden v1 behavior.

## 38 Foreign Linkage Boundary

Status: `normative` for the Next source split.

- Chtholly `.cns` has no foreign declaration, `extern`, `bind`, or inline FFI
  contract. Foreign linkage is authored only in a `.cfdl` unit.
- CFDL uses `foreign type`, `foreign fn`, resource-flow qualifiers, and finite
  `where` facts. It publishes target-neutral ABI and resource facts in an
  immutable Interop artifact.
- The Chtholly checker imports ordinary public entities and an opaque artifact
  reference. ABI layout, callbacks, completion, and external symbols are
  compiler-internal Interop projections.
- No foreign boundary implicitly grants ownership, provenance, safety, or
  representation validity; the artifact must state every obligation.

## 39 Unsafe Authority

Status: `normative`; implementation complete for the closed v1 operation set.

- `unsafe { ... }` and `unsafe expression` grant lexical authority only. The
  machine-readable `UnsafeAuthority.def` catalog enumerates every admitted
  operation and binds it to a stable diagnostic and specification identity.
- Unsafe authority does not disable typing, null rejection, initialization,
  ownership, cleanup, provenance, visibility, representation checking, or
  artifact verification.
- Intrinsics and compiler-only binding-author roles use verified, unspellable
  semantic contracts and cannot be reached through a naming convention.
- Raw-pointer arithmetic, atomics, volatile device access, and general
  representation casts remain post-v1 and reserve no compatibility spelling.

## 40 Program Model

Status: `normative`; implementation complete for the hosted-only v1 model.

- The sole v1 source entry is synchronous, non-generic, parameterless
  `fn main(): i32` in the requested root module. Freestanding startup, custom
  entry ABIs, and asynchronous entry syntax are post-v1.
- The generated host bridge is `main(int, char**)` or Windows
  `wmain(int, wchar_t**)`; embedders may call `chtholly.entry(): i32`.
- Startup initializes the runtime and registers host arguments. Normal return
  drains thread-static and program-static runtime drops before shutdown and
  returns the source `i32`; fatal termination promises no structured cleanup.
- V1 injects no prelude. Standard-library modules enter the package graph and
  namespace only after an explicit `import std...`.
- Existing asynchronous main and lexical task-scope execution remain post-v1
  implementation evidence and do not alter the frozen hosted contract.