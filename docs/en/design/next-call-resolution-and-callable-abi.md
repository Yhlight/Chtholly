# Next Call Resolution And Callable ABI

Status: implemented and reviewed for free functions, inherent members, generic
type specialization, default/named arguments, and ordinary native Chtholly
symbols. The language rules are normative in
`../spec/declarations-callables-and-generics.md`.

## One Resolution Pipeline

The semantic checker retains overload sets through lookup. It binds the
receiver separately, maps positional and `.name = value` source arguments to
parameter slots, fills missing slots from typed constant defaults, specializes
generic candidates, and ranks conversions. Local, imported, free, associated,
and instance calls share this planner. Generic instance methods keep owner type
arguments separate while specializing the underlying callable.

Candidate ranking uses `(maximum conversion rank, total conversion rank,
is generic)`. Equal best tuples are diagnosed as ambiguous. This deliberately
does not use declaration order, result type, or an open conversion protocol.
Function-value formation instead requires one exact contextual function type.

Defaults are constant artifacts, not hidden runtime thunks. Generic candidate
planning retains metadata from the template declaration while selecting a
concrete function reference, so default slots and parameter names are never
indexed through a specialization with different metadata ownership.

## Artifact Boundary

`CHNXTPK51` stores one parameter name and one optional typed default constant
for every parameter. Decoding requires exact vector lengths. Public lookup has
overload-aware free and member queries; owner, member kind, generic shape, and
parameter pattern distinguish bindings. Dependency observations include the
selected entity fingerprint, preventing a same-name sibling overload from
satisfying validation or incremental reuse.

Public function entity fingerprints use epoch 28 and public interface
fingerprints use epoch 29. Parameter names and defaults participate in public
source-interface compatibility, even though they do not distinguish source
overloads or native ABI lanes. The artifact cache namespace is `next-v26`.

## Native Symbol Identity

A native symbol keeps the readable canonical package/module/name prefix and
adds 24 hex digits formed from two SHA-256 prefixes:

1. a 12-hex source-overload fingerprint;
2. a 12-hex concrete-callable-ABI fingerprint.

The source fingerprint includes canonical owner/member kind, generic binding
shape, and the parameter type pattern. Type-parameter entity IDs are
normalized while binding positions remain significant. It excludes return
type, parameter names/defaults, `const`, and `unsafe`, because none may create
a source overload. Hash domain version 4 also removes the compiler-generated
readable `$specific$...` suffix before hashing, so different concrete instances
of one generic declaration retain one source lane.

The concrete fingerprint encodes the specialized function type recursively.
It includes stable nominal keys, scalar widths, arrays, references, raw and C
function pointers, ordinary and async functions, callback context parameters,
callable ownership contracts, callback registration fields, and canonical
foreign-resource protocol contents. Canonical compiler-generated semantic
helpers use their frozen owner/role/specific name plus this concrete type
because evaluator templates do not share a provider-local source declaration
shape. Foreign C names remain their declared external symbols.

## Review Boundary

The implementation rejects equivalent generic patterns after parameter
renaming, return-type-only overloads, duplicate instance signatures, duplicate
associated signatures, foreign overloads, and overloaded `main`. Cross-session
tests cover artifact-only defaults, named arguments, free/member overloads,
generic/non-generic overloads with equal concrete ABI types, native linking,
overload-isolated tooling references, independent public/source/concrete
identity mutation, and provider/consumer native symbol agreement.

Constructors, interfaces, extension methods, bound methods, and operator
overloads must extend this pipeline and identity scheme. Package scheduling is
still deferred until a representative large workspace demonstrates a measured
bottleneck.
