# Typed channel ABI and lowering

Status: compiler transition/lifecycle lowering implemented (2026-09-02).

Typed-channel transitions are represented by append-only SemIR/LowIR
instructions. Before concrete specialization publication, the semantic pass
walks function blocks and emits one `SemTypedChannelDescriptor` per owner and
canonical payload type. The descriptor contains payload, layout and lifecycle
fingerprints, representation facts, compiler-derived Send/Sync facts and
canonical lifecycle targets. Session-local `FunctionRefId` values are never
serialized.

Concrete specialization artifacts persist the target-independent
`ConcreteTypedChannelDescriptor` beside the specialized body. Artifact
verification rejects borrowed or dependent payloads, non-transferable values,
missing lifecycle witnesses, malformed targets and unknown runtime epochs.

LLVM lowering resolves the descriptor for the current function and payload,
then materializes private `void(void*, void*)` move and `void(void*)` drop
thunks through the existing nominal object/lifecycle services. A runtime
descriptor uses the v1 layout:

```c
{ uint32_t abi; uint32_t capabilities; uint64_t size; uint64_t alignment;
  void (*move)(void*, void*); void (*drop)(void*); }
```

The runtime typed-channel token remains caller-owned and must be completed or
cancelled exactly once. Prepare does not transfer source ownership; commit is
the only destructive move. The existing byte-channel CFDL and Component ABI-1
remain separate compatibility boundaries.

The compiler intrinsic role names are reserved as `channel.*` identities and
are intentionally kept separate from ordinary foreign byte-channel calls.
The additive `std::typed_channel::Channel<T>` facade is now published through
the stdlib manifest with Result-based `init`, `send`, `receive`, and `close`.

The source surface now has an explicit `initializes p` contract effect for
receive destinations and requires `(move channel).close()`. Low-level token
operations remain compiler-internal. The current `Result<void, ErrorCode>` send
form conservatively consumes its by-value argument at the call boundary; a
future recovery-result policy can make failed-send payload reuse explicit,
without weakening place-state checks or introducing bytewise fallback.
