# Chtholly Abstract Machine And Memory Model

Status: normative core model. Undefined details must not be guessed by a
frontend or lowering stage.

## Values, Objects, And Storage

A value is a typed semantic result. An object is a value-bearing entity with a
lifetime. Storage is memory capable of containing an object. A place identifies
storage or a logical subobject and carries the authority needed to read,
initialize, mutate, move, borrow, or destroy it.

Each logical ownership cell is in one of four states: `uninitialized`, `live`,
`moved`, or `destroyed`. Construction changes uninitialized storage to live.
A destructive move transfers the value and cleanup responsibility and changes
the source cell to moved. Initialization may make an uninitialized or moved
cell live again. Destruction ends a live object's lifetime exactly once.

Aggregates have independently tracked logical subobjects. Physical overlap,
custom carriers, and bit packing do not merge distinct logical ownership cells.
Reading a non-live cell, destroying it twice, or leaving a required result
partially initialized is invalid in safe code.

Fixed-array elements and tuple positions are separate ownership cells. A
static element projection may be moved, borrowed, initialized, and destroyed
independently; cleanup visits only cells that are live. A dynamic array index
is represented by a wildcard element projection and conservatively overlaps
every element. A `slice<T>` is a borrowed read-only pointer-plus-length view;
`slice_mut<T>` is the explicitly mutable form. Both are borrowed views:
indexing
supports read, shared borrow, mutable borrow, and mutable-slice write, but
never transfers element ownership or creates initialization authority. Slice
element projections therefore cannot be moved or initialized, and dynamic
slice indices conservatively overlap.

The standard `string` byte projection follows the same rule. It creates a
read-only slice view whose lifetime is a loan of the complete string object;
the projection does not allocate, copy, or transfer ownership. A source move,
destruction, or mutating operation that could relocate string storage conflicts
with a live projection. C-string termination and foreign buffer validity are
CFDL facts, not properties inferred from this projection.

## Expression Results And Temporaries

Expressions are classified as error, value, place, temporary, or diverging.
A place identifies an existing object. A temporary is a value whose cleanup
obligation belongs to the current full expression unless ownership is
transferred. A diverging expression has type `never` and produces neither a
value nor an object.

An expression-valued control construct produces a temporary, never a place.
The abstract machine selects one reachable arm and initializes the result once
from that arm. A compiler may realize this rule with a result slot, but the
slot, its address, and whether it was elided are unobservable. Diverging arms
do not initialize the result. When every arm diverges, no result object exists.

Objects local to an arm are destroyed in reverse construction order before
leaving that arm unless their value and cleanup obligation are transferred
into the control result. The selected result remains alive through its owning
full expression. Binding that temporary transfers it into the destination;
the source temporary is not separately destroyed.

Temporary storage does not itself own cleanup. Materializing a temporary starts
its object lifetime and registers exactly one cleanup obligation. The current
full expression consumes that registration in reverse materialization order,
unless ownership is transferred into a value destination or the registration
is explicitly transferred into a lexical reference binding. This separation is
preserved in semantic IR, generic artifacts, concrete specialization, LowIR,
and native lowering.

A declaration initializer, assignment, expression statement, return operand,
and complete call including all arguments form full expressions. An `if`
condition and each evaluation of a `while`, `for`, or `do...while` condition
form independent full expressions. Their temporaries are destroyed after the
condition value is computed and before the selected control edge is entered.
Postfix `?` uses the same registrations: its `Err` edge performs return cleanup,
while its `Ok` edge retains the registrations until the surrounding full
expression ends.

A direct local binding of a read-only reference explicitly extends a temporary
to the binding's lexical lifetime:

```cns
let whole = &make();
let field = &make().value;
```

