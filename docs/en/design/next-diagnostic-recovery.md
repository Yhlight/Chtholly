# Next Diagnostic Recovery

Status: implemented; pending the separate v1 syntax-freeze decision.

## Recovery Contract

The Next parser has three shared synchronization boundaries: declarations,
statements, and comma-separated lists. A failed parse iteration must either
consume input or stop at EOF. Declaration recovery resumes at an unambiguous
declaration introducer. Statement recovery stops at the current semicolon or
block close. List recovery stops at the list's comma or matching close.

Synchronization skips complete nested parenthesis, bracket, and brace groups.
An unmatched close is left for the owning outer parser. 

## Error Ownership

Missing tokens and skipped constructs create explicit `Error` nodes. Existing
postorder subtree construction propagates `HasError` only through the owning
subtree, so a later valid declaration remains clean. Recovery does not invent
a valid token or attach semantics to an error node.

The compilation pipeline always parses `.cns` token buffers, including buffers
that contain lexical errors, so tooling can inspect the recovered tree. Any
lexical or parse error remains a hard gate before import resolution and SemIR.
No artifact or native output can be produced from recovered source.

## Evidence

Frontend tests cover repeated declaration modifiers, missing list separators,
nested groups during list synchronization, missing function bodies, multiple
diagnostics in one file, clean following declarations, and recovery after a
lexical error. Pipeline tests verify that lexical recovery exposes a parse tree
while semantic analysis remains absent. Existing SemIR and pipeline suites
cover valid-source tree compatibility.
