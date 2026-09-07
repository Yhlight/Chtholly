# CFFI Binding Generation And Regeneration

`chtholly-cffi` generates mechanical CFDL declarations from explicit Clang
roots. Ownership, invalid sentinels, cleanup, callback lifetime, and resource
protocols remain human-authored CFDL facts.

The Tier-1 closure inventory is declared in
`support/chtholly-cffi-tier1.toml` and checked by
`scripts/cffi-tier1-audit.py`. It binds the real SQLite, zlib, libcurl, Linux
POSIX, and Windows SDK cases to their provider headers, negative/positive
markers, and required generation/regeneration/verification/consumer evidence.
The native suite writes `chtholly-cffi-tier1-evidence-v1` JSON after every
declared phase succeeds, so closure is not inferred only from a process exit
code.

## Traditional CFFI (Raw ABI)

The shortest binding path is a Raw CFFI declaration. `generate` emits this
form for ordinary Clang roots unless a binding author adds semantic overlays:

```cfdl
module native_api;

foreign fn version() -> c_int link "library_version" call c;
foreign fn reset() -> void;
```

Raw declarations retain the native return value and physical argument lanes.
They do not create ownership or cleanup obligations, borrow/escape facts,
`Result` projections, or buffer-prefix projections. This is intentional and
matches conventional CFFI. The compiler still verifies target-aware types,
calling convention, symbol identity, native probe results, and the resolved
link-library closure.

To make a binding resource-aware, add the semantic overlay explicitly:

```cfdl
foreign fn open() -> owned Session
where result obliges close;
```

`regenerate` preserves these overlays. It never infers them from a function
name, a pointer type, or a header comment. A raw opaque handle is consequently
not automatically released; the binding remains raw until its author declares
the resource protocol.

## Config Version 3

```toml
version = 3
module = "binding"
target = "x86_64-pc-windows-msvc"
headers = ["api.h"]

[toolchain]
compiler = "auto"

[clang]
language = "c"
standard = "c17"
include_paths = ["include"]
system_include_paths = []
defines = []
undefines = []
arguments = []

[probe]
compile_arguments = []
link_arguments = []
library_paths = []
libraries = ["api.lib"]
timeout_ms = 30000
```

`toolchain.compiler` defaults to `auto` and may name an explicit executable.
Windows additionally accepts `msvc_install`; Linux accepts `sysroot`. System
compiler/SDK paths are discovered and precede user additions. Config v1/v2 and
`probe.compiler` are unsupported.

Roots are explicit. `kind = "type"` and `kind = "function"` select C
declarations; `kind = "constant"` selects one active object-like macro. Macro
roots must evaluate through Clang to a target C integer or boolean constant.
Empty, function-like, floating, string, address, type, and non-constant macros
fail closed rather than entering CFDL as token text.

## Initial Generation

```powershell
chtholly-cffi generate --config chtholly-cffi.toml -o binding.cfdl
```

Generation writes `binding.cfdl` and the sibling `binding.cffi-state`. Use
`--state <path>` to select another state path. Existing CFDL or state files are
never overwritten by `generate`.

`CHCFFIS5` stores the previous canonical mechanical draft and its target,
module, toolchain, SDK, config, payload, and content identities. It is
maintenance state, not a compiler artifact, receipt, package input, or ABI
contract.

## Regeneration

```powershell
chtholly-cffi regenerate --config chtholly-cffi.toml binding.cfdl
chtholly-cffi regenerate --config chtholly-cffi.toml binding.cfdl --write
```

The first command is read-only. It returns 0 when no update is required and 3
when it reports an applicable diff. `--write` rechecks the observed input and
atomically replaces the CFDL followed by its state.

Regeneration performs a three-way merge between the stored mechanical draft,
the current human binding, and the new Clang model. It preserves flow
qualifiers, resource `where` facts, error/outcome clauses, invalid sentinels, imports,
manual declarations, and text outside managed declarations. Compatible parameter renames update fact
references. Mechanical edits, ambiguous renames, incompatible resource lanes,
or removal of declarations with live semantic overlays fail closed.

For `win32_read`, a compatible parameter rename updates buffer, capacity,
count, and context references together. Count/context remain human-authored
hidden-lane semantics; regeneration does not infer them from a function name.

