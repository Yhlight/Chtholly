# Packaging

## Package Contents

Release builds expose two install components. Select one with
`cmake --install <build> --component <profile>` (the default unqualified
install includes both components):

- `minimal` is the runtime/compiler profile for users and CI smoke jobs. It
  contains the `chthollyc` launcher and driver, Runtime ABI v1, standard
  library sources, frozen support manifests, and licensing/readme files. It
  intentionally omits developer executables, public native headers, docs,
  editor assets, and Runtime ABI v2/container archives.
- `full` is the developer and release profile. In addition to the minimal
  payload it contains `chtholly-test`, LSP, CFFI, ToolchainManager, registry
  tools, native headers, documentation, VS Code source/VSIX (when enabled),
  Runtime ABI v2, and container/component archives.

`CHTHOLLY_INSTALL_PROFILE=minimal|full` is a validated CMake cache value for
packaging configurations and selects the default CPack archive component.
Component selection remains explicit at install time so a release job can
publish and audit both inventories from one build.

A Chtholly package contains:

- `bin/chthollyc` (or `chthollyc.exe` on Windows) and the supported product
  tools
- `README.md`, contribution guidance, and license files
- frozen surface manifests under `share/chtholly/support/`
- Tier-1 CFFI evidence inventory under
  `share/chtholly/support/chtholly-cffi-tier1.toml`
- normative specifications and current implementation status under
  `share/chtholly/docs/`
- standard-library sources under `share/chtholly/stdlib/`
- `chtholly_next_runtime_v1` and its link manifest under
  `share/chtholly/runtime/`
- `next_host_v1.h`, `next_runtime_v1.h`, `next_task_v1.h`, and
  `next_hosted_async_v1.h` under
  `include/chtholly/`
- VS Code extension source and, for release builds configured with
  `CHTHOLLY_BUILD_VSCODE_EXTENSION=ON`, a ready-to-install VSIX under
  `share/chtholly/vscode/`

`docs/internal/` and compiler-development artifacts are not installed.

Published package artifacts may include an optional `interop-bundle` record in
`package.artifact`. The record stores the lower-case SHA-256 digest and a
package-relative `.interop` path. Archive packing includes the sidecar in the
canonical closure index; inspection and extraction verify entry order, size,
digest, and root-manifest identity before exposing the payload.

The runtime link manifest accepts `symbol <source> <runtime>` records in
addition to `system` and `file` records. Symbol mappings are portable names,
unique in both directions, and participate in the link-toolchain fingerprint;
they are never interpreted as filesystem paths.

## Build Flow

```powershell
.\scripts\build.ps1 -CMakeArgs -DCHTHOLLY_BUILD_VSCODE_EXTENSION=ON
cmake --build .\build-ninja --target package
Get-ChildItem .\build-ninja\chtholly-0.2.0-preview-*.zip |
  Get-FileHash -Algorithm SHA256
```

To produce a profile-specific archive, ask CPack for the corresponding
component after configuring the project. Release CI creates the complete
archive and a separate `minimal` archive, then records size evidence against
the matching archive/install-tree pair:

```powershell
cmake --install build-release --prefix install-minimal --component minimal
cpack --config build-release/CPackConfig.cmake -B build-release/minimal-package `
  -D CPACK_COMPONENTS_ALL=minimal
```

CPack selects `windows-x64`, `linux-x64`, or `macos-arm64` from the native
build platform. The signed toolchain archive is a separate lifecycle package
and is validated by `scripts/release-install-upgrade-evidence.py`.

Release packages include deterministic size-budget evidence. These regression budgets are separate from ToolchainManager's 8 GiB safety ceiling.
