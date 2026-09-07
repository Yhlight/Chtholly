# Chtholly Language Versioning

Status: normative for Chtholly v1.

## Source Version Selection

Every package backed by `chtholly.toml` must select its source semantics with a
string in the package table:

```toml
[package]
name = "example"
language = "1.0"
```

The value has the canonical form `MAJOR.MINOR`, with unsigned decimal
components and no suffix. A workspace has no inherited language version; every
member package selects its own. All source modules in a package use that
selection. A module cannot override it.

Direct single-file compilation and compiler-created packages that have no
project manifest use the compiler's current stable default. The stable default
remains `1.0`; the compiler also admits the frozen `1.1` through `1.10`
package versions. There is no source pragma or command-line override: a package
manifest is the authority for reproducible project builds.

There is currently no 1.11 candidate. Cancellation-aware channel operations
are not admitted through a hidden version, parser token, or standard-library
shortcut. A future version may be proposed only after the operation-capability
and Component ABI-2 resource protocol have complete ownership, cleanup,
artifact, diagnostics, and native evidence.

Missing, malformed, or unsupported project versions are diagnosed while the
build plan is formed, before source lexing or dependency compilation begins.

## Independent Compatibility Axes

The following values are independent and cannot substitute for one another:

- the source-language version selects parsing and source semantics;
- the semantic artifact epoch selects the meaning of persisted checked facts;
- component ABI versions select component schemas, layouts, and capabilities;
- runtime ABI versions select runtime symbols and behavioral contracts; and
- the standard-library epoch selects compiler-known library identities and
  protocols.

Package check artifacts and package build artifacts record the source-language
version, semantic artifact epoch, and standard-library epoch. All three values
participate in their verification and compilation-configuration fingerprint.
Serialized package artifacts additionally have an encoding format version;
changing the encoding alone does not create a source-language version.
The current internal package state is `CHNXTPK78` format 76. A package may
carry an optional canonical CFFI receipt identity; it participates in the
compilation configuration and dependency manifest fingerprint. Published
Package Artifact v21 carries the same `CHCFFI3` identity, receipt closure file,
and optional `interop-bundle` record. Changing these encoding formats alone
does not change the source-language or semantic artifact epoch.

Component and runtime ABI versions remain in their owning component,
descriptor, runtime, and support manifests. They are not folded into the
source version or semantic epoch. The standard-library provenance contract
fingerprint remains a stronger identity within a standard-library epoch.

## Dependencies And Artifacts

Each package in a dependency graph is parsed according to its own selected
source version. Source versions do not need to be numerically equal once a
compiler supports more than one version. A dependency is admitted only when:

1. the compiler supports the dependency's source version;
2. its semantic artifact epoch is accepted by the consumer compiler;
3. its standard-library epoch is compatible with the build graph; and
4. every component and runtime boundary passes its existing ABI checks.

The compiler supports frozen source versions `1.0` through `1.10`, semantic
artifact epoch 27, and standard-library epoch 13. Dependency source versions
may differ, but semantic and standard-library epochs require exact agreement.
An artifact with a missing, unknown, or stale compatibility value fails closed
and is never reinterpreted as current.

Changing a package's selected source version invalidates its semantic and
object reuse configuration even when its source bytes are unchanged. Imported
public entities keep their canonical producer identity; the package-level
compatibility record controls whether those entities may be consumed.

## Evolution Policy

A source version can be admitted as a candidate only after its normative
specification, compiler implementation, diagnostics, artifact representation,
migration story, and executable evidence are registered in the language
surface manifest. Candidate behavior may be corrected until release freeze,
but candidate status must be visible and must not masquerade as a different
frozen version.

After freeze, incompatible syntax or semantic changes select a new source
version. They do not rewrite an old version in place. Compatible compiler bug
fixes restore the frozen specification and do not select a new source version.
Artifact-only format changes select a new artifact format or semantic epoch as
appropriate, not a new source version.

Language evolution is migration-first rather than permanent multi-dialect
compatibility. A replacement version is added beside the version it replaces;
automated migration rewrites source and manifests, after which support for the
old version may be retired under an announced toolchain policy. Chtholly v1
does not expose conditional source syntax, feature probes, or compatibility
aliases for selecting version-dependent semantics.
