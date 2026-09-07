# Component ABI-2 Resource Protocol

Status: design-gate baseline; an isolated ABI-2 prototype and executable gate
evidence exist, but the public Component ABI remains ABI-1 and Runtime ABI v1
is unchanged. Prototype code is not a product compatibility promise.

## Purpose

Component ABI-1 intentionally transports only synchronous scalar results and
call-scoped read-only byte views. It cannot safely express a pending channel
operation, an owned buffer, or a completion capability. ABI-2 is therefore
designed as a resource protocol rather than as a wider collection of C value
records.

## Core entities

An ABI-2 operation descriptor is an immutable, versioned record containing:

- canonical component/module/entity identity;
- operation kind and terminal cardinality;
- payload type, layout, representation, and lifecycle fingerprints;
- channel/resource identity and lease policy;
- ownership, initialization, close, cancellation, and quiescence facts;
- runtime ABI and contract digests.

An operation capability is an opaque owner-bearing handle. Its public state
machine is:

```text
Created -> Armed -> (Committed | Failed | Cancelled) -> Released
Created -> Failed
Created -> Cancelled
```

Only the compiler/runtime protocol may advance these states. A component host
invokes by stable operation ID and descriptor digest; it never receives a
native callback address as a durable identity.

## Ownership and cleanup

- Send prepare reserves capacity but does not move the source payload.
- Send commit performs the verified destructive move and publishes the queue
  owner exactly once.
- Receive acquire removes a payload into an active capability; receive commit
  initializes the destination and releases the queue ownership.
- Cancel or failure before commit invokes no move callback and preserves source
  ownership.
- Cancel or failure after acquire drops the queued payload exactly once and
  leaves the destination uninitialized.
- Channel close rejects new operations, requests cancellation where required,
  waits for active capabilities to reach terminal state, and only then destroys
  storage.
- Every capability has one terminal cleanup obligation; frame destruction,
  component unload, and host release must converge on the same release path.

## Cancellation and completion

Operation completion is represented as a one-shot or multi-submit descriptor,
using the existing internal Outcome vocabulary without exposing an
`Outcome<T>` source type. The descriptor records legal transitions and
cardinality; LowIR verifies it before native lowering.

Precedence is deterministic:

1. a committed payload/result is not replaced by a later cancellation;
2. cancellation or deadline expiry before commit selects the cancellation
   terminal;
3. an already-selected component error is preserved while its children drain;
4. close waits for quiescence and cannot race destruction with an armed
   capability.

Cancellation is not an `ErrorCode` conversion and cannot be silently treated
as success.

## Compatibility boundary

ABI-2 receives a new component ABI epoch and new descriptor magic. ABI-1
loaders reject ABI-2 descriptors before invocation; ABI-2 loaders reject ABI-1
operations that claim richer capabilities. Runtime ABI v1 remains valid for
existing synchronous paths. No ABI-2 field is added to current artifacts until
the operation state machine, target layout, host lease behavior, and upgrade
policy have executable evidence.

Operation identity includes canonical semantic facts and contract digests, but
excludes session-local IDs, process addresses, callback addresses, and executor
identity. Artifact replay must reconstruct the same descriptor or fail closed.

## Compiler architecture boundary

SemIR owns the canonical operation descriptor and ownership transition facts.
LowIR owns a verified operation plan, lease edges, suspension kind, cleanup
graph, and terminal precedence. LLVM lowers only that plan and does not infer
resource behavior from symbol names. Imported descriptors are resolved through
owner-checked artifact references.