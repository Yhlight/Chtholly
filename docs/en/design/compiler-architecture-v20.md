# Compiler Architecture V20: Win32 Read Outcome Lanes

Status: implemented by the Chtholly 1.10 Tier-1 CFFI wave.

## Source And ABI Boundary

CFDL adds the bounded synchronous contract:

```cfdl
outcome win32_read<u8>(buffer, capacity, count, overlapped)
error win32 when result == 0
```

The physical Win64 call keeps all five `ReadFile` lanes. The published
Chtholly function exposes handle, mutable buffer, and 32-bit capacity only and
returns `Result<ReadOutcome<slice<u8>>, u32>`. The compiler owns mutable
32-bit count storage and injects a null `OVERLAPPED`; asynchronous completion,
`ERROR_IO_PENDING`, and message-mode `ERROR_MORE_DATA` are outside this
contract.

## Typed Lane Plan

Interop records a physical-to-public lane source for every ABI argument:
`PublicArgument(index)`, `OutcomeStorage`, or `NullPointer`. Buffer, capacity,
count, and context lane identities are explicit artifact facts. LowIR
reconstructs the physical layout from this map and verifies it against the
foreign ABI signature before LLVM emission. Public parameter count and physical
parameter count may differ only for this verified projection.

LLVM allocates count storage in the entry block, starts its lifetime before the
call, and loads it only in the success block. On failure, `GetLastError` is read
without touching count. A positive count produces `Data(buffer[0..count])`;
zero with nonzero capacity produces `Eof`; zero capacity and zero count produces
`Data(empty)`. Count beyond capacity and positive count with null storage trap
as provider contract violations.

No layer selects this behavior from the external symbol name. Regeneration
preserves the authored outcome/error clauses and updates all four lane names
only after a compatible declaration rename is established.

## Version Closure

CFDL epoch 14, `CHNXIOP11` format 11/schema 10, semantic artifact epoch 22,
standard-library epoch 10, Package Artifact v20, `CHNXTPK77` state 74, cache
namespace `next-v46`, foreign operation fingerprint v8, CFFI state `CHCFFIS5`,
and concrete specialization `CHNXSCC50` replace their predecessors. CFFI
config v3, receipt `CHCFFI3`, Component ABI epoch 1, runtime ABI v1, and native
C ABI are unchanged.
