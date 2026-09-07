# Chtholly 0.2.0 Preview Quickstart

The supported developer-preview hosts are Windows x64 and Linux x64. macOS
arm64 is a best-effort compatibility host. Install the release ZIP,
add its `bin` directory to `PATH`, and verify the toolchain:

```powershell
chthollyc --version
chthollyc doctor
chthollyc new hello-chtholly
Set-Location hello-chtholly
chthollyc check
chthollyc run
```

## Install profiles

The `minimal` profile is intended for running and checking Chtholly projects.
It contains the compiler launcher/driver, standard-library sources, support
manifests, and Runtime ABI v1. It is sufficient for `chthollyc doctor` and the
hello-project workflow, while omitting developer-only tools and headers.

The `full` profile is intended for development and release engineering. It
adds `chtholly-test`, LSP, CFFI, ToolchainManager, registry tools, native
headers, documentation, VS Code assets, Runtime ABI v2, and container
archives. Select a profile when installing a CMake build:

```powershell
cmake --install build-release --prefix install-minimal --component minimal
cmake --install build-release --prefix install-full --component full
```

The default unqualified CMake install remains the complete (`full`) tree for
backward compatibility. The profile cache variable is validated during
configuration with `-DCHTHOLLY_INSTALL_PROFILE=minimal` or `full` and selects
the default CPack archive component.

For a release-tree-style smoke check, run the same workflow used by the
preview acceptance gate from the source checkout:

```powershell
python scripts/preview-acceptance.py --chthollyc .\build-ninja\tools\chthollyc\chthollyc.exe
```

The gate uses a clean temporary directory and verifies `doctor`, application
and library scaffolding, a local path dependency, cold and warm native builds,
execution, and a stable missing-project diagnostic.

`new` creates an application by default. Use `chthollyc new my-library --lib`
for a library, or run `chthollyc init` in an empty directory. `check` validates
the package graph and semantic program without generating or linking native
objects. `build` creates native outputs, and `run -- <arguments>` builds and
runs an application with UTF-8 process arguments.

`doctor` verifies the installed compiler resources, standard-library format,
runtime archive, host target, native linker, Tier-1 C compiler/SDK,
`chtholly-cffi`, libclang, and a native C smoke probe. Use `--resource-dir`,
`--linker`, or `--cffi-config` to diagnose a custom toolchain layout before
building a project.

Use `--explain-invalidation` with `check` or `build` when an incremental build
does more work than expected. It reports the affected module, cache result, and
the semantic reason for each invalidation; `--output-format jsonl` exposes the
same information as structured records for editor and CI integration.

Use `--dump-analysis-metrics <path>` to write deterministic ownership and
PlaceState work counts plus diagnostic phase timings. The JSON schema is
`chtholly-compiler-analysis-metrics-v1`; timings are reports rather than build
correctness inputs.

`-gline-tables-only` emits per-instruction source line tables for LLVM and
native output, enabling source breakpoints and symbolic stack traces. `-g`
also emits scalar/pointer local-variable declarations and debugger types. On
Windows, executable linking writes a PDB beside the requested output.

Native output supports LLVM optimization levels `-O0`, `-O1`, `-O2`, `-O3`,
`-Os`, and `-Oz`. Optimization is applied after semantic and ownership
verification; it cannot change observable evaluation order, cleanup, or ABI
behavior. For example:

```powershell
chthollyc build --project . -O2
```

## Standard Library

Imports are explicit; there is no implicit prelude. The preview includes:

- `std::option::Option<T>` and `std::result::Result<T, E>`;
- `std::env` for immutable UTF-8 process arguments;
- `std::fs` for safe UTF-8 path existence checks, complete-file writes, file
  removal, and move-only `File` streams with Result-based open/read/close;
- `std::io` for safe synchronous stdout/stderr writes;
- `std::text` helpers for the builtin `string` value, including the
  provenance-preserving `as_bytes` read-only view;
- `std::vec` ownership and borrowing contracts;
- `std::collections` shared views and collector construction;
- `std::iter::mutable` stateful callable adapters and algorithms;
- synchronous `std::net`, `std::sync`, and `std::log` Result-based wrappers;
- experimental `std::typed_channel::Channel<T>` with compiler-owned lifecycle;
- numeric conversion, comparison, time, atomic, volatile, and low-level
  `std::host` modules.

`std::host` and `std::volatile` are unsafe low-level boundaries. `std::vec`
uses runtime-backed aligned dynamic storage with checked growth,
ownership-aware relocation, and deterministic element destruction.

## VS Code

Install the shipped VSIX, then point `chtholly.server.path` at
`chtholly-lsp.exe` if the tool is not on `PATH`:

```powershell
code --install-extension .\chtholly-vscode-0.2.0-preview.vsix
```

The extension registers `.cns` and `.cfdl`, provides syntax highlighting, and
connects completion, hover, definition, references, and diagnostics to the
language server. Document symbols and semantic rename work across source
modules in the current workspace. The command palette exposes **Chtholly: Check Project**,
**Build Project**, **Run Project**, and **Diagnose Toolchain**. Compiler
diagnostics from these tasks are added to the Problems panel through the
shipped Chtholly problem matcher. Configure `chtholly.compiler.path` when
`chthollyc` is not on `PATH`.