An old binding without state may bootstrap only when its complete mechanical
projection matches the current configured headers. A stale binding is never
adopted by guessing.

Human and `jsonl-v1` output report stable `add`, `remove`,
`mechanical-update`, `parameter-rename`, `semantic-preserved`,
`manual-retained`, `state-bootstrap`, and `state-update` events.

After regeneration, run `verify` again to create a receipt for the new header
and CFDL identities:

```powershell
chtholly-cffi verify --config chtholly-cffi.toml binding.cfdl `
  --receipt binding.cffi-verify
```

Error contracts are human-authored overlays:

```cfdl
error code when result != 0
error win32 when result == 0
error errno when result == -1
error errno when result == null
error win32 when result == invalid
error code when result in { RETRY, 100 through 109 }
error code when result not in { OK, DONE }
```

The physical C function remains unchanged. Return-code contracts expose
`Result<void, Code>`. Errno and Win32 contracts preserve the successful raw
result and expose `i32` or `u32` errors respectively. Win32 contracts are
rejected on non-Windows targets; null is restricted to pointer errno results.
All three forms reject `out` and `inout` lanes. Regeneration preserves them as
human semantic overlays.

`invalid` refers to the result foreign type's `invalid` declaration. A
non-zero integer declaration on a pointer carrier is checked as a target-width
bit pattern, so the platform sentinel is stated once. Set members may use local
integer foreign constants; `through` is inclusive. Artifact normalization
removes ordering, spelling, overlap, and adjacency differences before lowering.

POSIX reads are explicit human overlays:

```cfdl
foreign fn read(buffer: view_mut void*, capacity: c_size) -> c_ptrdiff
link "read"
call c
outcome posix_read<u8>(buffer, capacity)
error errno when result == -1;
```

They expose `Result<ReadOutcome<slice<u8>>, i32>` in language 1.10. The Data
slice is the initialized buffer prefix and remains a non-owning view. EOF is
distinct from a zero-capacity empty read. Regeneration preserves the outcome
and updates its parameter references across compatible mechanical renames.

## Doctor

`chtholly-cffi doctor` checks automatic compiler/SDK discovery, libclang
loading, standard C headers, and a native smoke probe. `--config` additionally
parses and completes the configured header roots. `chthollyc doctor` performs
the mandatory installation-level CFFI checks without linking libclang and
accepts `--cffi-config` for config-specific toolchain discovery.

Doctor JSONL v1 also reports the discovery trace and a normalized environment
event. The trace records compiler candidates, selected paths, rejected paths,
and the source of each decision (config, environment, PATH, vcvars, or
vswhere); the environment event includes target, compiler family, compiler
version, and toolchain fingerprint. Human output summarizes the same run with
`c-discovery` while retaining the existing compiler, SDK, include, library, and
probe lines. This makes Linux `CC`/sysroot and Windows MSVC/vcvars/SDK failures
actionable without changing the CFFI artifact ABI.

Toolchain discovery supports an optional request-fingerprinted cache:

```powershell
chtholly-cffi doctor --target x86_64-pc-windows-msvc `
  --cache-dir .\\.chtholly\\cffi-toolchain --output-format jsonl-v1
```

The first lookup performs normal discovery; later processes may reuse the
strictly validated contract from disk and report `cache-hit-disk`. The cache
key includes target, explicit compiler/SDK/sysroot requests, relevant
environment values, and the cache schema. Cache records contain only
serializable toolchain facts, use atomic replacement and a per-key lock, and
are ignored on truncation, invalid fields, or fingerprint mismatch. In-process
lookups use the same key and report `cache-hit-memory`.

Cache entries are governed by a 32-entry LRU and a 30-day access expiry by
default. Metadata is written atomically beside each `.toolchain` record;
legacy V1 records remain readable and receive metadata on first access. Doctor
JSONL emits a `cache-metrics` summary, and `cache-gc` performs explicit cleanup.
The cache test treats these counters as application evidence: cold discovery
reports a miss, an independent warm command a disk hit, and a damaged record
an invalid-entry fallback.

