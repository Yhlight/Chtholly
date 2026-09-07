# Next Lifecycle Witness Calls

## Contract

Next resolves lifecycle policy while building SemIR and nominal artifacts.
LowIR and LLVM consume the bound result; they do not search impls or infer
capability from target layout.

The currently supported source boundary is intentionally small:

```cns
lifecycle(copy = custom, move = default, drop = custom)
struct Resource { value: i32; }

impl core::Copy for Resource {
  fn copy(self: Self&, source: const Self&): void { return; }
}

impl Resource {
  fn drop(self: Self&): void { return; }
}
```

Concrete structs and generic impls evaluated for concrete nominal specifics are
accepted. The copy ABI is
`fn(Self&, const Self&): void`; the drop ABI is `fn(Self&): void`. These impls
do not introduce ordinary source-visible methods or general trait lookup.

## Canonical Resolution

Lifecycle functions are published under lexer-inexpressible canonical names:

```text
$lifecycle$<NominalName>$copy
$lifecycle$<NominalName>$drop
```

Each `CHNXWIT7` witness commits to the nominal specific, representation facts,
field-wise generated copy/drop bodies, optional canonical targets, and sorted
transitive specific closure. Generic impl owner patterns are evaluated under
the concrete nominal arguments; applicable template fingerprints enter the
closure before publication. A consumer loads the witness under its active
`NextArtifactLease` and validates the nominal template, arguments, provider
semantic options, structural fingerprint, and generated body. Custom targets
materialize a canonical external `SemFunctionRef`; default generic lifecycle
work instead consumes the persisted field body. A re-export preserves the
directly observed facade binding while any function reference still points to
the original canonical entity.

LowIR emits `LifecycleCopy`, `LifecycleDestroy`, or
`LifecycleDestroyValue`. Conditional cleanup is an explicit
`IsInitialized`/`BranchIf` path instead of a combined lifecycle opcode. The
verifier matches the opcode, bound type witness, and canonical entity before
LLVM creates the declaration or call. Local lifecycle definitions use the same
external artifact ABI even though their names are hidden from source lookup.

## Incremental State

`CHNXTPK16` version 16 stores lifecycle observations with four independent
facts: visible provider/binding, canonical callable fingerprint, witness result
fingerprint, and transitive closure fingerprint. The visible edge handles
re-export changes; the witness identity handles representation or lifecycle
policy changes. Active leases and garbage collection retain the referenced
witness CAS closure.

Custom cleanup is emitted into interned LowIR tails keyed by cleanup action,
place, conditional state, ended lifetime set, and successor. Loop backedges
therefore share one verified cleanup block instead of duplicating custom calls.