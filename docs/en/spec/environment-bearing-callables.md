# Environment-Bearing Callables

This document specifies the first Chtholly 1.2 callable wave. It builds on the
frozen v1 function-value ABI and the 1.1 transfer/share model.

## Source Surface

A closure expression uses the ordinary function introducer with a capture
list:

```chtholly
let offset = 2;
let add = fn [copy offset](value: i32): i32 {
  return value + offset;
};

var counter = fn [var value = 0](): i32 {
  value += 1;
  return value;
};
```

`copy name` and `move name` create immutable fields initialized by the
corresponding explicit ownership operation. `var name = expression` creates a
mutable field. Initializers execute exactly once from left to right. A closure
body must explicitly capture every enclosing local it names; module entities,
constants, types, and callable declarations are resolved normally and are not
captures.

Checked references cannot be captured in this wave. Borrowed value types such
as slices retain their complete provenance and do not receive lifetime
extension. Duplicate captures, capture/parameter name collisions, self-use in
an initializer, and implicit local capture are ill-formed.

## Type And Invocation

Every closure expression defines a unique, anonymous, non-nameable type. Its
value representation is the ordered tuple of capture fields. A closure with no
captures is still distinct from `fn(T...): U` and does not implicitly convert
to a function value.

A body that only reads its environment is invoked through `const Self&`. A
body that writes or mutably borrows a capture, or moves an individual capture
field, is invoked through `Self&` and therefore requires a stable mutable
place. A `var` capture establishes that capability immediately; the other
operations are derived from the checked body. An explicit `move capture`
expression consumes only that environment field, so it does not create an
implicit once-call ABI. Whole closure values retain the ordinary affine move
operation, and a closure whose body consumes a field must not be reused after
that field has been moved.

The standard library publishes two ordinary interfaces:

```chtholly
pub trait Invoke<Args> {
  alias Output;
  pub fn invoke(self: const Self&, args: Args): Output;
}

pub trait InvokeMut<Args> {
  alias Output;
  pub fn invoke(self: Self&, args: Args): Output;
}

pub trait InvokeOnce<Args> {
  alias Output;
  pub fn invoke(self: Self, args: Args): Output;
}
```

`Args` is one tuple type (`()` for no arguments). Read-only closures get an
`Invoke<Args>` witness; closures with `var` captures get an `InvokeMut<Args>`
witness. The witness function is a compiler-generated tuple adapter that
projects the tuple and calls the existing hidden body. Direct closure calls
remain direct calls and no erased representation is introduced.
`InvokeOnce` is reserved for callable environments whose invocation consumes
the complete environment. The bound-method wave is its first producer; owned
closures do not implicitly acquire consuming invocation in this document.

## Lifecycle And Escape

Closure copy, move, and destruction capabilities are derived field by field.
Destruction runs live captures in reverse initialization order. A failed
initializer cleans only earlier fields, and moving the complete closure
transfers its single cleanup obligation.

Closure escape provenance is the union of captured values and body return
provenance. Capturing a slice or another borrowed aggregate cannot hide a local
source, extend a temporary, or cross a return/task boundary that the captured
value could not cross directly.

## Concurrency

`transferable` and `shareable` are structural conjunctions of the capture
facts. Unknown imported facts fail closed. A closure environment that is live
across a coroutine suspension must be transferable; task construction and
shared access retain their existing 1.1 checks. This wave does not broaden the
coroutine rule to unrelated nominal types. Checked references are excluded from
capture; slices and raw pointers remain non-transferable under the 1.1 rules.

## Lowering And ABI

The environment is an ordinary aggregate with no hidden allocation. The
compiler emits one hidden body per closure expression. Its first parameter is
the checked environment reference with read-only or mutable authority, followed
by the source parameters. Known closure calls lower to direct calls. There is no
erased callable representation in this wave.

SemIR retains formation as an explicit `Closure` instruction containing the
hidden target and ordered capture block. A session-owned callable-environment
record binds the anonymous nominal type to its kind, invocation target,
formation target, capability, and stable identity. Lowering still emits the
same field-only aggregate representation; the explicit instruction is a
checked semantic and artifact boundary, not a runtime ABI change.

The hidden identity consists of the owning callable identity, concrete generic
arguments, and a stable structural closure ordinal. Source addresses and
process-local IDs never enter public or cache fingerprints.

## Public Templates

Public generic templates encode an explicit closure opcode, target entity,
capability, ordered captures, nested body regions, environment construction,
and calls. Materialization creates a local anonymous type, restores the
closure-environment flag and callable-environment record, resolves the hidden
body from explicit generic type arguments, and derives lifecycle and
concurrency facts for the concrete specialization. Captures initialize the
environment and never participate in hidden-target generic argument deduction.
Callable-interface witnesses are
ordinary interface artifacts. Their identity includes the interface, tuple
arguments, concrete closure type, invocation capability, `Output`, and hidden
target fingerprint; imported witnesses are rematerialized and verified.
Generic witnesses and their adapter functions use the enclosing callable's
generic environment. The hidden body keeps its private materialization generic;
binding indices are remapped through the interface owner before requirement
signature comparison. Bodyless requirements publish a signature without a
generic-template body, while defaulted requirements retain their template.
This artifact schema is semantic epoch 10. The generic closure opcode is
append-only value 58, and the current concrete component is `CHNXSCC44`
format 41. Artifact verifiers reject an unknown capability, stale or missing
target, non-nominal environment, inconsistent capture types, and inconsistent
receiver authority before lowering. Driver session identities include
both semantic and standard-library epochs, so an epoch change starts a cold
session while leaving prior CAS entries available for ordinary garbage
collection.

An anonymous closure type is valid only inside template-local types and
regions. The artifact verifier rejects it in public function signatures,
aliases, nominal fields, foreign declarations, and component ABI surfaces.
