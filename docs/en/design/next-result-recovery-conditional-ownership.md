# Result Recovery And Conditional Ownership

Status: implemented compiler-owned semantic extension (2026-09-02).

## Scope

Chtholly keeps `Result<T, E>` as an ordinary nominal value. Recovery uses the
existing `switch` and `if` forms; this change does not add `recover`,
`try/catch`, implicit error conversion, or a user-programmable residual
protocol. Postfix `?` remains the explicit early-propagation form.

The compiler now carries a condition on each inferred callable postcondition.
The condition is a canonical bounded DNF over boolean callable parameters. It is
compiler-owned: source contracts continue to describe unconditional facts, and
the condition is not a second source-level effect language.

## Conditional postconditions

For a body such as:

```cns
fn update(target: Box&, enabled: bool): void {
  if (enabled) { target.value = 9; }
  return;
}
```

the ownership summary can contain an initialization fact for the `enabled`
path and a preservation fact for its complement. A caller may therefore
continue, retry, or return from either `Result` arm without treating a
conditionally initialized or invalidated place as unconditionally live.

Conditions are propagated through the ownership CFG, composed with nested
`if`/loop edges, substituted through calls, and canonicalized before they cross
public or incremental artifact boundaries. A condition that exceeds the
bounded representation widens to `always`, which is conservative for safety
and is recorded by the existing analysis metrics.

The initial condition vocabulary is intentionally limited to boolean
parameters and their `!`, `&&`, and `||` combinations. Enum/Result discriminant
predicates and suspension-local conditions remain future work because they
require a stable representation for returned outcome channels rather than
ordinary parameter predicates.

## Result recovery and cleanup

`switch` over canonical `Result<T, E>` moves only the selected payload. An
`Ok` arm owns its success payload and an `Err` arm owns its error payload; the
unselected payload is not initialized, moved, or dropped. Existing lexical
cleanup, `defer`, temporary lifetime, and place-state rules apply independently
to every arm. A recovered `Err` is an ordinary value-flow decision; cancellation
is not converted into `E`.

The semantic implementation records path conditions in the same append-only
ownership analysis used for move, assign, and callable postcondition facts.
LLVM lowering and Component ABI physical layouts are unchanged. Public and
incremental semantic fingerprints include the condition so stale artifacts
fail closed; their wire versions are incremented accordingly.