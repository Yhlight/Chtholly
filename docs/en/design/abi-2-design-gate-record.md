# Component ABI-2 Design-Gate Record

Status: design-pending. This record describes the experimental prototype and
the evidence required before publishing Component ABI-2. It does not change
the public Component ABI-1, Runtime ABI v1, artifact compatibility, or the
1.0--1.10 source surfaces.

## Frozen protocol boundary

ABI-2 descriptors use the `CHTHAB2` envelope, descriptor version 1, ABI epoch
2, bounded little-endian canonical fields, and the digest domain
`chtholly.component.abi2.descriptor.v1`. Descriptor identity is formed from
canonical component, entity, resource, operation, payload, lifecycle,
ownership, contract, runtime, and plan facts. Process addresses, callback
addresses, executor identity, and session-local IDs are excluded.

An operation capability has one legal linear lifecycle:

```text
Created -> Armed -> Committed -> Released
                  -> Failed    -> Released
                  -> Cancelled  -> Released
Created -> Failed
Created -> Cancelled
```

Every terminal transition is one-shot. Destroying an armed capability is
rejected. Frame destruction, host release, and component unload share the same
exactly-once release obligation.

Send prepare reserves capacity without moving the source. Send commit transfers
queue ownership; pre-commit cancellation or failure preserves the source.
Receive acquire takes queue ownership into a capability. Receive commit
initializes the destination; receive cancellation or failure drops the queued
payload once and leaves the destination uninitialized.

Close rejects new operations, requests cancellation where required, waits for
lease quiescence, and only then destroys storage. A committed result wins over
later cancellation; a selected component error is preserved while children
drain; cancellation is not converted into success or an ordinary error code.