# Release Supply-Chain Evidence

Chtholly release artifacts carry a versioned supply-chain record. The record
binds the package bytes, source commit, host/lifecycle evidence, SBOM, and
compiler inputs into one verifiable chain.

## Inputs

`support/supply-chain-lock.json` pins GitHub Actions to commit SHAs, the vcpkg
baseline, and the LLVM release source commit. The release workflow also records
the resolved LLVM installation/archive digest, vcpkg manifest digests, and the
versions of Python, CMake, and Ninja. A mutable action tag or a vcpkg checkout
that does not match the lock is rejected before the release build proceeds.

## Evidence files

The release workflow emits the following JSON records for each host:

- `*-inputs.json` — resolved build inputs and their digests;
- `*-sbom.json` — SPDX 2.3 package inventory;
- `*.json` — host smoke/doctor and installed-tree evidence;
- `*-install.json` — signed install, upgrade, rollback, and tamper evidence;
- `*-provenance.json` — package, SBOM, input, and evidence digests bound to one
  source commit.

`release-matrix-evidence.py` verifies that required hosts share the same source
commit, release version, standard-library identity, and runtime ABI. Package
digests may differ by target platform; each host's package digest must match its
own host and provenance records. Missing or mixed provenance fails closed.
Host evidence also records and verifies the resolved target triple, 64-bit
little-endian layout, Component ABI epoch, and runtime ABI epoch. Linux is a
required parity host; macOS evidence remains best-effort until its release job
is promoted to required.

## Execution tiers

Windows/MSVC remains the fast development path. The complete Ubuntu/macOS host
matrix and lifecycle evidence run on release tags, nightly/manual workflows,
or an explicit workflow dispatch. This is intentional: a WSL run can take
about seven minutes and a full GitHub Actions run about 20–40 minutes. These
checks are release confidence gates, not ordinary language-development
feedback loops.

Run the fast local contract with:

```powershell
python scripts/supply-chain-evidence.py inputs --source-dir . --output evidence/inputs.json
python tests/supply_chain_evidence_tests.py `
  --script scripts/supply-chain-evidence.py --source-dir .
```

The supply-chain evidence format does not alter Chtholly syntax, Component ABI,
runtime ABI, artifact bytes, or standard-library epochs.
