# Publish Checklist

- `chthollyc --help` and `chthollyc --version` report the intended release
- the complete generated `chtholly-test` suite passes
- frozen 1.0 through 1.10 surface manifests pass their executable evidence checks
- the 1.4.1 source-fuzz dispatch, diagnostic determinism, parallel stress, and
  sanitizer gates pass without changing the frozen 1.4 surface
- the Next-only boundary audit passes
- normative docs, Next standard library, and Next runtime resources install
- `share/chtholly/docs/internal` is absent
- the installed runtime directory contains only the Next runtime archive and
  its link manifest
- the install-tree black-box test creates, checks, builds, and runs a project
- installed host evidence passes doctor, path dependency, cold/warm build, run,
  and negative diagnostic checks on required hosts
- signed lifecycle evidence passes package, verify, install, activate, upgrade,
  compiler preflight, rollback, tamper rejection, and inactive removal
- the VSIX packages and starts the shipped `chtholly-lsp` client configuration
- Windows/Linux release parity agrees on source commit, standard-library epochs,
  runtime ABI, and preview smoke results
- the Ubuntu required CI job uploads failure evidence before enforcing its final
  status, so the first Linux run produces actionable diagnostics
- the release archives use the native profile-aware names
  `chtholly-<version>-<platform>-<profile>.zip` (`windows-x64`, `linux-x64`, or
  `macos-arm64`; `minimal` or `full`)
- the stage-boundary evidence audit passes with
  `chtholly-stage-boundary-evidence-v1`; full/minimal size, install
  space/lifecycle, artifact-cache, Component ABI-1 application, and
  performance/diagnostic reports all carry the release source commit and
  target and have `valid = true`
- profile inventories are represented by the full/minimal release-size reports;
  diagnostic counters are retained in the performance-baseline report. No
  parallel profile or diagnostic schema is accepted

## Verification Commands

```powershell
.\scripts\build.ps1 -CleanFirst -SkipTests -ReportBuildArtifacts -BuildArtifactReportPath .\artifacts\build-artifacts.json
& .\build-ninja\tools\chtholly-test\chtholly-test.exe run `
  --manifest .\build-ninja\tests\chtholly-tests.generated.toml `
  --format junit --output .\artifacts\chtholly-tests.xml `
  --artifact-dir .\artifacts\test-output
python .\scripts\chtholly-v1-surface.py --source-dir . --check
python .\scripts\reference-doc-audit.py --source-dir . --check
```

MSVC AddressSanitizer verification uses a separate build tree:

```powershell
.\scripts\build.ps1 -BuildDir build-asan -Sanitizer address `
  -CompilerLauncher none -Target chtholly_source_dispatch_tests -SkipTests
```

- Verify the release size audit evidence and budget report before publishing.
