# Typed Channel Runtime Prototype

Status: runtime ABI v1 with compiler-owned source facade (2026-09-02).

The existing `std::channel` boundary is a bounded byte transport. It remains
unchanged and is not a generic value channel. This document records the
separate typed-channel boundary used by the compiler-owned
`std::typed_channel::Channel<T>` facade.

## Ownership boundary

Each send has two phases:

1. `send_prepare` reserves one queue slot and may block for capacity. It does
   not inspect or move the source value. A closed/full/error result leaves the
   runtime source pointer unchanged until a later commit.
2. `send_commit` allocates a typed payload slot and invokes the descriptor's
   destructive move callback. Only this operation changes the source place to
   moved and publishes a new queue owner. Allocation or close failure before
   the callback releases the reservation without invoking the callback. The
   current source `send` API consumes its by-value argument at the call
   boundary; payload recovery on a failed Result is a follow-up policy.

Receiving is similarly explicit. `receive_acquire` removes one payload from
   the queue and holds an active receive token. `receive_commit` moves the
   payload into the destination and releases the token. Cancellation drops the
   queued payload exactly once. Channel close waits for prepared sends and
   active receives before destroying the queue.

## Descriptor contract

`chtholly_next_typed_channel_descriptor` is compiler-owned metadata. It
contains an ABI epoch, capability bits, object size/alignment, and destructive
move/drop callbacks. The runtime rejects descriptors without the `Send`
capability, valid layout, or both lifecycle callbacks. It never falls back to
`memcpy` based on size.

The ABI remains a C boundary, and compiler integration now supplies stable
type/lifecycle fingerprints, recursive witness closure, artifact replay, and
target-local move/drop thunks. The additive `std::typed_channel` module exposes
the minimal generic `Channel<T>` intrinsic facade; the legacy byte channel is
still separate.

The telemetry ingest host uses this boundary with a native `Frame` descriptor;
the application-level `.cns` pipeline additionally exercises
`Channel<Sample>::init/send/receive/close` and Result status handling.

## Deferred integration

Guard/suspend interaction, borrowed payloads, and typed multi-producer
scheduling remain gated on semantic closure. Result-based source wrappers are
now provided by `std::typed_channel`; the byte channel and its symbols remain
available for compatibility.