The repository contains checked-in SQLite 3.40.1/3.53.4, zlib 1.2.11/1.3.1,
and libcurl 7.88.1/8.12.1
upgrade fixtures. Tests generate against the older public header, regenerate
against the newer header, preserve manual overlays, verify a new receipt, and
load the resulting provider artifact from an independent consumer. The
SQLite, zlib, and libcurl consumers execute representative retained and newly
introduced entry points through the native link closure, including SQLite
version/initialization, zlib version/bounds/flags, and libcurl
pause/error-string/cleanup calls.
Each consumer also has a negative run with its provider link library replaced
by a deterministic missing path. Native build fails before output publication,
then the manifest is restored and a locked warm run must succeed. The same
closure checks run on Linux ELF and Windows MSVC link paths.
The Windows fixture additionally compiles a provider wrapper against the real
SDK `bcrypt.h` and links `bcrypt.lib`; its independent consumer executes the
`BCryptGenRandom` path, while the missing `bcrypt.lib` case remains negative.
The same corpus now exercises `BCryptGetProperty`: a real SDK property query
writes a caller buffer and an `out ULONG` result length, while the NTSTATUS
return is projected through the existing status-plus-output `error code`
contract.
The BCrypt fixture also executes an invalid-property query and a deliberately
undersized property buffer. Both failures remain `Err` results; the undersized
call publishes the required length. CFFI verification preflights the complete
native link closure, canonicalizes every resolved library path and content
digest, and incorporates the closure identity into the existing receipt probe
identity. Missing or conflicting `.lib` candidates fail closed with the
request, searched directories, candidate paths, and SHA-256 values.
The cross-package corpus also runs real failure calls from SQLite, zlib, and
libcurl. SQLite opens an invalid path and closes any partially returned handle;
zlib receives a corrupt stream; libcurl performs with an invalid easy handle.
Each result is projected through the existing CFFI error-code contract and is
linked through an isolated real-library provider so the generic fixture
provider remains link-closed for unrelated probes.
Receipt pressure tests now require repeated identical inputs to produce
byte-identical `CHCFFI3` files, while a deliberate module/config identity change
must produce a different receipt. Package configuration fingerprints and
immutable archive closures are checked with the same distinction.

Toolchain diagnostics additionally identify validated and missing components.
On Windows these include VC tools, Windows SDK, UCRT, INCLUDE and LIB
directories. On Linux they include the requested/resolved sysroot, its
`usr/include` and `usr/lib` directories, compiler resource directory, and
system search paths. Missing entries are reported as component events with a
normalized absolute path and a stable rejection reason.
Windows discovery also canonicalizes the SDK root implied by all INCLUDE/LIB
candidates. Divergent roots are reported as an SDK candidate conflict and are
rejected before CFFI generation; the selected root is emitted as a stable
discovery event.

On Linux, doctor also records the compiler-reported target triple, multiarch,
multilib output, and whether discovery uses host-root or an explicit sysroot.
It validates standard C runtime include/library search directories and rejects
an incompatible compiler triple before CFFI generation. Explicit sysroots must
provide `usr/include` and `usr/lib`; host-root mode reports the compiler's
native system paths without pretending that `/` is a configured sysroot.
Each standard C runtime header (`stddef.h`, `stdint.h`, `stdio.h`, `stdlib.h`,
and `errno.h`) receives an independent `header-probe` event. An explicit
sysroot or compiler target mismatch fails before generation and names the
missing path or requested/actual triple.
Linux doctor also queries CRT startup and runtime files (`crt1.o`, `crti.o`,
`crtn.o`, libc and libgcc) through the selected compiler. Their resolved paths
and missing-file results are emitted as `runtime-file` events and contribute to
toolchain identity; the resulting runtime contract digest is stable across
doctor, generation, verification, and receipt creation.
Linux tests construct explicit sysroots in temporary directories, so runtime
paths are never hard-coded into repository fixtures. Missing CRT files and
incomplete sysroot closures are rejected before receipt creation.
The Linux positive fixture copies the compiler-resolved include, library,
multiarch, and CRT closure into a temporary sysroot and proves a native CFFI
probe link before generating its receipt. Startup CRT objects are checked as
little-endian ELF files for the requested x86_64 machine; replacing one with
an i386 (or malformed) ELF fails during discovery with an architecture
mismatch before generation or linking.
