# Async Channel API Design Decision

Status: design decision; no source or ABI surface is admitted.

## Decision

Chtholly does not add a 1.11 source version for cancellation-aware channel
wrappers at this time. The frozen source versions remain 1.0 through 1.10.
`std::typed_channel::Channel<T>` keeps its synchronous Result-based API and
the `cancellation-aware-channel` product capability remains `design-pending`.

This is a semantic decision, not an implementation deferral. The current
async model rejects a checked reference that remains live across a possible
suspension. The existing typed-channel operations use a mutable `Channel<T>&`
receiver. A direct API such as:

```text
async fn send_async<T>(channel: Channel<T>&, value: T): Result<void, ErrorCode>
```

would require the coroutine frame to retain a channel borrow while the send is
pending, and is therefore invalid under the current abstract machine.

## Alternatives

| Alternative | Result |
| --- | --- |
| `Channel<T>&` receiver in an async function | Rejected: it violates the stable no-borrow-across-suspension rule. |
| Move `Channel<T>` into the operation and return it on completion | Deferred: it needs an explicit owner/return protocol for success, error, close, cancellation, and task drop. |
| Introduce a suspension-safe borrowed reference | Rejected for now: it would require a new frame-owned provenance and borrow ABI. |
| Compiler-owned operation capability with a channel lease | Selected for Component ABI-2 design; it separates the pending operation from a checked source borrow. |

## ABI-2 design input

The selected future abstraction is an owned operation capability:

```text
channel owner
  -> operation capability + payload ownership
  -> ready | armed | cancelled | committed | failed
  -> capability release after terminal cleanup
```

The capability must carry canonical channel identity, payload type and
representation fingerprints, lifecycle witness identity, cancellation domain,
and a resource-contract digest. Session-local IDs and callback addresses are
not part of its identity.

While an operation is armed, the channel lease prevents conflicting structural
mutation. Payload ownership remains with the source until send commit; receive
initializes the destination only after receive commit. Close is a quiescence
operation and cannot destroy a channel with an armed operation.

Cancellation, deadline expiry, owner cancellation, and close are terminal
events. They must be linearized before payload commit, and cancellation must
never be converted into the declared `ErrorCode` value. A failed or cancelled
send preserves the source payload; a failed or cancelled receive leaves the
destination uninitialized.

## Source-surface boundary

No spelling is reserved for this design. In particular, Chtholly does not add
`await`, `Future`, `Outcome<T>`, lifetime parameters, async methods, foreign
async declarations, or implicit FFI operations. A future source API may be
compiler-owned standard-library declarations, but only after the operation
capability and Component ABI-2 contracts are frozen.

## Acceptance boundary

This document is complete when a future implementation can answer, without
new product decisions, how operation identity, payload ownership, channel
leases, close quiescence, cancellation precedence, cleanup, artifact replay,
and cross-component transport behave. Until then, no 1.11 async channel API is
implemented or advertised.
