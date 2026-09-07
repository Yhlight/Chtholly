# Canonical Imported Intrinsic Resolution

Status: implemented in the Next compiler.

Imported compiler intrinsics must be lowered through a canonical external
function reference after generic arguments are concrete. A local imported
evaluator or generic self-specific reference is not a valid lowering target:
it may describe an artifact evaluator body while the intrinsic contract is
owned by the imported public entity.

## Resolution Boundary

`SemIR::resolveCanonicalIntrinsic` owns the boundary. Its input is an imported
`PublicEntityId`, an ordered list of concrete `PublicType` arguments, and a
`CanonicalIntrinsicShapeSpec` containing the expected materialized parameter,
return, error, and async shape.

The helper performs the following steps:

1. Validate the imported function entity and generic argument count.
2. Substitute and materialize the concrete public signature through
   `addCanonicalExternalFunctionRef`.
3. Require the resulting `FunctionRefId` to be an external canonical target,
   with no local evaluator or import instruction attached.
4. Compare parameter count/types, return type, error outcome, and execution
   kind against the declared shape.
5. Return the canonical reference and its materialized local types, or fail
   closed before semantic lowering creates dependent instructions.

`addCanonicalExternalFunctionRef` remains the single materialization primitive.
It records the concrete public arguments and does not reuse a local evaluator
reference that happens to share the same public entity and `SpecificId`.

## Generic Intrinsics

Ordinary overload and generic resolution still use the existing local specific
materialization path. At the final `SemCompilerIntrinsicCall` boundary, the
resolved specific's recorded concrete public arguments are passed to the
helper. The expected shape is taken from that concrete local function type;
the emitted intrinsic instruction therefore receives the canonical external
reference while overload inference retains its existing behavior.

This means `EnumPayloadAccess`, `Switch`, `While`, binding, escape, and cleanup
consume one concrete function reference model without a separate intrinsic
ABI or source syntax. Shape failures are rejected before those lowering paths.

Package-private nominal arguments are the deliberate exception. They have no
stable `PublicEntityId` and cannot be encoded as an imported `PublicType`; the
local concrete intrinsic reference remains in that case and is checked by the
existing SemIR intrinsic verifier. Publicly representable arguments always use
the canonical external path.