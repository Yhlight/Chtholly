# Compiler Architecture V15: Tier-1 CFFI Toolchain Discovery

Status: implemented by the 1.9.11 CFFI toolchain wave.

## Shared Toolchain Contract

`CFFIToolchainResolver` is the single owner of the effective native C
environment. It publishes compiler family/path/version, target, sysroot or
resource directory, SDK identity, ordered system include/library paths,
subprocess environment overrides, discovery trace, and canonical fingerprints.
libclang parsing, native probes, receipts, regeneration state, and both doctor
commands consume that contract instead of reconstructing command lines.

Windows discovery checks explicit config, a complete inherited environment,
`ChthollyMSVCPath`, `VSINSTALLDIR`, and standard vswhere discovery in that
order. vcvars output is captured through an isolated command wrapper and passed
only to child processes. Linux checks explicit config, `CC`, Clang, GCC, and
`cc`, then queries sysroot, resource directory, include search, and library
search paths from the selected compiler.

## Trust And Doctor

CFFI config v2 moves compiler selection to `[toolchain]`; probe configuration
contains only compile/link behavior. `CHCFFI2` records compiler family plus
effective toolchain and SDK digests. `CHCFFIS2` carries the same identities for
regeneration.

`chthollyc doctor` requires the C compiler/SDK contract, installed
`chtholly-cffi`, libclang runtime, and a native C smoke probe before reporting
ready. It never links libclang. `chtholly-cffi doctor` additionally loads
libclang and, when given a config, parses and completes its configured roots.

## Artifact Closure

Package Artifact v15 and `CHNXTPK72` state 69 carry `CHCFFI2`; cache namespace
is `next-v41`. CFFI config v1, `CHCFFI1`, `CHCFFIS1`, Package Artifact v14,
`CHNXTPK71`, and `next-v40` fail closed. CFDL epoch 9, semantic artifact epoch
17, standard-library epoch 9, Component ABI epoch 1, and runtime ABI v1 are
unchanged.

The real upgrade gate uses unmodified SQLite 3.40.1 and 3.53.4 headers. It
preserves open/close resource facts, adds `sqlite3_is_interrupted`, verifies
against the current native library, and executes an independent Chtholly
consumer without network access.
