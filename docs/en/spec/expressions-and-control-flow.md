# Chtholly Expressions And Control Flow

Status: normative for the frozen 1.0-1.8 source versions. Version-specific
extensions are identified where they differ from the 1.0 baseline.

## Conditional Expressions

Conditional control expressions use parentheses. `switch` retains exhaustive,
non-fallthrough arms and payload patterns. The canonical exterior is:

```cns
if (condition) {
  statement();
} else {
  alternative();
}

let selected = switch (value) {
  Option<i32>::None => 0;
  Option<i32>::Some { item = move .0 } => item;
};

let selected_if = if (condition) {
  prepare();
  42
} else {
  7
};
```

An expression-valued `if` requires `else`. Each arm is a value block. Only a
control-expression value block admits one final expression without a
semicolon; preceding expressions are statements and retain their semicolons.
`else if` is the nested expression form and must eventually end in an `else`
value block. A statement `if` may omit `else` and does not produce a value.

The condition is evaluated exactly once before either arm. Only the selected
arm is evaluated. Every reachable arm must produce a value adjustable to the
single result type. An expected type flows into every arm. Without one, the
compiler accepts only one symmetric common type under the lossless conversion
matrix; source order does not choose the result.
This rule also applies to expression-valued `switch`.

The result is a temporary value, never a place. Lowering may allocate an
unobservable result slot and initialize it exactly once from the selected
reachable arm. A diverging arm does not initialize the slot and does not
participate in type convergence. If every arm diverges, the expression has
type `never` and no result slot exists.

## Full Expressions And Temporary References

Materializing an owned temporary registers its cleanup with the current full
expression. Complete calls evaluate arguments left-to-right and destroy their
materialized temporaries in reverse order after the call. Discarded values with
nontrivial destruction are materialized before they are discarded. Declaration
initializers, assignments, expression statements, and return operands end their
full expression after the value has been consumed. Exact-error `?` performs the
same cleanup on its residual return edge.

Each `if` condition and each evaluation of a loop condition has its own full
expression. Cleanup runs after producing the `bool` value and before branching
to the body, alternative, backedge, or loop exit.

`let reference = &temporary_expression;` extends the complete materialized
temporary to `reference`'s lexical lifetime. The reference is read-only. A
stable field projection such as `&make().field` extends the complete `make()`
result. Function arguments retain full-expression lifetime only, and returning
a reference derived from a temporary is invalid. Aggregate storage and
reference-returning calls do not hide this provenance and cannot implicitly
extend it.

## Structured Statement Control Flow

The canonical loop forms are:

```cns
while (condition) { body(); }
for (var index: i32 = 0; condition; index = index + 1) { body(); }
for (;;) { break; }
do { body(); } while (condition);
```

`for` evaluates initializer once and then condition, body, and step. All three
clauses may be omitted and an omitted condition is `true`. Initializer and step
each accept at most one expression or assignment clause; initializer also
accepts one `let` or `var` declaration. Assignment remains a statement.
`continue` targets the `for` step, or the condition for `while` and
`do...while`. `break` targets the nearest loop exit. Language 1.3 adds labeled
`break` and `continue` as specified by `v1.3-language-roadmap.md`. Language 1.4
adds the ownership-aware iterator loop specified by
`v1.4-language-roadmap.md`; it uses the same structured cleanup edges.
Language 1.5 resolves the same loop through the standard iterator witness and
adds owned Item transfer as specified by `v1.5-language-roadmap.md`.
Language 1.6 adds only library adapters over that unchanged grammar, as
specified by `v1.6-language-roadmap.md`.
Language 1.7 admits lexical value blocks, callable tail values, and statement
switch as specified by `v1.7-language-roadmap.md`.

Each natural edge, `break`, `continue`, and `return` ends the appropriate
lifetimes and destroys live owned values in reverse lexical order. The
place-state fixed point includes natural and explicit backedges. Public generic
templates persist the same structured regions; imported concrete bodies may
not weaken cleanup or scope behavior.

