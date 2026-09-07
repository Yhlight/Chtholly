# Next Contract Artifact Consumption

Status: implemented focused cross-module verification.

## Boundary

The existing Chtholly `contract {}` syntax is published through the existing
`PublicFunctionArtifact.ownership_summary` field. No source syntax or artifact
ABI field was added. A provider bodyless declaration remains a `Forward`
public callable; its summary is canonicalized before publication and verified
again after manifest encode/decode.

Consumers obtain the summary from the imported public entity through the
dependency manifest. They do not need the provider source body and do not
re-infer or widen the provider's ownership facts. Imported calls therefore use
the same borrow-return provenance and effect checks as local calls. LLVM emits
the imported callable as an external declaration rather than an internal
undefined function.

## Initialize Boundary

Chtholly contract syntax currently expresses `reads`, `writes`, `takes`,
borrows, postconditions, and borrowed returns. The direct `Initialize` effect
is intentionally still produced by CFDL `out` lanes. `ensures initialized`
can publish an initialized postcondition for a declared write, but it does not
invent a new Chtholly source spelling for an initialization capability. The
cross-module test therefore verifies Chtholly borrowed-return consumption and
uses the existing CFDL provider fixture for cross-package Initialize/
Invalidate behavior.

The boundary is enforced in both directions: `initialize` is not accepted as a
Chtholly contract entry, and an ordinary `writes` plus `ensures initialized`
summary contains a postcondition but no `Initialize` effect. Only a CFDL
`out` lane (or an ordinary wrapper whose verified body forwards that capability)
may publish the direct effect used by place-state initialization.
