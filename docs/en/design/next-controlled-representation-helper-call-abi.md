# Controlled Representation Helper Call ABI

## Status And Boundary

The controlled representation-helper call ABI is implemented in the isolated
Next compiler. It is an internal semantic ABI. It does not add a source-level
way to name or call compiler helpers, and it does not make raw carrier
conversion public.

Every callable now carries one persisted `CallableSemanticContract` in addition
to its interprocedural ownership/effect summary. The contract is the authority
for checking, specialization, LowIR selection, LLVM ABI selection, dependency
observation, and artifact identity. Reserved `$representation$`, `$projection$`,
`$object$`, and `$lifecycle$` spellings are generated link identities only.
Importing never recovers authority from those strings.

## Contract

The canonical contract stores:

- semantic domain and role;
- the single capability bit implied by that role;
- the nominal owner as a structural `PublicType`;
- a stable logical projector field index when applicable;
- either whole-carrier authority or a canonical carrier field-index path;
- an optional half-open bit range.

The legal domain/role matrix is:

| Domain | Roles | Carrier authority |
| --- | --- | --- |
| `Ordinary` | `None` | none |
| `Lifecycle` | `Copy`, `Drop` | logical owner only |
| `ValueRepresentation` | `Pack`, `Init` | whole carrier |
| `ObjectProjection` | load/store/take/init/borrow roles | declared carrier path and optional bit range |
| `ObjectShell` | object init/copy-init/move-init/drop | whole carrier |

For a projection, an empty path is canonical only with `whole_carrier=true`;
a non-empty path is canonical only with `whole_carrier=false`. Domain, role,
and capability cannot vary independently. The owner must equal the first
reference parameter's pointee, and every role has an exact mutability, arity,
and return-shape contract.

## Call Authorization

Ordinary `SemCall` and `LowCall` accept only the `Ordinary` domain. Source name
lookup also rejects a non-ordinary target. Helper execution is introduced only
by dedicated semantic operations selected from a verified nominal witness:

- lifecycle operations require the exact owner and `Copy` or `Drop` contract;
- pack/unpack and converted initialization require the exact owner and
  `Pack` or `Init` whole-carrier contract;
- computed projectors require the exact owner, logical field, capability,
  carrier path, and bit range;
- object-shell operations require the exact owner and whole-carrier role.

The ownership/effect ABI is checked with the helper contract. Projection
effects rooted at the owner reference must name exactly the authorized logical
field and bit range. Effects on by-value operands describe a helper-local copy
and do not grant representation authority. Borrow return provenance must remain
inside the same authorized logical region.

`core::carrier(self)` remains a compiler intrinsic rather than a callable. It
is accepted only inside projector and object-shell roles. CarrierView analysis
reads the persisted carrier path from the current contract, and LowIR repeats
the role/owner/field/path checks before LLVM treats the view as an address.

## Generics, Imports, And Private Owners

A generic helper template persists a dependent owner type. Concrete
specialization substitutes its structural type arguments into the owner and
persists the complete concrete contract in `ConcreteSpecificNodeArtifact`.
Component verification binds the concrete contract to the node signature and
effect summary. Cache materialization independently computes the expected
substituted template contract and rejects any mismatch before rebuilding the
SemIR function.

Imported callables are materialized from the persisted contract. Projector
identity is mapped from the stable field index to the session-local field name;
no prefix or suffix parsing remains. A public or foreign owner must resolve
through the public nominal closure with the exact definition fingerprint,
generic arity, and field bound.

A helper owned by a private nominal is retained in its defining module's
artifact so local lowering can recover its ABI, but the private nominal is not
added as a public binding. Registration accepts an unresolved owner witness
only when its canonical package and module are exactly the module being
registered. A re-exporting or foreign module therefore cannot use that
exception.

## Persistence And Invalidation

- `CHNXTPK17`, state format version 17, stores full callable contracts and
  declaration/foreign ABI state.
- `CHNXSCC10`, component version 8, stores concrete substituted contracts.
- public entity/interface fingerprint domain is version 7.
- specialization component fingerprint domain is version 4.
- the artifact store namespace is `next-v17`.

Old state and component versions fail closed. Contract fields contribute to
entity, interface, specialization, package, and object identities. Decoders
reject invalid enum combinations, capability drift, malformed owner types,
non-canonical whole/path states, and invalid bit ranges. Registry, SemIR, and
LowIR verification then add the closure-aware checks unavailable to a wire
decoder.

## Source Syntax

The existing Chtholly forms remain the supported surface:

- `repr(value = T)` plus `impl core::ValueRepresentation`;
- `repr(object = T)` with stable, computed, or bit-packed field mappings;
- `impl core::ObjectProjection for Owner as projector`;
- `impl core::ObjectLifecycle for Owner`;
- restricted `core::carrier(self)` inside projector and shell bodies.

There is no syntax for semantic roles, capabilities, carrier paths, helper
names, or direct helper calls. Those are compiler-derived and persisted ABI
facts. 