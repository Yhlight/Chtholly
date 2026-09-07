# Compiler Architecture V7

Status: implemented for the Chtholly 1.9.3 Linux x64 validation wave.

Architecture V7 retains V6's aggregate representation and concrete-specific
identity contracts and adds native target conformance as a phase boundary.

## SysV ABI Conformance

LowIR remains the single owner of foreign ABI classification. The dedicated
SysV AMD64 gate constructs completed semantic `repr(C)` structs, arrays, and
unions, then verifies the immutable function layout, the phase-end
recalculation, and the LLVM declaration generated from that layout. It covers
narrow integer extension, INTEGER and SSE eightbytes, mixed lanes, 16-byte
direct values, and MEMORY `byval`/`sret` values.

Declaration, parameter order, and return-slot
facts have one owner.

## Native Linux Boundary

Linux x64 is now a native build, link, and execution target. Native linker
entry points create their output parent before invoking the external linker;
the POSIX runtime links pthread through its resource manifest. The telemetry
workspace runs cold and warm as a System V ELF64 executable, retaining V6's
cross-package struct copy, aggregate-array return, and multiple warm `MapMut`
specializations.

System `cpp-httplib` discovery distinguishes compiled Debian packages from
header-only installations and supports the 0.14 multipart API without changing
the registry protocol. Zero-length console writes flush without passing a null
buffer to `fwrite`.

## Compatibility Contract

No source grammar, standard-library API, component ABI, runtime symbol, or
foreign classification rule changes. Semantic artifact epoch 17, compiler
contract 14, cache namespace `next-v38`, concrete component `CHNXSCC46`,
foreign function/call layout epoch 9, callback-thunk epoch 10, and runtime ABI
v1 are unchanged.
