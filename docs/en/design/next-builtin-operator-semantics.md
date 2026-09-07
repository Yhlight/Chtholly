# Next Builtin Operator Semantics

Status: implemented normative core.

## Boundary

This milestone closes the Chtholly v1 builtin scalar operator set. The parser
classifies source tokens into a closed `BuiltinOperatorKind`; SemIR and LowIR
carry that identity explicitly, and LLVM lowering never repeats source-level
overload or conversion lookup. User-defined operator protocols are specified
separately by the language 1.3 roadmap and do not alter this builtin contract.

The admitted binary set is `+ - * / % << >> & ^ | == != < <= > >= <=>`.
Prefix `+ - ~ !` and every corresponding compound assignment are supported.
`&&` and `||` are structured control flow rather than eager builtin calls.
Assignment remains a statement and is not an expression.

## Value And Type Rules

Arithmetic and ordering use the symmetric lossless common numeric type.
Remainder, shifts, and bitwise operations require integers. Shift counts may
use any integer type; the result retains the left operand type. Equality also
accepts `bool`, identical raw-pointer types, and identical C function-pointer
types. References, strings, native nominal values, and callable values do not
gain builtin comparison.

`<=>` requires an explicit `import std::compare;` and returns the canonical
`std::compare::Ordering` enum with `Less`, `Equal`, `Greater`, and `Unordered`
unit variants. Floating NaN maps to `Unordered`. The compiler validates the
canonical public identity and shape instead of accepting a lookalike enum.

Compound assignment is destination-typed. Its right operand must convert
losslessly to the destination type, except that shift counts retain their own
integer type. The compiler fixes the destination place, reads the old value,
evaluates the right operand, performs the checked operation, and commits once.
A failed operation cannot partially update the destination.

This guarantee applies to builtin scalar compound assignment. A nominal or
constrained-generic destination dispatches a language 1.3 mutable protocol;
that ordinary `Self&` call is its mutation boundary and is not followed by a
compiler-generated assignment.

## Failure And Floating Rules

Integer add, subtract, multiply, negate, and overflowing left shift trap with
arithmetic reason 7 in every build mode. Division by zero uses reason 1,
remainder by zero uses reason 2, signed `MIN / -1` and `MIN % -1` use reason 3,
and a shift count outside `[0, left_width)` uses reason 4. Constant failures are
diagnosed before lowering. Integer division truncates toward zero and the
remainder follows the dividend sign. Signed right shift is arithmetic;
unsigned right shift is logical. Unsigned unary minus is checked `0 - value`.

Floating `+ - * /` and comparisons follow IEEE behavior. Floating remainder is
not admitted. `!=` is true for NaN; equality and relational predicates other
than `!=` are false when unordered. No integer overflow or zero-divisor policy
is inferred for floating operations.

## Sequencing And IR

Operands evaluate left to right and exactly once. `&&` evaluates its right
operand only when the left operand is true; `||` does so only when the left
operand is false. SemIR represents these as typed `If` arms with `Yield`.
Place-state analysis records compound-assignment reads and writes separately;
LowIR uses `LoadPlace` for the old value, making the single-read contract
verifiable before LLVM emission.

## Artifact And ABI Impact

Generic templates persist operator identity and non-owning operand blocks.
Generic specialization remaps those operands to already-cloned instructions;
it never treats an operand list as an owning block or evaluates it twice.
Concrete-specialization components use `CHNXSCC32` version 30; package state
uses `CHNXTPK48` version 48. Older readers fail closed. No value ABI lane was
added. `std::compare::Ordering` uses its ordinary verified nominal enum layout,
and the existing hosted arithmetic trap ABI carries all runtime failures.