The field form requires an address-stable projection and extends the complete
root temporary, not only the projected field. Passing a temporary reference as
an argument does not extend it beyond the call's full expression. A reference
to a temporary cannot be returned, stored into a longer-lived destination, or
converted to mutable authority. No source-level lifetime parameter or native
ABI field is introduced by lifetime extension. Escape checking follows
temporary references through aggregate construction, reference-returning
calls, and expression-valued control flow; an invalid derived escape reports
`chtholly.next.sem.ownership.temporary-reference-escape`.

## Nominal Construction And Placement

`T::init(args)` is the explicit nominal construction entry point. The
constructor has no receiver and returns either `Self` or canonical
`Result<Self, E>`. A successful `Self` result owns one live value; a `Result`
error owns no `Self` payload and follows ordinary typed error flow.

`T::init(args) in destination` is a non-allocating expression. Constructor
arguments evaluate left-to-right before `destination`. The destination must be
a mutable local place in the moved state. Direct placement has type `void`.
Fallible placement has canonical type `Result<void, E>`, so
`(T::init(args) in destination)?` uses ordinary exact-error propagation.

The caller supplies separate success and outcome slots to a fallible
constructor. The success slot is committed to the destination only on `Ok`;
on `Err` the destination remains moved and non-live. Constructor-owned locals
are cleaned in reverse construction order before either return. Cleanup
ownership transfers exactly once on commit; a constructor slot and destination
never both destroy the same object, and the failure path never destroys an
uncommitted destination.

## C Union Storage And Active Members

A `repr(C)` union is the deliberate exception to independent aggregate
ownership cells. All alternatives occupy one atomic ownership cell and one
borrow/alias region. Moving through the union or any member moves the complete
storage; borrowing one alternative overlaps every alternative. There is no
runtime discriminant.

The semantic place state separately records `Active(member)`, a control-flow
set of possible active members, or `UnknownActive`. Construction and direct
member assignment establish `Active(member)`. Copy or assignment preserves a
known state, including nested union states. A CFG join unions the predecessor
possibilities; safe access succeeds only when exactly the requested member is
possible. A foreign or opaque returned union starts as `UnknownActive`.

An `unsafe` member access asserts that the selected member may be interpreted
under the platform C rules for that operation. It does not change
`UnknownActive`, discard other path possibilities, or authorize later safe
access. Initialization, move, destruction, and cleanup remain checked even in
an unsafe block.

## Ownership And Cleanup

Owned values are affine: every live owner has exactly one cleanup obligation.
By-value transfer moves non-copyable values. A named non-terminal transfer is
written with `move`; return, temporary, and compiler-proven final-use transfers
may be implicit. `copy` requires a copy capability. Primitive trivial values
may copy implicitly; resource-bearing user types do not.

Cleanup is deterministic. All normal exits and structured residual exits run
the cleanup selected by place state in reverse construction order. Chtholly has
no implicit exception unwinding. Recoverable failure uses typed values such as
`Result`; unrecoverable failure follows an explicit component panic policy.

Heap ownership is provided by library types such as `Box`, `Vec`, and `String`.
Arena and reference-counted ownership are library policies. The core language
does not contain a tracing garbage-collected object domain.

The standard `Vec<T>` policy is contiguous storage with relocation-capable
growth. A checked reference or iterator into its allocation is a loan of the
Vec owner. A structural owner write conflicts with that loan and cannot be
performed until the loan ends; no runtime dangling-reference check is needed.

An uninitialized binding may be passed through a synchronous callable chain when
each callable has a verified `Initialize` effect for the forwarded parameter.
The binding carries one affine initialization capability: it may be forwarded
to exactly one matching callee, but it cannot be read, copied, borrowed into a
longer-lived value, stored, or returned before initialization completes. Every
normally returning path must establish the initialized postcondition before the
capability returns to its original scope. `move` still requires a live value and
does not transfer an uninitialized binding.

## References And Provenance

`const T&` is a shared read-only reference. `T&` is an exclusive mutable
reference. References do not own their referent. Initialization authority is
carried by a verified callable `Initialize` effect rather than by a distinct
source reference type.

