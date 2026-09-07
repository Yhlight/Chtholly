#pragma once

#include "chtholly/Core/Id.h"

namespace chtholly::compiler {

// Identifies an IR only within one compilation session. It is never a stable
// or serialized identity.
struct CheckIRId : core::IndexBase<CheckIRId> {
  using IndexBase::IndexBase;
};

// A canonical public identity owned by the session-level interface registry.
// Unlike CheckIRId, references to this identity are persisted by canonical
// package/module/name and remapped on load.
struct PublicEntityId : core::IndexBase<PublicEntityId> {
  using IndexBase::IndexBase;
};

struct NominalTypeId : core::IndexBase<NominalTypeId> {
  using IndexBase::IndexBase;
};

// Identifies a variant within one canonical enum declaration. The enclosing
// NominalTypeId remains part of every semantic use of this index.
struct EnumVariantId : core::IndexBase<EnumVariantId> {
  using IndexBase::IndexBase;
};

struct NominalTypeSpecificId : core::IndexBase<NominalTypeSpecificId> {
  using IndexBase::IndexBase;
};

struct IdentifierId : core::IndexBase<IdentifierId> {
  using IndexBase::IndexBase;
};

struct StringLiteralId : core::IndexBase<StringLiteralId> {
  using IndexBase::IndexBase;
};

struct IntegerId : core::IndexBase<IntegerId> {
  using IndexBase::IndexBase;
};

struct CanonicalTypeId : core::IndexBase<CanonicalTypeId> {
  using IndexBase::IndexBase;
};

struct ForeignResourceProtocolId : core::IndexBase<ForeignResourceProtocolId> {
  using IndexBase::IndexBase;
};
struct ForeignOperationPlanId : core::IndexBase<ForeignOperationPlanId> {
  using IndexBase::IndexBase;
};
struct PayloadOperationPlanId : core::IndexBase<PayloadOperationPlanId> {
  using IndexBase::IndexBase;
};
struct ForeignCallOutcomePlanId : core::IndexBase<ForeignCallOutcomePlanId> {
  using IndexBase::IndexBase;
};
struct ForeignOperationCallbackPlanId
    : core::IndexBase<ForeignOperationCallbackPlanId> {
  using IndexBase::IndexBase;
};
struct ForeignOperationCompletionPlanId
    : core::IndexBase<ForeignOperationCompletionPlanId> {
  using IndexBase::IndexBase;
};
// Session-local identity for a canonical value/protocol outcome descriptor.
// The descriptor itself is never a serialized pointer or runtime handle.
struct OutcomeDescriptorId : core::IndexBase<OutcomeDescriptorId> {
  using IndexBase::IndexBase;
};

struct CanonicalConstantId : core::IndexBase<CanonicalConstantId> {
  using IndexBase::IndexBase;
};

struct GenericId : core::IndexBase<GenericId> {
  using IndexBase::IndexBase;
};

struct SpecificId : core::IndexBase<SpecificId> {
  using IndexBase::IndexBase;
};

struct InterfaceId : core::IndexBase<InterfaceId> {
  using IndexBase::IndexBase;
};

struct InterfaceWitnessId : core::IndexBase<InterfaceWitnessId> {
  using IndexBase::IndexBase;
};

struct TypeAliasId : core::IndexBase<TypeAliasId> {
  using IndexBase::IndexBase;
};

} // namespace chtholly::compiler
