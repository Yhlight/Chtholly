# Explicit Carrier Object View

## Boundary

`core::carrier(self)` is a reserved compiler intrinsic. It does not participate
in name lookup and does not create a public callable entity. It is accepted
only in these canonical roles:

- `ProjectionLoad`, `ProjectionStore`, `ProjectionTake`, `ProjectionInit`
- `ProjectionBorrow`, `ProjectionBorrowMut`
- `ObjectInit`, `ObjectCopyInit`, `ObjectMoveInit`, `ObjectDrop`

The argument must be the first, direct owner-reference parameter. The result is
a reference to the semantic witness's object carrier and inherits the input
mutability. A local reference binding may retain this view. There is no general
logical-object to carrier conversion and no representation helper call ABI.

## Region And Escape ABI

Each `SemCarrierView` stores the logical contract field selected by its
projector. Object-shell roles store the owner's field count as the complete
carrier sentinel. This identity survives generic template serialization,
concrete SCC materialization, and lowering. It avoids recovering projector
identity from a function spelling or a physical GEP.

A projector may navigate parent objects while reaching its region, but every
observable endpoint must have the canonical `region_indices` path as a prefix.
An omitted region means the complete carrier. Shell roles can access hidden
carrier state. Raw views cannot enter an ordinary call or aggregate, undergo an
owned copy/move, or cross a return boundary. Borrow projectors may return a
derived stable reference inside their region; they may not return the raw view.

## IR Ownership

SemIR records `CarrierView(Inst, Integer)` and replays provenance through local
reference aliases, dereference, field projection, borrow, move, copy, and
assignment. The SemIR verifier independently checks canonical role, owner
parameter, mutability, contract field, escape sinks, and region endpoints.

LowIR records `CarrierView(Value, Field)`. Its verifier checks that the source
is loaded from the first canonical parameter, that the result pointee equals
the witness-selected object type, and that the field/shell contract did not
change during lowering. It independently replays value and slot aliases and
rejects LowIR call, aggregate, return, and region escapes. LLVM lowers the
instruction to its input address. It performs no role lookup, region analysis,
or representation selection.

The former `DereferenceObject` shortcut is now restricted to value
representation `pack/init`. Projectors and object-shell functions cannot regain
implicit logical-field access through ordinary `*self`.

## Borrow Dataflow

The previous source-order last-use approximation has been replaced. The
place-state phase builds instruction CFG edges for fallthrough, `if`, `while`
condition/body/backedge/exit, and `return`. A forward fixed point propagates
reference loans through SSA values, locals, assignments, and call return-source
provenance. A backward fixed point computes SSA and local liveness. Conflict
checks use only loans that are both path-reachable and live at the operation.

Canonical physical paths and half-open bit ranges remain the overlap identity.
Branch-only loans do not remain active in the continuation, while loop-carried
loans converge over the real backedge.

## Artifact Versions

- `GenericTemplateOpcode::CarrierView`
- `CHNXSCC10`, component version 7, persists callable semantic contracts and
  ownership summaries
- `CHNXTPK16`, state version 16
- cache namespace `next-v17`

Type, nominal-specific, witness, and layout wire formats remain `CHNXTYPE7`,
`CHNXSPEC9`, `CHNXWIT7`, and `CHNXLAY4` because their wire layouts did not
change.