Checked references are transparent in source value and place contexts. The
compiler preserves reference identity when the destination requires a
reference and otherwise resolves the complete transparent reference alias
chain to the referent. This projection remains explicit in semantic and lowered IR so provenance,
mutability, ownership transfer, and alias analysis continue to operate on the
referent place. References cannot be rebound, and no adjustment may turn a
shared reference into an exclusive reference. An initialization capability may
establish the identity of previously uninitialized reference storage; later
assignment through that storage writes its referent.

Typed uninitialized storage is represented by a place-state bit, not by a
runtime sentinel value. `let name: T;` and `var name: T;` create such storage,
and an eligible call receives the place through the ordinary `name`
expression. The compiler materializes an internal mutable initialization
projection only when the callable contract has an exact `Initialize` effect
for that lane. Place-state analysis rejects reads, moves, returns, ordinary
borrows, repeated initialization, and scope exits while the transition is not
valid. Afterward `let` storage is immutable and `var` storage remains mutable.

Every reference and raw pointer carries hidden provenance identifying its
storage origin and permitted logical region. Provenance is a value-level
semantic fact, not a source-level lifetime parameter and not part of runtime
layout. Aggregates, closures, and returned values propagate the provenance of
contained references. Storing such a value is valid only when every contained
reference remains valid for the destination's lifetime.

Borrow duration is inferred from reachable uses. Shared borrows may overlap;
an exclusive borrow excludes other overlapping access. Reborrowing narrows
authority and cannot extend provenance. Public semantic interfaces persist
derived return sources and conditions so consumers do not need source bodies.

## Raw Pointers And Unsafe Operations

Raw pointers use `T*` and `const T*`. They preserve allocation provenance but
do not preserve a live borrow or guarantee a valid referent. Null values and
one-past pointers are representable. Dereference requires a live, aligned,
properly typed object inside the pointer's allocation; arithmetic must remain
within that allocation or one-past it.

Pointer dereference, arithmetic, integer-pointer conversion, type punning, and
foreign authority are unsafe operations. Violating their documented
preconditions is undefined behavior. Safe code cannot manufacture invalid
references, observe uninitialized storage, double-destroy an object, or cause a
data race.

Raw-pointer dereference is not the checked-reference projection described
above. It remains an explicit unsafe operation with independently specified
syntax and validity checks. Evaluating `*pointer` requires an `unsafe` context
and yields an external place; it does not create a borrow, acquire ownership,
or make the pointee eligible for `move` or `copy`. Reading or writing that
place requires a non-null, non-one-past pointer to a live, aligned object of
the exact pointee type. Writing additionally requires a non-const pointee and
valid mutable access under the aliasing and concurrency rules. Violation of
these dynamic obligations is undefined behavior; the compiler emits no null,
provenance, alignment, or lifetime check and adds no ABI metadata.

## Concurrency

Types have compiler-derived transfer and sharing capabilities. A value may
cross a thread boundary only when it is transferable. Shared access across
threads requires a shareable type. Ordinary mutation continues to require
exclusive authority; locks, channels, and atomics are verified library
abstractions over narrow unsafe primitives.

Non-atomic conflicting accesses without synchronization constitute a data
race and are impossible in safe code. Atomic operations explicitly select
`relaxed`, `acquire`, `release`, `acq_rel`, or `seq_cst` ordering and follow the
target-independent LLVM/C++20 atomic model. Data-race-free programs using only
ordinary synchronization have sequentially consistent observable behavior.

## Representation

Object representation, expression value representation, and initialization
representation are separate semantic facts. The default representation is not
a public layout promise. `repr(C)` selects the platform C ABI. A future stable
Chtholly representation must name an ABI epoch and publish its evolution rules.
Lowering consumes completed representation facts and may not reconstruct them
from LLVM types.

For a `repr(C)` union, every member offset is zero. Size is the maximum member
size rounded up to the maximum member alignment. These layout facts and the
nominal struct/union kind are persisted in target-keyed artifacts; overlapping
fields are valid only when the persisted kind is union.
