# Compiler Architecture V21: POSIX `recv` Outcome Lanes

Status: implemented by the Chtholly Tier-1 CFFI wave.

## Scope

V21 validates the existing POSIX outcome projection against the real Linux
socket ABI:

```c
ssize_t recv(int sockfd, void *buffer, size_t capacity, int flags);
```

CFDL continues to use:

```cfdl
outcome posix_read<u8>(buffer, capacity)
error errno when result == -1
```

The public call retains all ordinary lanes, including `sockfd` and `flags`.
Only buffer and capacity participate in the Data/Eof/prefix projection. No
ordinary input is hidden or inferred from the symbol name.

## Compiler Boundary

Interop records a public argument source for each physical lane. LowIR verifies
that the physical `recv` signature and the public function type have the same
ordinary lane order, then LLVM emits the already-formed four-argument call.
Positive results publish the initialized buffer prefix, zero publishes Eof for
nonzero capacity, zero capacity publishes empty Data, and `-1` reads errno.

This confirms that the V20 lane model generalizes from hidden Win32 lanes to a
POSIX ABI with additional ordinary input lanes without introducing a new
outcome syntax or backend symbol special case.

## Deliberate Non-Goals

V21 does not add `buffer_prefix` syntax, `recvmsg`, scatter/gather buffers,
nonblocking retry policy, `MSG_PEEK` semantics, or `fread`'s `feof`/`ferror`
side-channel contract. Those require separate semantic design.
