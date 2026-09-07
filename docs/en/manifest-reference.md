# Project And Workspace Manifest Reference

The 0.2.0 preview formally supports local projects, local path dependencies,
and workspaces. Registry distribution remains experimental and is not part of
the stable project workflow.

## Project Manifest

Every package has a `chtholly.toml` file:

```toml
[package]
name = "application"
language = "1.3"

[build]
entry = "src/main.cns"
module_paths = ["src"]

[dependencies]
utility = { path = "../utility", default_features = true }

[features]
default = []
extras = ["utility/extra"]
```

`package.name` and `package.language` are required. `build.entry` is required
for an executable root and omitted by dependency libraries; `module_paths`
defaults to `["src"]`. The optional `[target]` table accepts `triple`,
`sysroot`, and `linker`. The optional `[native]` table accepts
`library_paths` and `link_libraries`.

Next packages may opt into the CFFI publication gate:

```toml
[cffi]
receipt = "bindings.cffi-verify"
required = true
```

When enabled, `chthollyc` requires a canonical `CHCFFI3` receipt before
compiling, checks its target against the resolved target for the complete
package graph, and records the canonical receipt identity SHA-256 in
`chtholly.lock`. The receipt is part of the build-control snapshot and Next
artifact configuration. The compiler does not load libclang;
`chtholly-cffi verify` remains responsible for producing the receipt.
When a Package Artifact is published, v20 emits the complete receipt identity
and the receipt closure file as its `cffi-receipt` record.

Binding generation and header-upgrade regeneration are documented in
`cffi.md`. The sibling `CHCFFIS5` file is local maintenance state and is not
referenced from `chtholly.toml`, the lockfile, or a Package Artifact.

A dynamically loaded component omits `build.entry` and adds:

```toml
[component]
abi = 1
identity = "org.example.math"
exports = ["math::add", "math::checksum"]
```

All three keys are required. Export names use unambiguous `module::function`
identity and select synchronous, safe, non-generic public free definitions.
`build` produces a platform shared library and adjacent `.chcomponent`
contract; `check` validates the contract, while `run` rejects component
packages.

A dependency is an inline table. Formally supported sources are
`path = "..."` and `workspace = true`. Optional keys are `features`,
`optional`, and `default_features`. An immutable local artifact may accompany a
path dependency through `artifact` and a required 64-character lowercase
`sha256`; this is an advanced reproducibility boundary, not a registry source.

## Workspace Manifest

A workspace root has `chtholly.workspace.toml`:

```toml
[workspace]
members = ["app", "utility"]
default_members = ["app"]
```

`members` is required. A workspace command without `--package` requires
exactly one default member. A member refers to another member with
`dependency_name = { workspace = true }`.

## Commands And Locking

`check`, `build`, and `run` discover the nearest project or workspace from the
current directory, or accept `--project` / `--workspace`. Use `--package` to
select a workspace member. Builds write `chtholly.lock`; `--locked` rejects a
change, while `--no-lockfile` disables lockfile reads and writes.
