# Chtholly Toolchain Management

Before publishing or activating a generation, verify the preview's product
baseline in addition to the toolchain signature:

```powershell
python scripts/product-status-audit.py --source-dir . --check
```

This checks the product version, Windows/Linux x64 preview targets, supported source
versions, semantic/package/standard-library epochs, and evidence paths for
preview versus experimental capabilities.

`chtholly-toolchain` installs signed, immutable compiler generations and keeps
activation history for rollback. A manager root contains the trust root,
installed generations, and active-generation state; it must not be shared by
untrusted users.

Component deployment generations are separate from compiler toolchain
generations. They retain versioned libraries/contracts side by side and use an
atomic active marker plus history for rollback; an active component generation
cannot be removed.

## Publisher Setup

Generate an Ed25519 signing key and create the initial trust root:

```powershell
chtholly-toolchain key generate `
  --secret release.secret --public release.public
chtholly-toolchain trust create -o root.trust `
  --version 1 --threshold 1 `
  --key release.public --secret-key release.secret
```

Keep the secret key offline. Distribute `root.trust` independently from release
archives so users can establish trust before installing a generation.

Package an install tree:

```powershell
chtholly-toolchain package .\install-tree -o .\chtholly.zip `
  --version 0.2.0 --source-commit <full-40-hex-commit> `
  --secret-key release.secret
```

## Install And Upgrade

Initialize a manager once, then verify or install a signed archive:

```powershell
chtholly-toolchain trust init .\root.trust --root .\toolchains
chtholly-toolchain verify .\chtholly.zip --root .\toolchains
chtholly-toolchain install .\chtholly.zip --root .\toolchains
chtholly-toolchain activate <release-id> --root .\toolchains
```

`upgrade` installs the generation, runs its `chthollyc --version` preflight,
and activates it atomically. The prior active generation enters rollback
history:

```powershell
chtholly-toolchain upgrade .\chtholly-next.zip --root .\toolchains
chtholly-toolchain list --root .\toolchains
chtholly-toolchain rollback --root .\toolchains
```

`remove <release-id>` rejects the active generation. Trust-root updates require
a strictly newer root signed according to the currently installed threshold.

### Install-space preflight

After the archive index and every payload digest have been verified, the
installer measures free space on the parent of the manager root before it
creates a staging generation or extracts a payload. The required amount is the
sum of the verified uncompressed payload bytes and the signed index bytes. A
successful `install` or `upgrade` records these evidence fields in its normal
output (and in `--output-format jsonl-v1` records):

```text
space-payload-bytes
space-index-bytes
space-required-bytes
space-available-bytes
space-path
space-sufficient
```

When the filesystem reports an error or has less than the required amount, the
operation fails closed with the stable `insufficient-space` reason and includes
`space-required-bytes`, `space-available-bytes`, and `space-path` in both the
human diagnostic and JSONL diagnostic. No generation directory or payload file
is created in that case. The check is advisory with respect to the normal
filesystem race: another process may consume space after the measurement, so
extraction still handles ordinary write failures and cleans up its staging
tree.

## Release evidence

The release contract validates the installed tree itself on Windows x64 and
Ubuntu x64. macOS remains a non-blocking compatibility host for this preview.
The host evidence command performs `doctor`, clean application/library
scaffolding, a local path dependency, cold/warm build, native run, and a
negative project diagnostic:

```powershell
python scripts/release-host-evidence.py `
  --host windows-2022 `
  --install-prefix .\install-release `
  --output .\evidence\windows-2022.json
```

Signed lifecycle evidence packages the complete install tree, verifies and
installs it, upgrades to a second generation, rolls back, rejects a tampered
archive, and removes the inactive generation. CPack ZIP names include the
native platform (`windows-x64`, `linux-x64`, or `macos-arm64`).
