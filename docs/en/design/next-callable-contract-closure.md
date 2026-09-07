# Next Callable Contract Closure

Status: implemented semantic boundary for the existing `contract {}` syntax.

## Source Boundary

`contract {}` remains the existing Chtholly declaration form. It is not
rewritten into a fixed-width three-slot or four-slot grammar, and it is not a
CFDL `where` clause. The existing entry spellings remain the source contract:
`reads`, `writes`, `takes`, `borrows shared`, `borrows mutable`, `ensures`, and
`returns borrow` with the existing guard forms.

A callable definition with a body derives ownership facts from that body. A
bodyless declaration may publish an explicit contract summary. A declaration
cannot use both a body and a contract. CFDL continues to own ABI, foreign
resource flow, callback, event, and release facts.

## Semantic Closure

The checker stores the existing `CallableOwnershipSummary` and does not change
its serialized shape. Contract declarations participate in ordinary callable
identity and declaration merging. Repeated declarations must agree on both
contract presence and canonical summary contents. A contract-bearing forward
declaration is a valid external boundary and is emitted with external linkage;
it is not diagnosed as a missing local definition.

The ownership pass uses declared summaries for non-definition callables and
continues to compare inferred definition facts against an explicitly declared
summary when a body is present through another declaration path. `otherwise`
remains the complement of earlier return guards and is not a new artifact
fact.
