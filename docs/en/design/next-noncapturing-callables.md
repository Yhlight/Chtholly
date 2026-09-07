# Next Non-capturing Callables

Status: implemented v1 language boundary.

## Source Contract

`fn(T...): U` is the ordinary callable type. Safe concrete ordinary functions
and associated functions may form values of this type. The value has no bound
receiver or captured environment, is immutable and trivially copyable, and has
pointer representation.

Generic templates must first be concretized. Receiver-bound instance methods
and unsafe declarations cannot form ordinary callable values. Foreign ABI
callables are CFDL artifact entities, not Chtholly source types. Capturing
closures remain post-v1.

## Explicit IR

SemIR uses `FunctionValue` for value formation and `IndirectCall` for
invocation. LowIR preserves both operations. LLVM lowers a function value to a
function address and constructs the indirect call signature from the verified
ordinary Chtholly function type, including the same in-place result convention
as a direct call.

Function types are public artifact types. Generic templates and concrete
specializations encode function values with canonical package, module, name,
fingerprint identities, plus structural concrete type arguments when owner or
function generics have been concretized. Replay validates those arguments
against the public entity's declared arity before specialization;
process-local `FunctionRefId`, `SpecificId`, and canonical type IDs never cross
an artifact boundary. Specific cloning treats indirect-call operands as value
blocks and preserves one instruction identity for every cloned operand.