`defer { statements }` adds its body to the nearest lexical block when the
statement executes. Deferred bodies and local lifetime registrations share one
registration sequence and run in strict LIFO order on normal fallthrough,
`return`, exact-error residual return, `break`, and `continue`. A captured place
is read when cleanup runs. Return expressions are evaluated and transferred to
the function result slot before the cleanup suffix executes.

A defer body must fall through. It cannot contain nested defer, return,
residual propagation, or loop control that targets a loop outside the body.
Conditionals and loops declared inside the body, including their local break
and continue edges, are valid. Traps and coroutine suspension do not run defer
in this version.

## Divergence And Unreachable Code

An expression of type `never` does not fall through. A direct or indirect call
returning `never`, an expression conditional whose arms all diverge, and a
proven constant-true loop with no reachable `break` in that loop terminate the
current control-flow path. The implemented loop cases are `while (true)`,
`for (;;)`, and `do ... while (true)`.

Statements after a non-fallthrough operation receive the
`chtholly.next.sem.unreachable-code` warning. They are still parsed and checked
in a detached semantic block so errors in unreachable source are not hidden,
but they do not enter executable SemIR or LowIR. Chtholly v1 currently exposes
no `panic`, `abort`, or explicit `unreachable` source operation; those remain
owned by the unrecoverable-failure design.

## Builtin Operators

The normative precedence, from tightest to loosest, is prefix unary, postfix
cast, multiplicative, additive, shift, `<=>`, relational, equality, bitwise `&`,
bitwise `^`, bitwise `|`, `&&`, and `||`. Binary operators associate left.
Assignment is a statement and has no value or associativity.

Numeric `+ - * /` use the symmetric lossless common type. `%`, shifts, and
bitwise operations require integers. A shift result has the left type and its
count may have any integer type. Numeric comparisons produce `bool`; equality
also admits `bool`, identical raw-pointer types, and identical C
function-pointer types. Floating `%`, pointer ordering, pointer arithmetic,
reference comparison, string comparison, and nominal comparison are rejected.

`left <=> right` requires an explicit `import std::compare;` and returns the
canonical `std::compare::Ordering`: `Less`, `Equal`, `Greater`, or `Unordered`.
NaN produces `Unordered`. A source-defined enum with the same spelling is not a
substitute for the standard public identity.

Integer addition, subtraction, multiplication, unary minus, and left shift
are checked. Division and remainder reject zero; signed `MIN / -1` and
`MIN % -1` are overflow. Shift counts must be in `[0, left_width)`. Constant
failure is diagnosed; runtime failure uses the stable arithmetic trap reasons
1, 2, 3, 4, and 7. Division truncates toward zero, remainder follows the
dividend sign, signed right shift is arithmetic, and unsigned right shift is
logical. Floating arithmetic and comparison follow IEEE rules; `!=` alone is
true for unordered operands.

Operands evaluate from left to right exactly once. `&&` and `||` short-circuit.
A compound assignment fixes its destination place, reads the old value,
evaluates the right operand, performs a destination-typed checked operation,
and commits once. The right operand must convert losslessly to the destination;
a shift count remains independently integer-typed. Failure leaves the
destination unchanged.

For a language 1.3 nominal or constrained-generic destination, compound
assignment instead resolves the fixed `std::ops` assignment protocol. The
destination place and right expression are still evaluated exactly once; the
right operand is borrowed read-only before the destination is borrowed as
`Self&`. The resulting ordinary direct or interface call must return `void` and
is the mutation boundary. Failure before invocation performs no protocol
mutation, while unrecoverable failure after entry has no rollback guarantee.
Assignment remains a statement and has no value.

## Version Boundaries

The frozen versions do not admit implicit exception unwinding,
provenance-changing pointer conversions, or user-defined control-flow
operators. Public imported enums use the same exhaustive switch and wildcard
rules as local enums.

No control-flow syntax may introduce implicit exception unwinding or bypass
place-state cleanup.
