# Compiler Architecture V9

Status: implemented for the Chtholly 1.9.5 real-host stability wave.

Architecture V9 retains Component ABI epoch 1 and proves its loader boundary
with an independent host, two independently built components, and sustained
concurrent unload.

## Public Host Boundary

The real host includes only the public C loader header and links the standalone
loader archive. It does not include `Next/ComponentABI.h`, link compiler
libraries, or decode `CHNXCMP1`. The new requirement initializer converts a
deployment identity, pinned contract digest, normalized target, and runtime ABI
into the exact epoch-1 handshake record.

## Multi-Component Isolation

Alpha and Beta are separate source projects with different component
identities. Both export `plugin::process` and `plugin::hold`. Their export IDs
differ because identity participates in the hash; using Alpha's ID with Beta
is rejected. Each cycle copies both libraries to unique generation paths,
loads them, invokes both under concurrent traffic, and removes the copies only
after release.

## Concurrent Close And Release

Epoch 1 keeps `unload` for externally synchronized callers and adds explicit
`close` plus `release` operations. `close` atomically rejects new calls, waits
for acquired call leases, unloads the library, and retains a closed opaque
handle. Calls against that handle return `CLOSING`. `release` destroys the
handle after close; it never unloads live code.

The host starts a long Alpha call, closes Alpha concurrently, observes
`CLOSING` while close is still waiting, and keeps Beta traffic active. It then
releases Alpha, verifies file/mapping removal, closes Beta, and reloads both in
the next generation.

## Evidence And Compatibility

The normal cross-platform gate runs 15 seconds. Windows and Linux each passed a
120-second soak with hundreds of load/unload generations, no call failures,
no cleanup failures, and stable post-warmup handles/file descriptors. Linux
GCC ASan+UBSan and Clang 18 TSan 15-second runs pass. GCC TSan on WSL cannot
reserve shadow memory and exits before user code; it is not treated as a race
result.

No source grammar, CFDL surface, Component ABI descriptor, `CHNXCMP1`, package
artifact v10, semantic epoch, compiler contract, cache, or runtime ABI changes.
The requirement initializer and close/release functions are additive loader-v1
APIs.
