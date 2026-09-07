#include "chtholly/Compiler/SemIR.h"

#include "chtholly/Compiler/CallableOwnership.h"

#include <algorithm>
#include <ranges>
#include <string>

namespace chtholly::compiler {

namespace {
constexpr std::uint32_t UnionFieldUnsafeBit = 1U << 31U;
constexpr std::uint32_t UnionFieldIndexMask = ~UnionFieldUnsafeBit;
constexpr std::uint32_t ProjectionIndexMask = 0x7fffffffU;
constexpr std::uint32_t ProjectionKindShift = 31U;
constexpr std::uint32_t MutableReferenceBit = 1U;
constexpr std::uint32_t ParameterProvenanceBit = 1U << 1U;
} // namespace

bool SemIR::verifyTypeRecords(std::string &error) const {
  const auto valid_wake_entry = [&](TypeId entry, TypeId context) {
    if (!entry.hasValue() || entry.index >= types_.size() ||
        type(entry).kind != SemTypeKind::CFunctionPointer)
      return false;
    const auto parameters = typeBlock(TypeBlockId(type(entry).arg0));
    const auto context_parameter = callbackContextParameter(entry);
    if (parameters.size() != 1 || context_parameter != 0 ||
        parameters.front() != context || TypeId(type(entry).arg1) != void_type_)
      return false;
    const auto &contract = callbackContract(entry);
    return std::ranges::none_of(
               contract.effects,
               [](const auto &effect) {
                 return effect.kind != CallableEffectKind::Read &&
                        effect.kind != CallableEffectKind::Write;
               }) &&
           std::ranges::none_of(
               contract.postconditions,
               [&](const auto &postcondition) {
                 const auto derived_from_write = std::ranges::any_of(
                     contract.effects, [&](const auto &effect) {
                       return effect.kind == CallableEffectKind::Write &&
                              effect.region == postcondition.region;
                     });
                 return !derived_from_write ||
                        postcondition.outcomes != (CallableOutcomePreserve |
                                                   CallableOutcomeInitialize);
               }) &&
           contract.return_provenance.empty();
  };
  for (std::size_t index = 0; index < types_.size(); ++index) {
    const auto &value = type(TypeId(static_cast<std::uint32_t>(index)));
    if (value.reserved >= values_->generics().typeCount()) {
      error = "semantic type has an invalid canonical identity";
      return false;
    }
    switch (value.kind) {
    case SemTypeKind::Void:
    case SemTypeKind::Never:
    case SemTypeKind::Bool:
    case SemTypeKind::Char:
    case SemTypeKind::String:
      if (value.arg0 != core::AnyId::InvalidIndex ||
          value.arg1 != core::AnyId::InvalidIndex) {
        error = "primitive type has unexpected arguments";
        return false;
      }
      break;
    case SemTypeKind::Integer:
      if ((value.arg0 != 8 && value.arg0 != 16 && value.arg0 != 32 &&
           value.arg0 != 64) ||
          value.arg1 > 1) {
        error = "integer type has invalid width or signedness";
        return false;
      }
      break;
    case SemTypeKind::Float:
      if ((value.arg0 != 32 && value.arg0 != 64) || value.arg1 != 0) {
        error = "float type has invalid width";
        return false;
      }
      break;
    case SemTypeKind::Nominal:
      if (value.arg0 >= nominal_types_.size() ||
          value.arg1 >= type_blocks_.size() ||
          typeBlock(TypeBlockId(value.arg1)).size() !=
              (nominalType(NominalTypeId(value.arg0)).generic.hasValue()
                   ? values_->generics()
                         .generic(
                             nominalType(NominalTypeId(value.arg0)).generic)
                         .binding_count
                   : 0U)) {
        error = "nominal type has invalid definition or arguments";
        return false;
      }
      break;
    case SemTypeKind::Array:
      if (value.arg0 >= types_.size()) {
        error = "array type has an invalid element type";
        return false;
      }
      break;
    case SemTypeKind::Function:
    case SemTypeKind::CFunctionPointer:
    case SemTypeKind::CVariadicFunctionPointer:
      if (value.arg0 >= type_blocks_.size() || value.arg1 >= types_.size()) {
        error = "function type has invalid arguments";
        return false;
      }
      for (const auto parameter : typeBlock(TypeBlockId(value.arg0))) {
        if (parameter.index >= types_.size()) {
          error = "function type has an invalid parameter type";
          return false;
        }
      }
      break;
    case SemTypeKind::AsyncFunction: {
      if (value.arg0 >= type_blocks_.size() ||
          value.arg1 >= type_blocks_.size()) {
        error = "async function type has invalid storage";
        return false;
      }
      const auto outcomes = typeBlock(TypeBlockId(value.arg1));
      if (outcomes.empty() || outcomes.size() > 2 ||
          std::ranges::any_of(outcomes, [&](TypeId outcome) {
            return outcome.index >= types_.size();
          })) {
        error = "async function type has an invalid outcome";
        return false;
      }
      for (const auto parameter : typeBlock(TypeBlockId(value.arg0))) {
        if (parameter.index >= types_.size()) {
          error = "async function type has an invalid parameter type";
          return false;
        }
      }
      break;
    }
    case SemTypeKind::CoroutineScope:
      if (value.arg0 != core::AnyId::InvalidIndex ||
          value.arg1 != core::AnyId::InvalidIndex) {
        error = "coroutine scope type has unexpected arguments";
        return false;
      }
      break;
    case SemTypeKind::CoroutineTaskCompletion:
      if (value.arg0 != core::AnyId::InvalidIndex ||
          value.arg1 != core::AnyId::InvalidIndex) {
        error = "coroutine task completion type has unexpected arguments";
        return false;
      }
      break;
    case SemTypeKind::CoroutineTaskCompletionSet:
    case SemTypeKind::CoroutineTaskSelection:
      if (value.arg0 >= types_.size() ||
          !isCompletionAggregationProvider(TypeId(value.arg0))) {
        error = "coroutine completion aggregate type has invalid provider";
        return false;
      }
      break;
    case SemTypeKind::CoroutineTask:
    case SemTypeKind::CoroutineTaskOutcome:
      if (value.arg0 >= types_.size() ||
          (value.arg1 != core::AnyId::InvalidIndex &&
           value.arg1 >= types_.size())) {
        error = "coroutine task type has invalid outcomes";
        return false;
      }
      break;
    case SemTypeKind::CoroutineChecked:
      if (value.arg0 >= types_.size() ||
          value.arg1 != core::AnyId::InvalidIndex ||
          type(TypeId(value.arg0)).kind == SemTypeKind::CoroutineChecked) {
        error = "coroutine checked type has invalid payload";
        return false;
      }
      break;
    case SemTypeKind::CallbackAdapter: {
      if (value.arg0 >= type_blocks_.size() ||
          value.arg1 != core::AnyId::InvalidIndex) {
        error = "callback adapter type has invalid storage";
        return false;
      }
      const auto fields = typeBlock(TypeBlockId(value.arg0));
      if (fields.size() != 3 ||
          type(fields[0]).kind != SemTypeKind::CFunctionPointer ||
          type(fields[1]).kind != SemTypeKind::RawPointer ||
          type(fields[2]).kind != SemTypeKind::CFunctionPointer) {
        error = "callback adapter type has invalid entry/context/release";
        return false;
      }
      const auto context = fields[1];
      if (rawPointerPointee(context) != void_type_ ||
          rawPointerPointeeConst(context)) {
        error = "callback adapter type has invalid owned context storage";
        return false;
      }
      const auto entry_parameters =
          typeBlock(TypeBlockId(type(fields[0]).arg0));
      const auto context_parameter = callbackContextParameter(fields[0]);
      if (context_parameter >= entry_parameters.size() ||
          entry_parameters[context_parameter] != context) {
        error = "callback adapter entry has an invalid context parameter";
        return false;
      }
      const auto &entry_contract = callbackContract(fields[0]);
      const auto invalid_entry_effect =
          std::ranges::any_of(entry_contract.effects, [&](const auto &effect) {
            return effect.region.parameter_index == context_parameter &&
                   effect.kind != CallableEffectKind::Read &&
                   effect.kind != CallableEffectKind::Write;
          });
      const auto invalid_entry_postcondition = std::ranges::any_of(
          entry_contract.postconditions, [&](const auto &postcondition) {
            if (postcondition.region.parameter_index != context_parameter)
              return false;
            const auto derived_from_write = std::ranges::any_of(
                entry_contract.effects, [&](const auto &effect) {
                  return effect.kind == CallableEffectKind::Write &&
                         effect.region == postcondition.region;
                });
            return !derived_from_write ||
                   postcondition.outcomes !=
                       (CallableOutcomePreserve | CallableOutcomeInitialize);
          });
      const auto invalid_entry_return = std::ranges::any_of(
          entry_contract.return_provenance, [&](const auto &source) {
            return source.region.parameter_index == context_parameter ||
                   std::ranges::any_of(
                       source.condition.clauses, [&](const auto &clause) {
                         return std::ranges::any_of(
                             clause.atoms, [&](const auto &atom) {
                               return atom.parameter_index == context_parameter;
                             });
                       });
          });
      const auto release_parameters =
          typeBlock(TypeBlockId(type(fields[2]).arg0));
      const auto &release_contract = callbackContract(fields[2]);
      const OwnershipRegion root{.parameter_index = 0};
      if (invalid_entry_effect || invalid_entry_postcondition ||
          invalid_entry_return || release_parameters.size() != 1 ||
          release_parameters[0] != context ||
          callbackContextParameter(fields[2]) != 0 ||
          TypeId(type(fields[2]).arg1) != void_type_ ||
          release_contract.effects !=
              std::vector<CallableRegionEffect>{
                  {CallableEffectKind::Take, root}} ||
          release_contract.postconditions !=
              std::vector<CallableRegionPostcondition>{
                  {root, CallableOutcomeInvalidate}} ||
          !release_contract.returns_owned ||
          !release_contract.return_provenance.empty()) {
        error = "callback adapter type has an invalid ownership contract";
        return false;
      }
      break;
    }
    case SemTypeKind::CallbackCompletion: {
      const auto protocol_id =
          foreignResourceProtocolId(TypeId(static_cast<std::uint32_t>(index)));
      if (!protocol_id.hasValue() ||
          protocol_id.index >=
              values_->generics().foreignResourceProtocolCount()) {
        error = "callback completion has no canonical resource protocol";
        return false;
      }
      const auto &protocol =
          foreignResourceProtocol(TypeId(static_cast<std::uint32_t>(index)));
      if (protocol.types.size() != typeBlock(TypeBlockId(value.arg0)).size() ||
          !protocol.facts.verify(
              static_cast<std::uint32_t>(protocol.types.size()), error) ||
          protocol.facts !=
              makeCallbackCompletionProtocol(
                  static_cast<std::uint8_t>(value.arg1),
                  static_cast<std::uint32_t>(protocol.types.size()),
                  callbackArmParameters(
                      TypeId(static_cast<std::uint32_t>(index))),
                  callbackDetachParameters(
                      TypeId(static_cast<std::uint32_t>(index))))) {
        if (error.empty())
          error = "callback completion resource protocol is inconsistent";
        return false;
      }
      if (value.arg0 >= type_blocks_.size() ||
          value.arg1 >=
              static_cast<std::uint32_t>(CallbackReleaseAuthority::Count)) {
        error = "callback completion type has invalid storage";
        return false;
      }
      const auto fields = typeBlock(TypeBlockId(value.arg0));
      if ((fields.size() != 4 && fields.size() != 5 && fields.size() != 7) ||
          type(fields[0]).kind != SemTypeKind::CallbackAdapter ||
          type(fields[1]).kind != SemTypeKind::RawPointer ||
          type(fields[2]).kind != SemTypeKind::RawPointer ||
          type(fields[3]).kind != SemTypeKind::CFunctionPointer ||
          (fields.size() >= 5 &&
           type(fields[4]).kind != SemTypeKind::CFunctionPointer) ||
          (fields.size() == 7 &&
           (type(fields[5]).kind != SemTypeKind::CFunctionPointer ||
            type(fields[6]).kind != SemTypeKind::CFunctionPointer)) ||
          rawPointerPointee(fields[1]) != void_type_ ||
          rawPointerPointeeConst(fields[1]) ||
          rawPointerPointee(fields[2]) != void_type_ ||
          rawPointerPointeeConst(fields[2])) {
        error = "callback completion type has invalid fields";
        return false;
      }
      const auto parameters = typeBlock(TypeBlockId(type(fields[3]).arg0));
      const auto &contract = callbackContract(fields[3]);
      const OwnershipRegion root{.parameter_index = 0};
      if (parameters.size() != 1 || parameters.front() != fields[2] ||
          TypeId(type(fields[3]).arg1) != void_type_ ||
          callbackContextParameter(fields[3]) != core::AnyId::InvalidIndex ||
          contract.effects !=
              std::vector<CallableRegionEffect>{
                  {CallableEffectKind::Take, root}} ||
          contract.postconditions !=
              std::vector<CallableRegionPostcondition>{
                  {root, CallableOutcomeInvalidate}} ||
          !contract.returns_owned || !contract.return_provenance.empty()) {
        error = "callback completion wait has an invalid ownership contract";
        return false;
      }
      if (fields.size() >= 5) {
        const auto poll_parameters =
            typeBlock(TypeBlockId(type(fields[4]).arg0));
        const auto &poll_contract = callbackContract(fields[4]);
        if (poll_parameters.size() != 1 ||
            poll_parameters.front() != fields[2] ||
            TypeId(type(fields[4]).arg1) != bool_type_ ||
            callbackContextParameter(fields[4]) != core::AnyId::InvalidIndex ||
            !poll_contract.effects.empty() ||
            !poll_contract.postconditions.empty() ||
            !poll_contract.returns_owned ||
            !poll_contract.return_provenance.empty()) {
          error = "callback completion poll has an invalid ownership contract";
          return false;
        }
      }
      if (fields.size() == 7) {
        const auto arm_roles =
            callbackArmParameters(TypeId(static_cast<std::uint32_t>(index)));
        const auto detach_roles =
            callbackDetachParameters(TypeId(static_cast<std::uint32_t>(index)));
        const auto arm_parameters =
            typeBlock(TypeBlockId(type(fields[5]).arg0));
        const auto detach_parameters =
            typeBlock(TypeBlockId(type(fields[6]).arg0));
        const auto callback_fields =
            typeBlock(TypeBlockId(type(fields[0]).arg0));
        const auto distinct = [](auto roles) {
          for (std::size_t i = 0; i < roles.size(); ++i)
            for (std::size_t j = i + 1; j < roles.size(); ++j)
              if (roles[i] == roles[j])
                return false;
          return true;
        };
        const auto valid_arm =
            distinct(arm_roles) && arm_parameters.size() == 4 &&
            callback_fields.size() == 3 &&
            std::ranges::all_of(
                arm_roles,
                [&](auto role) { return role < arm_parameters.size(); }) &&
            arm_parameters[arm_roles[0]] == fields[2] &&
            valid_wake_entry(arm_parameters[arm_roles[1]],
                             callback_fields[1]) &&
            arm_parameters[arm_roles[2]] == callback_fields[1] &&
            arm_parameters[arm_roles[3]] == callback_fields[2] &&
            TypeId(type(fields[5]).arg1) == bool_type_ &&
            callbackContextParameter(fields[5]) == core::AnyId::InvalidIndex &&
            callbackContract(fields[5]) == CallableOwnershipSummary{};
        const auto authority = callbackCompletionAuthority(
            TypeId(static_cast<std::uint32_t>(index)));
        const OwnershipRegion token_root{.parameter_index = detach_roles[0]};
        CallableOwnershipSummary expected_detach;
        bool valid_detach = false;
        if (authority == CallbackReleaseAuthority::Retained) {
          const OwnershipRegion userdata_root{.parameter_index =
                                                  detach_roles[1]};
          expected_detach.effects = {{CallableEffectKind::Take, token_root},
                                     {CallableEffectKind::Take, userdata_root}};
          expected_detach.postconditions = {
              {token_root, CallableOutcomeInvalidate},
              {userdata_root, CallableOutcomeInvalidate}};
          valid_detach =
              distinct(detach_roles) && detach_parameters.size() == 3 &&
              std::ranges::all_of(
                  detach_roles,
                  [&](auto role) { return role < detach_parameters.size(); }) &&
              detach_parameters[detach_roles[0]] == fields[2] &&
              detach_parameters[detach_roles[1]] == callback_fields[1] &&
              detach_parameters[detach_roles[2]] == callback_fields[2];
        } else {
          expected_detach.effects = {{CallableEffectKind::Take, token_root}};
          expected_detach.postconditions = {
              {token_root, CallableOutcomeInvalidate}};
          valid_detach = detach_parameters.size() == 1 &&
                         detach_roles[0] == 0 &&
                         detach_roles[1] == core::AnyId::InvalidIndex &&
                         detach_roles[2] == core::AnyId::InvalidIndex &&
                         detach_parameters[0] == fields[2];
        }
        expected_detach.canonicalize();
        valid_detach =
            valid_detach && TypeId(type(fields[6]).arg1) == void_type_ &&
            callbackContextParameter(fields[6]) == core::AnyId::InvalidIndex &&
            callbackContract(fields[6]) == expected_detach;
        if (!valid_arm || !valid_detach) {
          error = "callback completion wake ABI has invalid roles or contracts";
          return false;
        }
      }
      break;
    }
    case SemTypeKind::CallbackWake: {
      if (value.arg0 >= type_blocks_.size()) {
        error = "callback wake type has invalid storage";
        return false;
      }
      const auto fields = typeBlock(TypeBlockId(value.arg0));
      if (fields.size() != 1 ||
          type(fields[0]).kind != SemTypeKind::CallbackCompletion ||
          typeBlock(TypeBlockId(type(fields[0]).arg0)).size() != 7) {
        error = "callback wake type lacks an epoch-14 completion";
        return false;
      }
      break;
    }
    case SemTypeKind::CallbackRegistration: {
      const auto type_id = TypeId(static_cast<std::uint32_t>(index));
      const auto protocol_id = foreignResourceProtocolId(type_id);
      if (!protocol_id.hasValue() ||
          protocol_id.index >=
              values_->generics().foreignResourceProtocolCount()) {
        error = "callback registration has no canonical resource protocol";
        return false;
      }
      const auto &protocol = foreignResourceProtocol(type_id);
      const auto protocol_bindings = callbackRegistrationBindings(type_id);
      const auto protocol_parameters = callbackRegistrationParameters(type_id);
      if (protocol.types.size() != typeBlock(TypeBlockId(value.arg0)).size() ||
          !protocol.facts.verify(
              static_cast<std::uint32_t>(protocol.types.size()), error) ||
          protocol.facts !=
              makeCallbackRegistrationProtocol(
                  static_cast<std::uint8_t>(value.arg1), protocol_parameters[0],
                  protocol_parameters[1], protocol_parameters[2],
                  protocol_bindings,
                  static_cast<std::uint32_t>(protocol.types.size()),
                  callbackArmParameters(type_id),
                  callbackDetachParameters(type_id))) {
        if (error.empty())
          error = "callback registration resource protocol is inconsistent";
        return false;
      }
      if (value.arg0 >= type_blocks_.size() ||
          value.arg1 >=
              static_cast<std::uint32_t>(CallbackReleaseAuthority::Count) ||
          value.reserved == core::AnyId::InvalidIndex) {
        error = "callback registration type has invalid storage";
        return false;
      }
      const auto fields = typeBlock(TypeBlockId(value.arg0));
      if ((fields.size() != 5 && fields.size() != 7 && fields.size() != 8 &&
           fields.size() != 10) ||
          type(fields[0]).kind != SemTypeKind::CallbackAdapter ||
          type(fields[1]).kind != SemTypeKind::RawPointer ||
          type(fields[2]).kind != SemTypeKind::CFunctionPointer ||
          type(fields[3]).kind != SemTypeKind::CFunctionPointer ||
          type(fields[4]).kind != SemTypeKind::CFunctionPointer ||
          (fields.size() >= 7 &&
           (type(fields[5]).kind != SemTypeKind::CFunctionPointer ||
            type(fields[6]).kind != SemTypeKind::CFunctionPointer)) ||
          (fields.size() >= 8 &&
           type(fields[7]).kind != SemTypeKind::CFunctionPointer)) {
        error = "callback registration type has invalid fields";
        return false;
      }
      if (rawPointerPointee(fields[1]) != void_type_ ||
          rawPointerPointeeConst(fields[1])) {
        error = "callback registration type has invalid handle storage";
        return false;
      }
      const auto callback_fields = typeBlock(TypeBlockId(type(fields[0]).arg0));
      const auto register_parameters =
          typeBlock(TypeBlockId(type(fields[2]).arg0));
      const auto parameters = callbackRegistrationParameters(
          TypeId(static_cast<std::uint32_t>(index)));
      const auto authority = callbackRegistrationAuthority(
          TypeId(static_cast<std::uint32_t>(index)));
      const auto expected_register_parameters =
          (authority == CallbackReleaseAuthority::Transferred ? 3U : 2U) +
          callbackRegistrationBindings(
              TypeId(static_cast<std::uint32_t>(index)))
              .size();
      const auto bindings = callbackRegistrationBindings(
          TypeId(static_cast<std::uint32_t>(index)));
      const auto valid_terminal = [&](TypeId function_type) {
        const auto function_parameters =
            typeBlock(TypeBlockId(type(function_type).arg0));
        return function_parameters.size() == 1 &&
               function_parameters.front() == fields[1] &&
               TypeId(type(function_type).arg1) == void_type_ &&
               callbackContextParameter(function_type) ==
                   core::AnyId::InvalidIndex;
      };
      const OwnershipRegion async_root{.parameter_index = 0};
      const auto valid_consuming_callable =
          [&](TypeId function_type, TypeId parameter, TypeId result) {
            if (type(function_type).kind != SemTypeKind::CFunctionPointer)
              return false;
            const auto function_parameters =
                typeBlock(TypeBlockId(type(function_type).arg0));
            const auto &function_contract = callbackContract(function_type);
            return function_parameters.size() == 1 &&
                   function_parameters.front() == parameter &&
                   TypeId(type(function_type).arg1) == result &&
                   callbackContextParameter(function_type) ==
                       core::AnyId::InvalidIndex &&
                   function_contract.effects ==
                       std::vector<CallableRegionEffect>{
                           {CallableEffectKind::Take, async_root}} &&
                   function_contract.postconditions ==
                       std::vector<CallableRegionPostcondition>{
                           {async_root, CallableOutcomeInvalidate}} &&
                   function_contract.returns_owned &&
                   function_contract.return_provenance.empty();
          };
      const auto valid_poll_callable = [&](TypeId function_type,
                                           TypeId parameter) {
        if (type(function_type).kind != SemTypeKind::CFunctionPointer)
          return false;
        const auto function_parameters =
            typeBlock(TypeBlockId(type(function_type).arg0));
        const auto &function_contract = callbackContract(function_type);
        return function_parameters.size() == 1 &&
               function_parameters.front() == parameter &&
               TypeId(type(function_type).arg1) == bool_type_ &&
               callbackContextParameter(function_type) ==
                   core::AnyId::InvalidIndex &&
               function_contract.effects.empty() &&
               function_contract.postconditions.empty() &&
               function_contract.returns_owned &&
               function_contract.return_provenance.empty();
      };
      if (callback_fields.size() != 3 ||
          register_parameters.size() != expected_register_parameters ||
          parameters[0] >= register_parameters.size() ||
          parameters[1] >= register_parameters.size() ||
          parameters[0] == parameters[1] ||
          register_parameters[parameters[0]] != callback_fields[0] ||
          register_parameters[parameters[1]] != callback_fields[1]) {
        error =
            "callback registration type has invalid marked parameter positions";
        return false;
      }
      if (TypeId(type(fields[2]).arg1) != fields[1] ||
          callbackContextParameter(fields[2]) != core::AnyId::InvalidIndex) {
        error = "callback registration acquire callable is invalid";
        return false;
      }
      if (!valid_terminal(fields[3]) || !valid_terminal(fields[4])) {
        error = "callback registration terminal callable is invalid";
        return false;
      }
      if (fields.size() >= 7 &&
          !valid_consuming_callable(fields[5], fields[1], fields[1])) {
        error = "callback registration cancel-async callable is invalid";
        return false;
      }
      if (fields.size() >= 7 &&
          !valid_consuming_callable(fields[6], fields[1], void_type_)) {
        error = "callback registration wait callable is invalid";
        return false;
      }
      if (fields.size() >= 8 && !valid_poll_callable(fields[7], fields[1])) {
        error = "callback registration poll callable is invalid";
        return false;
      }
      if (fields.size() == 10 &&
          (type(fields[8]).kind != SemTypeKind::CFunctionPointer ||
           type(fields[9]).kind != SemTypeKind::CFunctionPointer)) {
        error = "callback registration wake callable is invalid";
        return false;
      }
      if ((authority == CallbackReleaseAuthority::Retained &&
           parameters[2] != core::AnyId::InvalidIndex) ||
          (authority == CallbackReleaseAuthority::Transferred &&
           (parameters[2] >= register_parameters.size() ||
            parameters[2] == parameters[0] || parameters[2] == parameters[1] ||
            register_parameters[parameters[2]] != callback_fields[2]))) {
        error = "callback registration type has invalid release authority";
        return false;
      }
      if (fields.size() == 10) {
        const auto arm_roles =
            callbackArmParameters(TypeId(static_cast<std::uint32_t>(index)));
        const auto detach_roles =
            callbackDetachParameters(TypeId(static_cast<std::uint32_t>(index)));
        const auto arm_values = typeBlock(TypeBlockId(type(fields[8]).arg0));
        const auto detach_values = typeBlock(TypeBlockId(type(fields[9]).arg0));
        const auto distinct = [](auto roles) {
          for (std::size_t i = 0; i < roles.size(); ++i)
            for (std::size_t j = i + 1; j < roles.size(); ++j)
              if (roles[i] == roles[j])
                return false;
          return true;
        };
        const auto valid_arm =
            distinct(arm_roles) && arm_values.size() == 4 &&
            std::ranges::all_of(
                arm_roles,
                [&](auto role) { return role < arm_values.size(); }) &&
            arm_values[arm_roles[0]] == fields[1] &&
            valid_wake_entry(arm_values[arm_roles[1]], callback_fields[1]) &&
            arm_values[arm_roles[2]] == callback_fields[1] &&
            arm_values[arm_roles[3]] == callback_fields[2] &&
            TypeId(type(fields[8]).arg1) == bool_type_ &&
            callbackContextParameter(fields[8]) == core::AnyId::InvalidIndex &&
            callbackContract(fields[8]) == CallableOwnershipSummary{};
        const OwnershipRegion token_root{.parameter_index = detach_roles[0]};
        CallableOwnershipSummary expected_detach;
        bool valid_detach = false;
        if (authority == CallbackReleaseAuthority::Retained) {
          const OwnershipRegion userdata_root{.parameter_index =
                                                  detach_roles[1]};
          expected_detach.effects = {{CallableEffectKind::Take, token_root},
                                     {CallableEffectKind::Take, userdata_root}};
          expected_detach.postconditions = {
              {token_root, CallableOutcomeInvalidate},
              {userdata_root, CallableOutcomeInvalidate}};
          valid_detach =
              distinct(detach_roles) && detach_values.size() == 3 &&
              std::ranges::all_of(
                  detach_roles,
                  [&](auto role) { return role < detach_values.size(); }) &&
              detach_values[detach_roles[0]] == fields[1] &&
              detach_values[detach_roles[1]] == callback_fields[1] &&
              detach_values[detach_roles[2]] == callback_fields[2];
        } else {
          expected_detach.effects = {{CallableEffectKind::Take, token_root}};
          expected_detach.postconditions = {
              {token_root, CallableOutcomeInvalidate}};
          valid_detach = detach_values.size() == 1 && detach_roles[0] == 0 &&
                         detach_roles[1] == core::AnyId::InvalidIndex &&
                         detach_roles[2] == core::AnyId::InvalidIndex &&
                         detach_values[0] == fields[1];
        }
        expected_detach.canonicalize();
        valid_detach =
            valid_detach && TypeId(type(fields[9]).arg1) == void_type_ &&
            callbackContextParameter(fields[9]) == core::AnyId::InvalidIndex &&
            callbackContract(fields[9]) == expected_detach;
        if (!valid_arm || !valid_detach) {
          error =
              "callback registration wake ABI has invalid roles or contracts";
          return false;
        }
      }
      std::vector<bool> occupied(register_parameters.size());
      occupied[parameters[0]] = true;
      occupied[parameters[1]] = true;
      if (authority == CallbackReleaseAuthority::Transferred)
        occupied[parameters[2]] = true;
      std::uint32_t previous = 0;
      bool first_binding = true;
      for (std::size_t binding_index = 0; binding_index < bindings.size();
           ++binding_index) {
        const auto &binding = bindings[binding_index];
        if (binding.name.empty() ||
            binding.parameter_index >= register_parameters.size() ||
            occupied[binding.parameter_index] ||
            (!first_binding && binding.parameter_index <= previous) ||
            std::ranges::any_of(bindings.first(binding_index),
                                [&](const auto &existing) {
                                  return existing.name == binding.name;
                                })) {
          error = "callback registration type has invalid bound parameters";
          return false;
        }
        occupied[binding.parameter_index] = true;
        previous = binding.parameter_index;
        first_binding = false;
      }
      if (std::ranges::any_of(occupied, [](bool value) { return !value; })) {
        error = "callback registration type has an unclassified parameter";
        return false;
      }
      const auto &contract = callbackContract(fields[2]);
      const auto mentions_synthesized = [&](const OwnershipRegion &region) {
        return region.parameter_index >= occupied.size() ||
               std::ranges::none_of(bindings, [&](const auto &binding) {
                 return binding.parameter_index == region.parameter_index;
               });
      };
      if (!contract.returns_owned || !contract.return_provenance.empty() ||
          std::ranges::any_of(contract.effects,
                              [&](const auto &effect) {
                                return mentions_synthesized(effect.region);
                              }) ||
          std::ranges::any_of(
              contract.postconditions, [&](const auto &postcondition) {
                return mentions_synthesized(postcondition.region);
              })) {
        error = "callback registration register contract escapes or observes a "
                "synthesized parameter";
        return false;
      }
      break;
    }
    case SemTypeKind::TypeParameter:
      if (value.arg0 >= values_->generics().genericCount() ||
          value.arg1 >= values_->generics()
                            .generic(GenericId(value.arg0))
                            .binding_count) {
        error = "type parameter has an invalid generic binding";
        return false;
      }
      break;
    case SemTypeKind::TypeProjection: {
      const auto projection = value.arg1 >> ProjectionKindShift;
      const auto projection_index = value.arg1 & ProjectionIndexMask;
      if (value.arg0 >= types_.size() ||
          projection >=
              static_cast<std::uint32_t>(SemTypeProjectionKind::Count) ||
          (projection ==
               static_cast<std::uint32_t>(SemTypeProjectionKind::Pointee) &&
           projection_index != 0) ||
          (type(TypeId(value.arg0)).kind != SemTypeKind::TypeParameter &&
           type(TypeId(value.arg0)).kind != SemTypeKind::TypeProjection)) {
        error = "type projection has an invalid dependent source";
        return false;
      }
      break;
    }
    case SemTypeKind::Reference:
      if (value.arg0 >= types_.size() ||
          ((value.arg1 & ParameterProvenanceBit) == 0 &&
           (value.arg1 & ~MutableReferenceBit) != 0)) {
        error = "reference type has invalid pointee or provenance";
        return false;
      }
      break;
    case SemTypeKind::RawPointer:
      if (value.arg0 >= types_.size() || value.arg1 > 1) {
        error = "raw pointer type has an invalid pointee";
        return false;
      }
      break;
    case SemTypeKind::CoroutineExecutor:
      if (value.arg0 != core::AnyId::InvalidIndex ||
          value.arg1 != core::AnyId::InvalidIndex) {
        error = "coroutine executor type has unexpected payload";
        return false;
      }
      break;
    case SemTypeKind::Invalid:
    case SemTypeKind::Count:
      error = "semantic type store contains an invalid type";
      return false;
    }
  }
  return true;
}
}
