# Next Time Value Semantics

Status: normative and implemented for the `std::time` value layer. Structured
timeout source syntax is not part of this milestone.

## Boundary

`std::time` owns a signed `Duration`, an opaque monotonic `Instant`, and the
safe `monotonic_now` clock query. The types are ordinary standard-library
nominals implemented with existing constant functions, checked integer
arithmetic, aggregate layout, and C FFI. The compiler adds no time-specific
token, numeric suffix, operator protocol, SemIR instruction, LowIR instruction,
or artifact encoding.

The initial arithmetic surface is deliberately affine. Duration supports
unit construction, addition, subtraction, negation, and comparison. Instant
supports Duration offsets, Instant difference, and comparison. Scalar
multiplication and division, Duration ratios, floating-point units, calendar
units, wall-clock conversion, and public Instant construction are deferred.

## Mathematical Values And Normalization

`Duration` has the C representation `i64 seconds`, `u32 nanoseconds`, and
`u32 reserved`. Its mathematical value in nanoseconds is
`seconds * 1_000_000_000 + nanoseconds`. Every safe value is floor-normalized:
`nanoseconds` is less than one billion and `reserved` is zero. Consequently,
negative fractional values use the preceding whole second; negative one
nanosecond is `(-1, 999_999_999, 0)`. The exact value domain is
`[i64::MIN * 1e9, i64::MAX * 1e9 + 999_999_999]`.

`Instant` has the C representation `u64 seconds`, `u32 nanoseconds`, and
`u32 reserved`, with the same nanosecond and reserved-field invariants. Its
domain is `[0, u64::MAX * 1e9 + 999_999_999]`. Its origin is arbitrary and
private to the hosted monotonic clock. An Instant is meaningful only within
one runtime invocation and one clock authority. It is not civil time, Unix
time, a stable serialization value, or an inter-process identifier.

Both nominals use `repr(C)`, occupy 16 bytes on supported targets, and have
trivial copy, move, and drop behavior. Their fields remain private. Unsafe FFI
that imports a non-normalized representation violates the FFI precondition;
safe standard-library operations only produce normalized values.

## Arithmetic And Overflow

Pure value operations and unit constructors are public `const fn` methods.
The unit set is nanoseconds, microseconds, milliseconds, seconds, minutes, and
hours. Inputs are signed `i64`; there are no literal suffixes and no implicit
floating-point conversion. Unit construction splits quotient and remainder
before scaling so a representable result is never rejected because a temporary
total-nanosecond calculation overflowed.

Duration addition and subtraction operate on the normalized pair with explicit
nanosecond carry or borrow. Negation handles the `i64::MIN` boundary without
first negating the seconds field. Instant offset operations handle positive
and negative Duration directly; subtraction is not implemented by first
negating the Duration. Instant difference normalizes the signed result and
traps when it cannot be represented by Duration. Intermediate evaluation must
not overflow when the final mathematical result is representable.

Overflow follows the existing checked-integer language rule. During constant
evaluation it produces `IntegerOverflow`; at runtime it calls
`chtholly_next_runtime_v1_trap_arithmetic` with the integer-overflow reason.
There is no
implicit wrapping or saturation and no time-specific trap ABI.

Comparison is structural over normalized seconds and nanoseconds. Duration
compares signed seconds first; Instant compares unsigned seconds first. The
public v1 methods are `equal`, `less_than`, and `less_than_or_equal`; callers
derive the reverse relations by exchanging operands. Time values do not
receive special `+`, `-`, or comparison operator behavior.

## Clock Authority And Failure

`monotonic_now` is a safe wrapper over the sole hosted authority,
`chtholly_next_runtime_v1_monotonic_now(uint64_t*, uint32_t*)`. It returns
canonical
`std::result::Result<Instant, ClockError>`. A zero runtime status and a
nanosecond output below one billion produce `Ok`; any nonzero status or invalid
nanosecond output produces the closed error `ClockError::Unavailable`. The
wrapper always initializes the reserved field to zero.

There is no public Instant constructor or field accessor. Constant evaluation
can execute pure Instant methods when an Instant operand already exists, but
source cannot obtain a clock sample in a constant context. Module constants
therefore cannot manufacture or persist a clock reading.

The task deadline runtime already stores normalized absolute instants from the
same authority. `std::time` does not expose task registrations or reinterpret
the service-context nanosecond counters. A later timeout scope may translate a
Duration to one absolute Instant exactly once, but that source and ownership
contract remains a separate design milestone.