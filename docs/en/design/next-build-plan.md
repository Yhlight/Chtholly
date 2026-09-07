# Next Build Plan

Status: implemented.

## Ownership

`NextBuildPlan` is the sole project-resolution input to the Next compiler
pipeline. It owns package identities, manifest and entry paths, module roots,
resolved features, direct dependency indices, native link inputs, selected
target facts, output identity roots, lockfile location, and compiler resources.
It does not expose compiler-internal AST, semantic, or package-graph types.

Interop declarations are also part of the resolved package input. A project
may declare a package-local bundle with:

```toml
[build]
interop_bundle = "artifacts/ffi.interop"
```

The path is resolved beneath the package root and hashed with SHA-256 during
plan construction. The path and digest participate in the package identity,
resolution fingerprint, control-input snapshot, and session identity. The
driver loads the bundle before adding source units, and each package session
owns an isolated `ArtifactRegistry`. Direct dependencies and their template
dependency closure are loaded explicitly; consumers resolve every
`ArtifactReference` through that registry.

Dependencies may additionally pin an immutable package archive:

```toml
[dependencies.provider]
path = "../provider"
artifact = "artifacts/provider.zip"
sha256 = "<64 lowercase hexadecimal characters>"
```

The driver installs the archive into `ArtifactStore`, consumes its verified
closure, and reads the root manifest and Interop sidecar from that closure.
ZIP files are never read directly by a compilation session. Archive-only
precompiled Next semantic inputs remain rejected; source/workspace dependencies
continue to provide the compilation units.

When artifact-load metrics are enabled, the structured JSON includes an
`archive-install` object. Its `attempts`, `fresh-installs`, and `closure-hits`
fields describe closure-level reuse; `archive-bytes` records inspected archive
input size. Specialization closure and component-cache counters remain separate
so package archive reuse is not confused with generic artifact reuse.

Persistent generic reuse is measured separately: a later build reports
specialization closure `found` requests and corresponding artifact reads. This
is distinct from executor-level in-process request coalescing.

The resolver accepts direct `.cns` input, one project manifest, or one selected
workspace member. Manifest parsing is deliberately narrow. v1 admits local
path dependencies and `workspace = true` dependencies. Git, registry, and
precompiled artifact sources fail closed instead of entering a compatibility
path.

## Resolution

Package discovery and semantic resolution are separate states. Discovery
assigns one normalized manifest to each package identity. Reusing an identity
from another manifest is an error. Resolution merges default and explicit
feature requests from every incoming edge, including package-qualified CLI
requests, then repeatedly expands feature requirements and optional
dependencies until no package request changes. The finalized graph must be
acyclic and every package-qualified option must name a reachable package.

## Lockfile And Scheduling

`chtholly.lock` is Next-owned and deterministic. It records the selected root,
each package's normalized manifest-path digest, resolved features, and direct
dependency edges in stable name order. Manifest contents remain build-control
inputs, so local edits trigger request-barrier and incremental invalidation
without pretending a local path dependency is a versioned remote release.
`--locked` requires the computed graph identity to match the existing file.

The build plan contains dependency indices, not an execution order.
`NextPackageQueryGraph` remains responsible for deterministic ready-package
scheduling and `--jobs` execution. Package scheduling will not be redesigned
without real large-workspace bottleneck data.
