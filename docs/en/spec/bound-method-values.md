# Bound Method Values

This document specifies the Chtholly 1.2 bound-method callable ABI. It extends
the environment-bearing callable model without introducing a delegate,
type-erased storage, or a second aggregate representation.

## Formation

An instance member expression forms a bound method value when it is used
outside a direct call:

```chtholly
let read = value.get;
var update = value.replace;
let consume = (move value).take;
```

The receiver is evaluated exactly once. The selected method must be determined
uniquely from the receiver alone. A same-name overload set with more than one
viable instance method is ambiguous; later callable arguments do not reopen
member resolution. Associated functions do not form bound methods.

The bound callable has the method's complete explicit parameter list after
`self`. Callable invocation is positional and does not retain source named or
default argument sugar. The first 1.2 wave admits synchronous safe inherent
methods, including imported methods and owner-generic specializations. Trait
requirements, method-level generics, async methods, and unsafe or foreign
methods are excluded.

## Receiver Capability

The method's declared `self` parameter fixes both capture and invocation:

| Method receiver | Environment field | Callable interface |
| --- | --- | --- |
| `const Self&` | `const Owner&` | `std::callable::Invoke<Args>` |
| `Self&` | `Owner&` | `std::callable::InvokeMut<Args>` |
| `Self` | `Owner` | `std::callable::InvokeOnce<Args>` |

Borrowed formation requires the same stable place and authority as a direct
method call. A temporary cannot be extended by forming a borrowed bound value.
An owned receiver requires `(move value).method`, and consuming the resulting
callable requires `(move bound)(arguments)`. These are separate visible
ownership transfers.

`InvokeOnce` is an ordinary versioned interface:

```chtholly
pub trait InvokeOnce<Args> {
  alias Output;
  pub fn invoke(self: Self, args: Args): Output;
}
```

`Args` is the tuple of explicit method parameters and `Output` is the concrete
method result. Each bound method receives exactly the interface matching its
receiver capability; this wave does not infer a capability hierarchy.

## Lifetime And Concurrency

A borrowed environment carries the receiver loan as an ordinary aggregate
field. Its lifetime begins when the member expression is evaluated and ends at
the bound value's last use. A mutable receiver retains exclusive authority for
that interval. Moving or copying a bound value does not detach or lengthen its
loan, and returning or task-transferring it cannot hide a local source.

Shared-reference lifecycle follows the checked-reference field. Mutable
borrowed bound values are move-only. Owned bound values derive copy, move,
destruction, and transferable facts from the receiver field, but are never
shareable because invocation consumes their environment. Partial-move cleanup
must not destroy the owned receiver after a successful invocation.

## Semantic And Artifact Model

SemIR retains bound-method formation as an explicit fact containing the
adjusted receiver, concrete target function, and receiver capability. The
physical value is still a one-field anonymous callable environment. Calls use
a compiler-generated adapter and the same tuple-projection witness path as
closures.

The callable-environment record separates the adapter invocation target from
the receiver-selected formation target and gives the environment a stable
fingerprint. Adapter bodies place every `SemParameter` in the function-body
prefix before projecting the receiver, including methods with explicit
arguments. SemIR verification checks the environment flag, one-field receiver
shape, target receiver authority, capability, formation target, and identity.

Generic artifacts encode the receiver operand, canonical target entity,
concrete target arguments, and capability. Materialization restores the
specific function before rebinding the receiver. Target fingerprints and
receiver shape are verified; process-local function or type indices are never
artifact identity. Anonymous bound types remain forbidden in public
declaration signatures.

Public generic materialization and cached concrete materialization restore the
bound-method environment flag and callable record from canonical target
identity and receiver shape. The current concrete component schema is
`CHNXSCC44` format 41 and the package semantic epoch is 10. Malformed target
indices, capabilities, receiver operand blocks, target fingerprints, and
receiver/target authority combinations fail before lowering.

## Deferred Surface

Bound-method equality, conversion to `fn`, erased storage, trait-bound and
extension methods, async invocation, unsafe/foreign authority capture, and
operator protocols remain outside this wave.
