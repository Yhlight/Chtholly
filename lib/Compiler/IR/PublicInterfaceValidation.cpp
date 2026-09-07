#include "chtholly/Compiler/PublicInterface.h"

#include "PublicInterfaceServices.h"

#include <algorithm>

namespace chtholly::compiler {

bool internal::PublicInterfaceValidationService::callableSemanticContract(
    const CallableSemanticContract &contract, std::uint32_t generic_count,
    std::string &error,
    const std::function<bool(const PublicType &, std::uint32_t)> &valid_type) {
  const auto capability = [](CallableSemanticRole role) {
    if (role == CallableSemanticRole::None || role >= CallableSemanticRole::Count)
      return static_cast<std::uint16_t>(CallableCapabilityNone);
    return static_cast<std::uint16_t>(1U << (static_cast<unsigned>(role) - 1U));
  };
  const auto &value = contract;
  error.clear();
  if (value.domain >= CallableSemanticDomain::Count ||
      value.role >= CallableSemanticRole::Count ||
      value.capability != capability(value.role) || value.carrier_path.size() > 256 ||
      (value.has_bit_range &&
       (value.bit_begin >= value.bit_end || value.bit_end > 32 || value.whole_carrier)) ||
      (!value.has_bit_range && (value.bit_begin != 0 || value.bit_end != 0))) {
    error = "callable semantic contract has invalid canonical fields";
    return false;
  }
  if (value.domain == CallableSemanticDomain::Ordinary) {
    if (value.role != CallableSemanticRole::None ||
        value.capability != CallableCapabilityNone ||
        value.owner.kind != PublicTypeKind::Count ||
        value.projector_field != core::AnyId::InvalidIndex || value.whole_carrier ||
        !value.carrier_path.empty() || value.has_bit_range) {
      error = "ordinary callable has a representation helper contract";
      return false;
    }
    return true;
  }
  if (value.domain == CallableSemanticDomain::NominalConstruction) {
    if (value.role != CallableSemanticRole::Constructor ||
        value.owner.kind != PublicTypeKind::Nominal ||
        !valid_type(value.owner, generic_count) ||
        value.projector_field != core::AnyId::InvalidIndex || value.whole_carrier ||
        !value.carrier_path.empty() || value.has_bit_range) {
      error = "constructor contract has invalid nominal owner or role";
      return false;
    }
    return true;
  }
  if (value.owner.kind != PublicTypeKind::Nominal ||
      !valid_type(value.owner, generic_count) ||
      value.role == CallableSemanticRole::None) {
    error = "representation helper contract has no valid nominal owner";
    return false;
  }
  const auto lifecycle = value.role == CallableSemanticRole::Copy ||
                         value.role == CallableSemanticRole::Drop;
  const auto representation = value.role == CallableSemanticRole::Pack ||
                              value.role == CallableSemanticRole::Init;
  const auto projection = value.role >= CallableSemanticRole::ProjectionLoad &&
                          value.role <= CallableSemanticRole::ProjectionBorrowMut;
  const auto shell = value.role >= CallableSemanticRole::ObjectInit &&
                     value.role <= CallableSemanticRole::ObjectDrop;
  const auto expected_domain =
      lifecycle ? CallableSemanticDomain::Lifecycle
      : representation ? CallableSemanticDomain::ValueRepresentation
      : projection ? CallableSemanticDomain::ObjectProjection
                    : CallableSemanticDomain::ObjectShell;
  if ((!lifecycle && !representation && !projection && !shell) ||
      value.domain != expected_domain ||
      (projection ? value.projector_field == core::AnyId::InvalidIndex
                  : value.projector_field != core::AnyId::InvalidIndex) ||
      (projection && value.whole_carrier != value.carrier_path.empty()) ||
      (lifecycle && (value.whole_carrier || !value.carrier_path.empty() ||
                    value.has_bit_range)) ||
      ((representation || shell) &&
       (!value.whole_carrier || !value.carrier_path.empty() ||
        value.has_bit_range))) {
    error = "callable semantic contract role disagrees with its domain";
    return false;
  }
  return true;
}

bool ForeignAbiSignature::verify(std::string &error) const {
  error.clear();
  if (unwind_policy != UnwindPolicy::NoUnwind ||
      calling_convention >= ForeignCallingConvention::Count) {
    error = "foreign callable must prohibit cross-boundary unwinding";
    return false;
  }
  if (is_variadic && parameters.empty()) {
    error = "variadic foreign callable requires a fixed parameter";
    return false;
  }
  const auto valid = [](const ForeignAbiValue &value, bool result) {
    if (value.kind >= ForeignAbiValueKind::Count ||
        (value.kind == ForeignAbiValueKind::Void && !result))
      return false;
    switch (value.kind) {
    case ForeignAbiValueKind::Void:
      return value.width == 0 && !value.pointee_const;
    case ForeignAbiValueKind::Bool:
      return value.width == 1 && !value.pointee_const;
    case ForeignAbiValueKind::SignedInteger:
    case ForeignAbiValueKind::UnsignedInteger:
      return (value.width == 8 || value.width == 16 || value.width == 32 ||
              value.width == 64) &&
             !value.pointee_const;
    case ForeignAbiValueKind::Float:
      return (value.width == 32 || value.width == 64) && !value.pointee_const;
    case ForeignAbiValueKind::RawPointer:
    case ForeignAbiValueKind::Reference:
    case ForeignAbiValueKind::FunctionPointer:
      return value.width == 0;
    case ForeignAbiValueKind::Aggregate:
      return value.width == 0 && !value.pointee_const;
    case ForeignAbiValueKind::Count:
      return false;
    }
    return false;
  };
  if (!valid(result, true) ||
      !std::ranges::all_of(parameters,
                           [&](const auto &value) { return valid(value, false); })) {
    error = "foreign ABI signature has an unsupported value";
    return false;
  }
  return true;
}

} // namespace chtholly::compiler
