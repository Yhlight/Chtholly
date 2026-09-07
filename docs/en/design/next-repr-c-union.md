# Next `repr(C)` Union

Status: implemented vertical slice, ABI epoch 5.

## Language Boundary

Next accepts only explicit C-layout unions:

```cns
repr(C) union Value {
  integer: i64;
  pointer: void*;
}

let value = Value { .integer = 7 };
```

An initializer names exactly one member. Empty unions, field defaults,
lifecycle policies, custom carriers, and non-C representation are rejected.
Members must be trivially copyable, movable, and destructible C-compatible
object types. Generic definitions are delayed layout templates: each concrete
specific repeats compatibility and triviality validation after substitution.

The nominal `Struct` or `Union` kind is a semantic fact, not a lowering guess.
It crosses public entities, definitions, specifics, witnesses, layouts,
imports, generic templates, SemIR, and LowIR. Every decoder and verifier rejects
an absent, out-of-range, or inconsistent kind.

## Ownership And Active State

A union has no runtime tag. Its complete storage is one ownership/move leaf and
one alias region shared by every alternative. A member or nested-subplace move
moves the complete union; a borrow of one member overlaps borrows of every
other member.

Place analysis separately tracks `Active(member)`, a CFG-joined set of possible
members, or `UnknownActive`. Construction and direct member assignment select
one active member. Aggregate copy initialization and assignment copy known
nested-union states. A control-flow join unions member possibilities. Safe
member access requires exactly the selected member to be possible.

A foreign or otherwise opaque union result starts as `UnknownActive`. An
`unsafe` member access is a local assertion that the C active-member rule holds;
it neither refines the state nor authorizes a later safe access. Ordinary
initialization, move, loan, and cleanup checks still apply.

## Representation And Lowering

Every member has offset zero. Union size is the maximum member size rounded up
to maximum member alignment. `CHNXTYPE11`, `CHNXSPE13`, `CHNXWIT11`, and
`CHNXLAY8` persist and verify the nominal kind; layout verification permits
overlap only for a union. ABI epoch 5 invalidates older target layout requests.

SemIR and LowIR use distinct union construction and projection operations.
Their verifiers reject struct operations on unions, union operations on
structs, invalid member indices, and type drift. LLVM materializes storage as a
maximum-alignment carrier plus padding and consumes the verified LowIR member
identity instead of reconstructing union semantics.

For foreign calls, Windows x64 applies its 1/2/4/8-byte direct-coercion rule.
SysV AMD64 classifies every overlapping alternative and merges the resulting
eightbyte classes. Linux/ELF AAPCS64 never treats a union-containing object as
an HFA; it uses the ordinary up-to-16-byte integer transport or indirect
fallback. Large results use the same verified return-slot descriptor shared by
declarations and calls.

## Deferred Work

Variadic foreign calls will extend the fixed descriptor with a verified
call-site suffix after C default promotions. C callbacks will use explicit
function-pointer adapters, with entry/context/release state represented before
LLVM lowering. Tagged unions, non-trivial union members, explicit alignment,
packed layout, Windows ARM64, and Darwin ARM64 are separate design milestones.
