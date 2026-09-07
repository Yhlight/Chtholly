# Next Structured Statement Control Flow

Status: implemented normative compiler design.

## Semantic Shape

Parser nodes keep `ForInit`, `ForCondition`, and `ForStep` present even when a
clause is empty. SemIR represents `if` arms and `for` clauses as non-executable
records that own separate executable blocks. `while`, `for`, and `do...while`
remain structured operations; `break` and `continue` are explicit terminators.
This avoids encoding clause roles in child counts and gives generic artifacts,
verifiers, and lowering one canonical region graph.

## State And Cleanup Edges

Loop analysis maintains explicit break and continue targets plus cleanup
depths. Natural body fallthrough and `continue` contribute to the header fixed
point. `break` contributes to the exit state. Each explicit edge stores a
place-state cleanup plan, so LowerToLowIR only emits the chosen reverse cleanup
and branch. A `for` initializer has loop lifetime: natural condition failure
uses an exit cleanup block, while `break` performs the same cleanup on its edge
and bypasses that block.

Generic template opcodes append structured arm, clause, loop, and terminator
records without renumbering older opcodes. Package and specialization format
versions reject older cached representations before materialization. No native
calling convention or public symbol identity changes.