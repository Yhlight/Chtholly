# Registry Transparency

Chtholly resolves signed registry artifacts against three independently
verifiable views: the checked-out registry snapshot, the live registry audit
checkpoint, and a quorum of independent witness statements. Dependency
resolution fails closed when those views cannot be placed on one append-only
Merkle history.

## Registry Manifest

A witnessed registry uses an explicit HTTPS origin in addition to its index:

```toml
[registries]
release = {
  index = "https://registry.example/index.git",
  origin = "https://registry.example",
  root_keys = ["ed25519:..."],
  root_threshold = 1,
  ca_bundle = "registry-ca.pem",
  witnesses = ["https://witness-a.example", "https://witness-b.example"],
  witness_keys = ["ed25519:...", "ed25519:..."],
  witness_threshold = 2,
  witness_ca_bundle = "witness-ca.pem"
}
```

`origin` is an origin only: paths, credentials, queries, and fragments are
rejected. Each accepted witness statement must authenticate under the policy's
public-key set, and the quorum must overlap
(`2 * witness_threshold > witness_keys.size()`). A witness policy also requires
root trust and a snapshot v2 audit checkpoint.

## Published View

`chtholly-registry-snapshot-v2` signs the SHA-256 digest of
`trust/audit-checkpoint.txt`. The checkpoint is signed by the root metadata's
audit role. Registry publication and yank/unyank materialization update the
package index, checkpoint, snapshot, and timestamp as one rollback-capable
operation.

An existing registry can publish its latest audit head without creating a
synthetic audit event:

```powershell
chtholly-registryd index reseal --config server.toml
```

Generic `chthollyc registry index seal` remains snapshot v1 when no audit
checkpoint is supplied. This preserves unwitnessed registry compatibility;
witness-enforced resolution does not accept that format.

## Client Enforcement

Online resolution performs these checks before accepting package metadata:

1. Verify the root chain, snapshot, timestamp, package-tree digest, and the
   snapshot-bound audit checkpoint without advancing local rollback state.
2. Fetch the live registry checkpoint and every configured witness head over
   verified HTTPS. A lagging witness receives the snapshot observation.
3. Verify witness identity, registry origin, root identity, signature, and
   quorum with duplicate key IDs removed.
4. Reject equal-size checkpoints with different roots. For different sizes,
   fetch and verify registry consistency proofs between every ordered view.
5. Persist witness statements and transparency state, then commit registry
   rollback state.

The registry CA bundle is also used for signed artifact downloads. Redirects
remain HTTPS-only, credentials in artifact URLs are rejected, and the received
size and SHA-256 must match signed entry facts.

## Lockfile And Offline Rules

`chtholly-lock-v12` records a registry checkpoint:

```text
registry-checkpoint <tree-size> <root-hash> <root-version> <root-sha256>
```

The pin is an accepted consistency-prefix floor, not a frozen latest head.
`--locked` may observe a later snapshot only after proving that the lockfile
checkpoint is its ancestor and confirming that the selected signed artifact
facts are unchanged. The lockfile is not rewritten in this case.

Offline witnessed resolution requires all of the following:

- a v12 checkpoint pin;
- the exact cached snapshot transparency state;
- a locally cached witness quorum whose statements and signatures still
  verify;
- the normal trusted registry and signed artifact caches.

Missing or altered transparency state fails closed. Online mode never uses a
cached quorum as a substitute for unavailable witnesses.

## Local State

Transparency state is stored beside the registry rollback state derived from
the artifact identity store:

```text
transparency-state.txt
witness-statements/<witness-key-id>.txt
```

These files are security state, not disposable download cache. Backup or
migration procedures that expect offline operation must preserve them with the
artifact identity store. A rejected witness view does not advance registry
trust rollback state.

## Compatibility Boundary

Snapshot v1 and lockfile v6 through v11 remain readable for registries without
a witness policy. Enabling witnesses requires snapshot v2 and a v12 lockfile;
old locks are rejected in `--locked` mode and must first be refreshed online.
No language syntax or program ABI changes are introduced by registry
transparency.
