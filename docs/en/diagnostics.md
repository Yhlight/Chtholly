# Chtholly Preview Diagnostics

Diagnostics are stable compiler interface data, not incidental text. The
canonical diagnostic names and codes are declared in
[`include/chtholly/Compiler/DiagnosticKind.def`](../include/chtholly/Compiler/DiagnosticKind.def)
and are used by the CLI, JSONL output, LSP, VS Code problem matcher, and test
fixtures.

## Output forms

Human diagnostics use:

```text
file.cns:line:column: error: message [chtholly.next.sem.unknown-name]
source text
          ^~~~~
```

`check`, `build`, `run`, and `doctor` support `--output-format jsonl`. JSONL
records preserve command output and finish with a versioned `command-result`
record. LSP diagnostics use the same code and severity with zero-based UTF-16
ranges. A later successful check clears stale diagnostics for the document.

Ownership diagnostics retain a structured explanation chain. The primary
diagnostic keeps its existing code and range; related notes identify the
borrow/move origin, conflicting access, callable effect, returned provenance,
cleanup boundary, contract boundary, or conservative analysis widening. Notes
have stable `chtholly.next.note.ownership.*` codes and are rendered after the
primary diagnostic. `jsonl-v1` remains unchanged; JSONL v2 publishes notes as
structured related records, and LSP publishes them through
`relatedInformation`.

The LSP exposes deterministic quick fixes for diagnostics whose correction is
unambiguous: missing `std::ops`, `std::compare`, `std::convert`, `std::result`,
or `std::io` imports, and the explicit by-value receiver move diagnostic. The
import fixes insert after the module declaration; the receiver fix inserts
`move ` at the reported receiver span. Other ownership diagnostics remain
explain-only because choosing `move`, `copy`, a longer borrow, or a different
cleanup path would be a semantic decision the compiler cannot safely infer.

## Diagnostic families

| Family | Examples | Typical repair |
| --- | --- | --- |
| Lexical | `chtholly.next.lex.invalid-utf8`, `invalid-escape` | Fix source encoding or literal spelling. |
| Parse/recovery | `chtholly.next.parse.expected-token`, `invalid-tree` | Add the missing delimiter/declaration; recovered error nodes have no semantics. |
| Module/import | `chtholly.next.import.unknown-module`, `cycle` | Correct the module path or dependency closure. |
| Semantic/type | `chtholly.next.sem.unknown-name`, `unknown-type`, `type-mismatch` | Fix lookup, imports, or the declared type. |
| Ownership/place | `chtholly.next.sem.place.use-after-move`, `borrow.region-conflict`, `uninitialized-storage` | Make transfer, borrow duration, and initialization explicit. |
| Lifecycle/representation | `chtholly.next.lifecycle.invalid-impl`, `representation.incomplete-init` | Complete the canonical lifecycle or representation contract. |
| Callable/generic | `sem.call.ambiguous`, `sem.ownership.contract-mismatch`, `generic.instantiation-limit` | Disambiguate arguments or repair the public contract. |
| Artifact/package | `artifact.*`, `package.*`, `incremental.*` | Rebuild stale/corrupt artifacts or repair the lock/target closure. |
| Compatibility routing | `chtholly.artifact.compatibility`, `chtholly.package.compatibility`, `chtholly.abi.mismatch` | Compare the reported expected/actual epoch, format, fingerprint, target, and ABI facts; rebuild or select the matching target. |
| CFDL/CFFI | `chtholly.next.cfdl.*`, `chtholly.cffi.*` | Correct the foreign contract, toolchain, receipt, or native library closure. |
| Toolchain/runtime | `chtholly.doctor.*`, `chtholly.runtime.*` | Install the required SDK/runtime/linker or fix the target contract. |

## Recovery and failure boundaries

The parser guarantees forward progress and keeps each invalid subtree contained
at the smallest recovery boundary. Semantic analysis never assigns meaning to
an error node. Any lexical or parse error prevents semantic analysis, artifact
publication, and code generation.

Ownership, cleanup, and `Result` residual diagnostics refer to the same
abstract-machine rules used by local, generic, imported, and lowered code.
Artifact, CFFI, package, and toolchain failures are fail-closed: no partial
artifact or activation may be published.

Explanation notes resolve to the local SemIR source locations that produced
the evidence whenever the corresponding source unit is active. When a related
path is present in the active compilation session, the driver resolves its
offset against that unit's source buffer and marks the location as available.
Artifact-only or unavailable provider evidence remains attached to the
canonical path with an explicit unavailable-location marker;
CLI renders `?:?` and LSP uses a zero-length range plus
an explicit `(source unavailable)` message marker rather than fabricating a
consumer span. The marker stays within the standard LSP related-information
shape (`location` and `message` only).
Explanation notes are transient diagnostics only; they are not serialized into
semantic artifacts, package artifacts, Interop bundles, or Component ABI
records.

The diagnostic catalog and documentation families are checked by:

The catalog currently includes the following additional families:
`chtholly.next.module.`, `chtholly.next.nominal.`,
`chtholly.next.representation.`, `chtholly.next.source.`,
`chtholly.next.std.`, and `chtholly.next.version.`. They cover declaration
placement, nominal completion, representation carriers, source limits,
standard-library closure, and language-version gating respectively.

```powershell
python scripts/reference-doc-audit.py --source-dir . --check
```
