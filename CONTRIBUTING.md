# Contributing to Chtholly

Compiler work must keep the Next parser, SemIR, LowIR, LLVM lowering, native
execution, tooling, and normative v1 documentation aligned.

## Environment

Before building, import the MSVC amd64 Developer Shell environment:

```powershell
Import-Module "$path\visual studio\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"

Enter-VsDevShell -VsInstallPath "$path\visual studio" -Arch amd64 -SkipAutomaticLocation
```

Here, `$path` is a placeholder for the parent directory where Visual Studio is installed.

For example, if Visual Studio is installed under `D:\yhprogram\visual studio`, the module path would be:

```text
D:\yhprogram\visual studio\Common7\Tools\Microsoft.VisualStudio.DevShell.dll
```

Use the project build script as the default entry point:

```powershell
.\scripts\build.ps1
```

## Testing

Run only the Next test surface.

```powershell
& .\build-ninja\tools\chtholly-test\chtholly-test.exe run `
  --manifest .\build-ninja\tests\chtholly-tests.generated.toml
python .\scripts\chtholly-v1-surface.py --source-dir . --check
python .\scripts\compiler-boundary-audit.py --source-dir . --check
```

## Change Rules

- Keep edits scoped.
- The implemented v1 baseline is normative while the expanded v1 scope is a
  candidate. A syntax or ABI change requires an explicit specification decision
  before parser or test changes.
- Update `support/chtholly-v1.toml`, `docs/spec/`, and the focused Next tests
  together when the admitted language surface changes.
- Keep compiler-specific IR behind the Next boundary; shared driver types must
  remain compiler-neutral.
- Package scheduling changes require representative large-workspace evidence.
- Update `README.md` when the compiler entry path changes.
- Do not commit generated artifacts from `build-*`, `artifacts/`, or local compiler output.
