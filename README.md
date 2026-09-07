# Chtholly

Chtholly is a component-oriented native systems language with affine ownership,
inferred borrowing and provenance, deterministic cleanup, and semantic
interfaces for native component boundaries. The repository contains one active
compiler implementation.

## Start Here

- [0.2.0 Preview Quickstart](docs/en/quickstart.md)
- [Project And Workspace Manifests](docs/en/manifest-reference.md)
- [CFFI Generation And Regeneration](docs/en/cffi.md)
- [Language Specification](docs/en/spec/README.md)
- [Language Charter](docs/en/spec/language-charter.md)
- [Language Surface](docs/en/spec/language-surface.md)
- [Abstract Machine And Memory Model](docs/en/spec/abstract-machine-and-memory-model.md)
- [Component And FFI ABI](docs/en/spec/component-and-ffi-abi.md)
- [Async Channel API Design Decision](docs/en/design/next-async-channel-api.md)
- [Component ABI-2 Resource Protocol](docs/en/design/next-component-abi-2-resource-protocol.md)
- [Component ABI-2 Design-Gate Record](docs/en/design/abi-2-design-gate-record.md)
- [Release Supply-Chain Evidence](docs/en/supply-chain.md)
- [Telemetry Pipeline Example](examples/telemetry-pipeline/README.md)
- [Documentation Index](docs/en/index.md)

## Build And Use

```powershell
.\scripts\build.ps1
.\build-ninja\tools\chthollyc\chthollyc.exe check --project .\examples\hello-preview
.\build-ninja\tools\chthollyc\chthollyc.exe run --project .\examples\hello-preview -- Chtholly
```

For Linux, ASAN, or TSAN validation, use `scripts/wsl-native-abi-test.ps1` to
copy the source tree to WSL's native filesystem before configuring and running
the focused ABI-2 matrix. Linux build state remains in WSL and is copied back
only as evidence.

## Testing

The generated Chtholly test manifest is the only test orchestration entry
point. Build the suite and run it with the dedicated runner:

```powershell
cmake --build .\build-ninja --target chtholly-test-suite
& .\build-ninja\tools\chtholly-test\chtholly-test.exe run `
  --manifest .\build-ninja\tests\chtholly-tests.generated.toml
```

```powershell
python .\scripts\preview-acceptance.py `
  --chthollyc .\build-ninja\tools\chthollyc\chthollyc.exe
```

Use `list`, `describe`, `--filter`, `--label`, `--capability`, `--jobs`, and
`--format json|junit` for focused local runs and CI reports.

Use `--artifact-dir <directory>` to retain per-test stdout/stderr files while
migrating or diagnosing CFFI and native tests. See [Test Framework](docs/testing.md)
for the migration contract.

The Tier-1 CFFI inventory and compiler source/history boundaries are checked
before the native suite:

```powershell
python .\scripts\cffi-tier1-audit.py --source-dir . --check
python .\scripts\source-architecture-audit.py --source-dir . --check
python .\scripts\historical-architecture-audit.py --source-dir . --check
```

The `0.2.0-preview` release target is Windows x64 and Linux x64; macOS arm64
is a best-effort compatibility host. Project creation, checking,
native build/run, local path dependencies, and workspaces are supported;
registry workflows remain experimental.

Only `docs/spec/` defines the language. The frozen surface manifests
`support/chtholly-v1.toml` through `support/chtholly-v1.9.toml` and the Next
test sources record the versioned implementation inventory and evidence.
