#include "SemIRVerificationContext.h"

#include "chtholly/Compiler/CompilerIntrinsic.h"

#include <array>
#include <limits>
#include <ranges>
#include <string>
#include <unordered_set>

namespace chtholly::compiler::internal {

bool SemIRVerificationContext::verifyCompilerIntrinsicInstruction(
    const SemInst &value, TypeId instruction_type,
    const std::function<std::optional<std::uint8_t>(InstId)>
        &constant_enum_discriminant,
    std::string &error) const {
  const auto &insts_ = sem_ir_.insts_;
  const auto &function_refs_ = sem_ir_.function_refs_;
  const auto bool_type_ = sem_ir_.bool_type_;
  const auto void_type_ = sem_ir_.void_type_;
  const auto is_u64 = [&](TypeId candidate) {
    return candidate.hasValue() && type(candidate).kind == SemTypeKind::Integer &&
           type(candidate).arg0 == 64 && type(candidate).arg1 == 0;
  };
      const auto target = FunctionRefId(value.arg0);
      if (!target.hasValue() || target.index >= function_refs_.size()) {
        error = "compiler intrinsic call has an invalid target";
        return false;
      }
      const auto role = functionIntrinsicRole(target);
      const auto &callee_type = type(functionRef(target).local_type);
      const auto arguments = instBlock(InstBlockId(value.arg1));
      if (role == CompilerIntrinsicRole::None ||
          role >= CompilerIntrinsicRole::Count ||
          callee_type.kind != SemTypeKind::Function) {
        error = "compiler intrinsic call has no verified intrinsic role";
        return false;
      }
      const auto parameters = typeBlock(TypeBlockId(callee_type.arg0));
      if (instruction_type.index != callee_type.arg1 ||
          arguments.size() != parameters.size() ||
          parameters.size() != compilerIntrinsicParameterCount(role)) {
        error = "compiler intrinsic call does not match its signature";
        return false;
      }
      for (std::size_t argument = 0; argument < parameters.size(); ++argument) {
        if (arguments[argument].index >= insts_.size() ||
            TypeId(inst(arguments[argument]).type) != parameters[argument]) {
          error = "compiler intrinsic argument does not match its parameter";
          return false;
        }
      }
      const auto is_usize = [&](TypeId candidate) {
        return candidate.hasValue() &&
               type(candidate).kind == SemTypeKind::Integer &&
               type(candidate).arg0 == 64 && type(candidate).arg1 == 0;
      };
      const auto named_variant =
          [&](TypeId candidate,
              std::string_view requested_name) -> std::uint32_t {
        if (!candidate.hasValue() ||
            type(candidate).kind != SemTypeKind::Nominal)
          return core::AnyId::InvalidIndex;
        const auto &nominal = nominalType(NominalTypeId(type(candidate).arg0));
        if (nominal.kind != NominalKind::Enum)
          return core::AnyId::InvalidIndex;
        for (std::uint32_t index = 0; index < nominal.variants.size(); ++index)
          if (identifier(name(nominal.variants[index].name).text) ==
              requested_name)
            return index;
        return core::AnyId::InvalidIndex;
      };
      const auto option_payload = [&](TypeId candidate) -> TypeId {
        const auto some = named_variant(candidate, "Some");
        const auto none = named_variant(candidate, "None");
        if (some == core::AnyId::InvalidIndex ||
            none == core::AnyId::InvalidIndex)
          return TypeId::invalid();
        const auto &nominal = nominalType(NominalTypeId(type(candidate).arg0));
        if (nominal.variants[some].fields.size() != 1 ||
            !nominal.variants[none].fields.empty())
          return TypeId::invalid();
        return enumPayloadFieldType(candidate, some, 0);
      };
      if (isOptionCompilerIntrinsic(role)) {
        const auto receiver = parameters[0];
        const auto by_reference = type(receiver).kind == SemTypeKind::Reference;
        const auto option =
            by_reference ? referencePointee(receiver) : receiver;
        const auto payload = option_payload(option);
        if (!payload.hasValue()) {
          error = "Option intrinsic receiver has an invalid enum shape";
          return false;
        }
        if (role == CompilerIntrinsicRole::OptionUnwrap) {
          if (by_reference || instruction_type != payload) {
            error = "Option unwrap has an invalid consuming signature";
            return false;
          }
        } else if (role == CompilerIntrinsicRole::OptionAsRef ||
                   role == CompilerIntrinsicRole::OptionAsMut) {
          const auto expected_mutability =
              role == CompilerIntrinsicRole::OptionAsRef
                  ? SemReferenceMutability::ReadOnly
                  : SemReferenceMutability::Mutable;
          const auto result_payload = option_payload(instruction_type);
          if (!by_reference || !result_payload.hasValue() ||
              referenceMutability(receiver) != expected_mutability ||
              type(result_payload).kind != SemTypeKind::Reference ||
              referencePointee(result_payload) != payload ||
              referenceMutability(result_payload) != expected_mutability) {
            error = "Option projection has an invalid receiver or result";
            return false;
          }
        } else if (!by_reference ||
                   referenceMutability(receiver) !=
                       SemReferenceMutability::ReadOnly ||
                   instruction_type != bool_type_) {
          error = "Option query has an invalid receiver or result type";
          return false;
        }
      }
      if (isChannelTransitionCompilerIntrinsic(role)) {
        const auto channel_type = parameters.empty()
                                      ? TypeId::invalid()
                                      : parameters.front();
        const auto channel_kind = channel_type.hasValue()
                                      ? type(channel_type).kind
                                      : SemTypeKind::Invalid;
        if (channel_kind != SemTypeKind::Nominal) {
          error = "channel intrinsic requires a nominal channel operand";
          return false;
        }
        const auto payload_valid = [&](TypeId candidate) {
          if (!candidate.hasValue())
            return false;
          const auto kind = type(candidate).kind;
          return kind != SemTypeKind::Reference &&
                 kind != SemTypeKind::Slice && kind != SemTypeKind::RawPointer &&
                 kind != SemTypeKind::Function &&
                 kind != SemTypeKind::CFunctionPointer &&
                 kind != SemTypeKind::TypeParameter &&
                 kind != SemTypeKind::TypeProjection;
        };
        if (role == CompilerIntrinsicRole::ChannelSendPrepare ||
            role == CompilerIntrinsicRole::ChannelSendCommit ||
            role == CompilerIntrinsicRole::ChannelReceiveCommit) {
          if (parameters.size() != 2 || !payload_valid(parameters[1])) {
            error = "channel intrinsic has an invalid payload operand";
            return false;
          }
        }
        if (instruction_type != void_type_) {
          error = "channel transition intrinsic must return void";
          return false;
        }
      }
      if (role == CompilerIntrinsicRole::ChannelMake ||
          role == CompilerIntrinsicRole::ChannelInit ||
          role == CompilerIntrinsicRole::ChannelSend ||
          role == CompilerIntrinsicRole::ChannelReceive ||
          role == CompilerIntrinsicRole::ChannelClose ||
          role == CompilerIntrinsicRole::ChannelDrop) {
        const auto channel_parameter =
            parameters.empty() ? TypeId::invalid() : parameters.front();
        const auto channel_type =
            channel_parameter.hasValue() &&
                    type(channel_parameter).kind == SemTypeKind::Reference
                ? referencePointee(channel_parameter)
                : channel_parameter;
        if (!channel_type.hasValue() ||
            type(channel_type).kind != SemTypeKind::Nominal) {
          error = "channel intrinsic requires a concrete Channel<T> receiver";
          return false;
        }
        const auto channel_arguments = typeBlock(TypeBlockId(type(channel_type).arg1));
        if (channel_arguments.size() != 1) {
          error = "channel intrinsic requires one payload type";
          return false;
        }
        const auto payload_type = channel_arguments.front();
        const auto payload_kind = type(payload_type).kind;
        if (payload_kind == SemTypeKind::Reference || payload_kind == SemTypeKind::Slice ||
            payload_kind == SemTypeKind::RawPointer || payload_kind == SemTypeKind::Void ||
            payload_kind == SemTypeKind::Never || payload_kind == SemTypeKind::TypeParameter ||
            payload_kind == SemTypeKind::TypeProjection) {
          error = "channel intrinsic has an invalid payload type";
          return false;
        }
        if (role == CompilerIntrinsicRole::ChannelSend && parameters[1] != payload_type) {
          error = "channel send payload disagrees with Channel<T>";
          return false;
        }
        if (role == CompilerIntrinsicRole::ChannelClose ||
            role == CompilerIntrinsicRole::ChannelInit ||
            role == CompilerIntrinsicRole::ChannelSend ||
            role == CompilerIntrinsicRole::ChannelReceive) {
          const auto shape = canonicalResultShape(instruction_type);
          if (!shape || shape->success !=
              (role == CompilerIntrinsicRole::ChannelReceive ? payload_type : void_type_)) {
            error = "channel intrinsic has an invalid success result";
            return false;
          }
          auto error_type = shape->error;
          if (role == CompilerIntrinsicRole::ChannelSend) {
            if (type(error_type).kind != SemTypeKind::Nominal) {
              error = "channel send requires SendError<T>";
              return false;
            }
            const auto &send_error = nominalType(NominalTypeId(type(error_type).arg0));
            const auto *entity = importIRs().tryGetEntity(send_error.canonical_entity);
            if (!entity || identifier(entity->package_name) != "std" ||
                identifier(entity->module_name) != "std::typed_channel" ||
                identifier(entity->name) != "SendError" || send_error.fields.size() != 2 ||
                nominalFieldType(error_type, 1) != payload_type) {
              error = "channel send must return its owned payload in SendError<T>";
              return false;
            }
            error_type = nominalFieldType(error_type, 0);
          }
          if (type(error_type).kind != SemTypeKind::Nominal) {
            error = "channel error code has invalid type";
            return false;
          }
          const auto &error_nominal = nominalType(NominalTypeId(type(error_type).arg0));
          const auto *error_entity = importIRs().tryGetEntity(error_nominal.canonical_entity);
          if (!error_entity || identifier(error_entity->package_name) != "std" ||
              identifier(error_entity->module_name) != "std::error" ||
              identifier(error_entity->name) != "ErrorCode") {
            error = "channel intrinsic result must use std::error::ErrorCode";
            return false;
          }
        }
        if (role == CompilerIntrinsicRole::ChannelDrop &&
            instruction_type != void_type_) {
          error = "channel drop intrinsic must return void";
          return false;
        }
      }
      if (role == CompilerIntrinsicRole::TextAsBytes) {
        const auto result = type(instruction_type);
        const auto source = type(parameters[0]);
        const auto valid_element = [&](TypeId element) {
          if (!element.hasValue())
            return false;
          const auto &value = type(element);
          return value.kind == SemTypeKind::Integer && value.arg0 == 8 &&
                 value.arg1 == 0;
        };
        if (source.kind != SemTypeKind::Reference ||
            referenceMutability(parameters[0]) !=
                SemReferenceMutability::ReadOnly ||
            type(referencePointee(parameters[0])).kind != SemTypeKind::String ||
            result.kind != SemTypeKind::Slice || result.arg1 != 0 ||
            !valid_element(TypeId(result.arg0))) {
          error = "text.as-bytes requires string to const slice<u8>";
          return false;
        }
      }
      if (role == CompilerIntrinsicRole::TextSliceData ||
          role == CompilerIntrinsicRole::TextSliceDataMut) {
        const auto source = type(parameters[0]);
        const auto result = type(instruction_type);
        const auto element_valid = [&](TypeId element) {
          if (!element.hasValue())
            return false;
          const auto &value = type(element);
          return value.kind == SemTypeKind::Integer && value.arg0 == 8 &&
                 value.arg1 == 0;
        };
        if (source.kind != SemTypeKind::Slice ||
            (role == CompilerIntrinsicRole::TextSliceData &&
             source.arg1 != 0) ||
            (role == CompilerIntrinsicRole::TextSliceDataMut &&
             source.arg1 == 0) ||
            !element_valid(TypeId(source.arg0)) ||
            result.kind != SemTypeKind::RawPointer ||
            type(TypeId(result.arg0)).kind != SemTypeKind::Void ||
            (role == CompilerIntrinsicRole::TextSliceData && result.arg1 == 0) ||
            (role == CompilerIntrinsicRole::TextSliceDataMut && result.arg1 != 0)) {
          error = "text slice data intrinsic has an invalid signature";
          return false;
        }
      }
      if (isVecCompilerIntrinsic(role)) {
        const auto nominal_name = [&](TypeId candidate) -> std::string_view {
          if (!candidate.hasValue() ||
              type(candidate).kind != SemTypeKind::Nominal)
            return {};
          const auto &nominal =
              nominalType(NominalTypeId(type(candidate).arg0));
          return nominal.name.hasValue() ? identifier(name(nominal.name).text)
                                         : std::string_view{};
        };
        const auto is_usize_type = [&](TypeId candidate) {
          return candidate.hasValue() &&
                 type(candidate).kind == SemTypeKind::Integer &&
                 type(candidate).arg0 == 64 && type(candidate).arg1 == 0;
        };
        const auto validate_iterator = [&](TypeId iterator_type,
                                           TypeId expected_vec,
                                           bool mutable_iterator) {
          if (nominal_name(iterator_type) !=
                  (mutable_iterator ? "VecMutIterator" : "VecIterator") ||
              type(iterator_type).kind != SemTypeKind::Nominal)
            return false;
          const auto &iterator =
              nominalType(NominalTypeId(type(iterator_type).arg0));
          if (type(expected_vec).kind != SemTypeKind::Nominal ||
              iterator.kind != NominalKind::Struct ||
              iterator.fields.size() != 2 ||
              nominalType(NominalTypeId(type(expected_vec).arg0))
                      .fields.size() != 3 ||
              nominalFieldType(iterator_type, 1) !=
                  nominalFieldType(expected_vec, 1))
            return false;
          const auto owner = nominalFieldType(iterator_type, 0);
          if (type(owner).kind != SemTypeKind::Reference ||
              referencePointee(owner) != expected_vec ||
              referenceMutability(owner) !=
                  (mutable_iterator ? SemReferenceMutability::Mutable
                                    : SemReferenceMutability::ReadOnly) ||
              !is_usize_type(nominalFieldType(iterator_type, 1)))
            return false;
          return true;
        };
        const auto validate_step = [&](TypeId step_type, TypeId iterator_type,
                                       TypeId, TypeId element,
                                       bool mutable_iterator) {
          if (type(step_type).kind != SemTypeKind::Nominal ||
              nominal_name(step_type) != "IterationStep")
            return false;
          const auto &step = nominalType(NominalTypeId(type(step_type).arg0));
          if (step.kind != NominalKind::Enum || step.variants.size() != 2)
            return false;
          std::uint32_t item = core::AnyId::InvalidIndex;
          std::uint32_t done = core::AnyId::InvalidIndex;
          for (std::uint32_t index = 0; index < step.variants.size(); ++index) {
            const auto variant_name =
                identifier(name(step.variants[index].name).text);
            if (variant_name == "Item")
              item = index;
            if (variant_name == "Done")
              done = index;
          }
          if (item == core::AnyId::InvalidIndex ||
              done == core::AnyId::InvalidIndex ||
              step.variants[item].fields.size() != 2 ||
              !step.variants[done].fields.empty())
            return false;
          const auto value_type = enumPayloadFieldType(step_type, item, 0);
          const auto next_type = enumPayloadFieldType(step_type, item, 1);
          return type(value_type).kind == SemTypeKind::Reference &&
                 referencePointee(value_type) == element &&
                 referenceMutability(value_type) ==
                     (mutable_iterator ? SemReferenceMutability::Mutable
                                       : SemReferenceMutability::ReadOnly) &&
                 next_type == iterator_type;
        };
        if (role == CompilerIntrinsicRole::VecIterNext ||
            role == CompilerIntrinsicRole::VecIterMutNext) {
          if (parameters.size() != 1 ||
              type(parameters[0]).kind != SemTypeKind::Nominal) {
            error = "Vec iterator next has an invalid receiver";
            return false;
          }
          const bool mutable_iterator =
              role == CompilerIntrinsicRole::VecIterMutNext;
          const auto iterator_type = parameters[0];
          const auto &iterator =
              nominalType(NominalTypeId(type(iterator_type).arg0));
          if (iterator.kind != NominalKind::Struct ||
              iterator.fields.size() != 2) {
            error = "Vec iterator next has an invalid receiver shape";
            return false;
          }
          const auto owner = nominalFieldType(iterator_type, 0);
          if (type(owner).kind != SemTypeKind::Reference) {
            error = "Vec iterator next has no borrowed owner";
            return false;
          }
          const auto vec_type = referencePointee(owner);
          if (type(vec_type).kind != SemTypeKind::Nominal ||
              nominalType(NominalTypeId(type(vec_type).arg0)).kind !=
                  NominalKind::Struct ||
              nominalType(NominalTypeId(type(vec_type).arg0)).fields.size() !=
                  3 ||
              type(nominalFieldType(vec_type, 0)).kind !=
                  SemTypeKind::RawPointer) {
            error = "Vec iterator next has an invalid concrete Vec";
            return false;
          }
          if (!validate_iterator(iterator_type, vec_type, mutable_iterator)) {
            error = "Vec iterator next has an invalid concrete iterator";
            return false;
          }
          if (!validate_step(instruction_type, iterator_type, vec_type,
                             rawPointerPointee(nominalFieldType(vec_type, 0)),
                             mutable_iterator)) {
            error = "Vec iterator next has an invalid concrete step";
            return false;
          }
        } else {
          TypeId vec_type = instruction_type;
          if (role != CompilerIntrinsicRole::VecInit) {
            if (parameters.empty() ||
                type(parameters[0]).kind != SemTypeKind::Reference) {
              error = "Vec intrinsic has no reference receiver";
              return false;
            }
            vec_type = referencePointee(parameters[0]);
          }
          if (type(vec_type).kind != SemTypeKind::Nominal) {
            error = "Vec intrinsic has no nominal Vec type";
            return false;
          }
          const auto &vec = nominalType(NominalTypeId(type(vec_type).arg0));
          if (vec.kind != NominalKind::Struct || vec.fields.size() != 3) {
            error = "Vec intrinsic receiver has an invalid object shape";
            return false;
          }
          const auto data = nominalFieldType(vec_type, 0);
          const auto length = nominalFieldType(vec_type, 1);
          const auto capacity = nominalFieldType(vec_type, 2);
          if (type(data).kind != SemTypeKind::RawPointer || !is_usize(length) ||
              capacity != length) {
            error = "Vec intrinsic receiver has an invalid field layout";
            return false;
          }
          const auto element = rawPointerPointee(data);
          const auto element_representation = typeRepresentation(element);
          if (element_representation.move == MoveReprKind::Unavailable) {
            error = "Vec intrinsic element type is not movable";
            return false;
          }
          const auto read_only = role == CompilerIntrinsicRole::VecLen ||
                                 role == CompilerIntrinsicRole::VecCapacity ||
                                 role == CompilerIntrinsicRole::VecAt ||
                                 role == CompilerIntrinsicRole::VecIter;
          if (role != CompilerIntrinsicRole::VecInit &&
              referenceMutability(parameters[0]) !=
                  (read_only ? SemReferenceMutability::ReadOnly
                             : SemReferenceMutability::Mutable)) {
            error = "Vec intrinsic receiver has invalid mutability";
            return false;
          }
          if (role == CompilerIntrinsicRole::VecInit) {
            if (instruction_type != vec_type) {
              error = "Vec init result does not match its Vec type";
              return false;
            }
          } else if (role == CompilerIntrinsicRole::VecLen ||
                     role == CompilerIntrinsicRole::VecCapacity) {
            if (instruction_type != length) {
              error = "Vec size query has an invalid result type";
              return false;
            }
          } else if (role == CompilerIntrinsicRole::VecReserve) {
            if (instruction_type != void_type_ || parameters[1] != length) {
              error = "Vec reserve has an invalid signature";
              return false;
            }
          } else if (role == CompilerIntrinsicRole::VecPush) {
            if (instruction_type != void_type_ || parameters[1] != element) {
              error = "Vec push has an invalid element or result type";
              return false;
            }
          } else if (role == CompilerIntrinsicRole::VecAt ||
                     role == CompilerIntrinsicRole::VecAtMut) {
            const auto result_mutability =
                role == CompilerIntrinsicRole::VecAt
                    ? SemReferenceMutability::ReadOnly
                    : SemReferenceMutability::Mutable;
            if (parameters[1] != length ||
                type(instruction_type).kind != SemTypeKind::Reference ||
                referencePointee(instruction_type) != element ||
                referenceMutability(instruction_type) != result_mutability) {
              error = "Vec element access has an invalid signature";
              return false;
            }
          } else if (role == CompilerIntrinsicRole::VecIter ||
                     role == CompilerIntrinsicRole::VecIterMut) {
            const bool mutable_iterator =
                role == CompilerIntrinsicRole::VecIterMut;
            if (!validate_iterator(instruction_type, vec_type,
                                   mutable_iterator)) {
              error = "Vec iterator result has an invalid borrowed carrier";
              return false;
            }
          } else if (role == CompilerIntrinsicRole::VecPop ||
                     role == CompilerIntrinsicRole::VecRemove) {
            if ((role == CompilerIntrinsicRole::VecRemove &&
                 parameters[1] != length) ||
                option_payload(instruction_type) != element) {
              error = "Vec removal result is not Option<T>";
              return false;
            }
          } else if (instruction_type != void_type_) {
            error = "Vec mutation or drop has a non-void result";
            return false;
          }
        }
      }
      if (isContainerCompilerIntrinsic(role)) {
        const bool hash_map = isHashMapCompilerIntrinsic(role);
        const bool make = role == CompilerIntrinsicRole::HashMapMake ||
                          role == CompilerIntrinsicRole::HashSetMake;
        const auto nominal_name = [&](TypeId candidate) -> std::string_view {
          if (!candidate.hasValue() ||
              type(candidate).kind != SemTypeKind::Nominal)
            return {};
          const auto &nominal = nominalType(NominalTypeId(type(candidate).arg0));
          return nominal.name.hasValue() ? identifier(name(nominal.name).text)
                                         : std::string_view{};
        };
        const auto is_usize_type = [&](TypeId candidate) {
          return candidate.hasValue() &&
                 type(candidate).kind == SemTypeKind::Integer &&
                 type(candidate).arg0 == 64 && type(candidate).arg1 == 0;
        };
        const auto option_payload_for = [&](TypeId candidate) {
          return option_payload(candidate);
        };
        const auto result_payloads = [&](TypeId candidate,
                                         std::string_view expected_ok,
                                         std::string_view expected_err) {
          if (!candidate.hasValue() || type(candidate).kind != SemTypeKind::Nominal)
            return false;
          const auto &result = nominalType(NominalTypeId(type(candidate).arg0));
          if (result.kind != NominalKind::Enum)
            return false;
          std::uint32_t ok = core::AnyId::InvalidIndex;
          std::uint32_t err = core::AnyId::InvalidIndex;
          for (std::uint32_t index = 0; index < result.variants.size(); ++index) {
            const auto variant = identifier(name(result.variants[index].name).text);
            if (variant == expected_ok)
              ok = index;
            if (variant == expected_err)
              err = index;
          }
          return ok != core::AnyId::InvalidIndex &&
                 err != core::AnyId::InvalidIndex &&
                 result.variants[ok].fields.size() == 1 &&
                 result.variants[err].fields.size() == 1;
        };
        const auto validate_container = [&](TypeId candidate,
                                            std::size_t argument_count,
                                            std::string_view expected_name) {
          if (!candidate.hasValue() || type(candidate).kind != SemTypeKind::Nominal ||
              nominal_name(candidate) != expected_name)
            return std::optional<std::vector<TypeId>>{};
          const auto &nominal = nominalType(NominalTypeId(type(candidate).arg0));
          if (nominal.kind != NominalKind::Struct || nominal.fields.size() !=
                  (hash_map ? 6U : 5U))
            return std::optional<std::vector<TypeId>>{};
          const auto type_arguments = typeBlock(TypeBlockId(type(candidate).arg1));
          if (type_arguments.size() != argument_count)
            return std::optional<std::vector<TypeId>>{};
          const auto storage = nominalFieldType(candidate, 0);
          if (type(storage).kind != SemTypeKind::RawPointer ||
              type(rawPointerPointee(storage)).kind != SemTypeKind::Integer ||
              type(rawPointerPointee(storage)).arg0 != 8 ||
              type(rawPointerPointee(storage)).arg1 != 0 ||
              rawPointerPointeeConst(storage))
            return std::optional<std::vector<TypeId>>{};
          if (!is_usize_type(nominalFieldType(candidate, 1)) ||
              !is_usize_type(nominalFieldType(candidate, 2)) ||
              (hash_map && !is_usize_type(nominalFieldType(candidate, 3))) ||
              type(nominalFieldType(candidate, hash_map ? 4 : 3)).kind !=
                  SemTypeKind::Integer ||
              type(nominalFieldType(candidate, hash_map ? 5 : 4)).kind !=
                  SemTypeKind::Integer)
            return std::optional<std::vector<TypeId>>{};
          return std::optional<std::vector<TypeId>>{
              std::vector<TypeId>(type_arguments.begin(), type_arguments.end())};
        };
        const auto receiver = parameters.empty() ? TypeId::invalid() : parameters[0];
        const auto receiver_object =
            receiver.hasValue() && type(receiver).kind == SemTypeKind::Reference
                ? referencePointee(receiver)
                : receiver;
        const auto expected_name = hash_map ? "HashMap" : "HashSet";
        if (make) {
          const auto container_arguments = validate_container(
              instruction_type, hash_map ? 2U : 1U, expected_name);
          if (!container_arguments || !parameters.empty()) {
            error = "container make has an invalid signature";
            return false;
          }
          for (const auto argument : *container_arguments)
            if (typeRepresentation(argument).move == MoveReprKind::Unavailable) {
              error = "container element type is not movable";
              return false;
            }
        } else {
          const auto container_arguments = validate_container(
              receiver_object, hash_map ? 2U : 1U, expected_name);
          if (!container_arguments || receiver.hasValue() &&
                                type(receiver).kind != SemTypeKind::Reference) {
            error = "container intrinsic has an invalid receiver";
            return false;
          }
          const bool read_only =
              role == CompilerIntrinsicRole::HashMapLen ||
              role == CompilerIntrinsicRole::HashMapCapacity ||
              role == CompilerIntrinsicRole::HashMapIsEmpty ||
              role == CompilerIntrinsicRole::HashMapContains ||
              role == CompilerIntrinsicRole::HashMapGet ||
              role == CompilerIntrinsicRole::HashSetLen ||
              role == CompilerIntrinsicRole::HashSetCapacity ||
              role == CompilerIntrinsicRole::HashSetIsEmpty ||
              role == CompilerIntrinsicRole::HashSetContains;
          const auto expected_mutability =
              read_only ? SemReferenceMutability::ReadOnly
                        : SemReferenceMutability::Mutable;
          if (referenceMutability(receiver) != expected_mutability) {
            error = "container intrinsic has invalid receiver mutability";
            return false;
          }
          const auto key_or_value = parameters.size() > 1 ? parameters[1]
                                                          : TypeId::invalid();
          const auto key_is_ref = key_or_value.hasValue() &&
                                  type(key_or_value).kind == SemTypeKind::Reference;
          const auto key_pointee = key_is_ref ? referencePointee(key_or_value)
                                              : key_or_value;
          const auto require_key = [&](bool mutable_value) {
            return parameters.size() == 2 && key_is_ref &&
                   referenceMutability(key_or_value) ==
                       SemReferenceMutability::ReadOnly &&
                   key_pointee == (*container_arguments)[0] &&
                   (!mutable_value || referenceMutability(receiver) ==
                                           SemReferenceMutability::Mutable);
          };
          if (role == CompilerIntrinsicRole::HashMapLen ||
              role == CompilerIntrinsicRole::HashMapCapacity ||
              role == CompilerIntrinsicRole::HashMapIsEmpty ||
              role == CompilerIntrinsicRole::HashSetLen ||
              role == CompilerIntrinsicRole::HashSetCapacity ||
              role == CompilerIntrinsicRole::HashSetIsEmpty) {
            const bool empty_query =
                role == CompilerIntrinsicRole::HashMapIsEmpty ||
                role == CompilerIntrinsicRole::HashSetIsEmpty;
            if (parameters.size() != 1 ||
                (empty_query ? instruction_type != bool_type_
                             : !is_usize_type(instruction_type))) {
              error = "container query has an invalid signature";
              return false;
            }
          } else if (role == CompilerIntrinsicRole::HashMapContains ||
                     role == CompilerIntrinsicRole::HashSetContains) {
            if (!require_key(false) || instruction_type != bool_type_) {
              error = "container contains has an invalid signature";
              return false;
            }
          } else if (role == CompilerIntrinsicRole::HashMapGet ||
                     role == CompilerIntrinsicRole::HashMapGetMut) {
            const bool mutable_result = role == CompilerIntrinsicRole::HashMapGetMut;
            const auto payload = option_payload_for(instruction_type);
            if (!require_key(mutable_result) || !payload.hasValue() ||
                type(payload).kind != SemTypeKind::Reference ||
                referencePointee(payload) != (*container_arguments)[1] ||
                referenceMutability(payload) !=
                    (mutable_result ? SemReferenceMutability::Mutable
                             : SemReferenceMutability::ReadOnly)) {
              error = "HashMap borrow result has an invalid provenance";
              return false;
            }
          } else if (role == CompilerIntrinsicRole::HashMapRemove) {
            if (!require_key(true) || option_payload_for(instruction_type) !=
                                          (*container_arguments)[1]) {
              error = "HashMap remove has an invalid signature";
              return false;
            }
          } else if (role == CompilerIntrinsicRole::HashMapInsert) {
            if (parameters.size() != 3 || type(parameters[1]).kind == SemTypeKind::Reference ||
                type(parameters[2]).kind == SemTypeKind::Reference ||
                parameters[1] != (*container_arguments)[0] || parameters[2] != (*container_arguments)[1] ||
                !result_payloads(instruction_type, "Ok", "Err") ||
                named_variant(instruction_type, "Ok") == core::AnyId::InvalidIndex ||
                option_payload_for(enumPayloadFieldType(
                    instruction_type, named_variant(instruction_type, "Ok"), 0)) !=
                    (*container_arguments)[1]) {
              error = "HashMap insert has an invalid signature";
              return false;
            }
          } else if (role == CompilerIntrinsicRole::HashMapReserve ||
                     role == CompilerIntrinsicRole::HashSetReserve) {
            if (parameters.size() != 2 || !is_usize_type(parameters[1]) ||
                !result_payloads(instruction_type, "Ok", "Err") ||
                named_variant(instruction_type, "Ok") == core::AnyId::InvalidIndex ||
                enumPayloadFieldType(instruction_type,
                                     named_variant(instruction_type, "Ok"), 0) !=
                    void_type_) {
              error = "container reserve has an invalid signature";
              return false;
            }
          } else if (role == CompilerIntrinsicRole::HashSetInsert) {
            if (parameters.size() != 2 || parameters[1] != (*container_arguments)[0] ||
                !result_payloads(instruction_type, "Ok", "Err") ||
                named_variant(instruction_type, "Ok") == core::AnyId::InvalidIndex ||
                enumPayloadFieldType(instruction_type,
                                     named_variant(instruction_type, "Ok"), 0) !=
                    bool_type_) {
              error = "HashSet insert has an invalid signature";
              return false;
            }
          } else if (role == CompilerIntrinsicRole::HashSetRemove) {
            if (!require_key(true) || instruction_type != bool_type_) {
              error = "HashSet remove has an invalid signature";
              return false;
            }
          } else if (role == CompilerIntrinsicRole::HashMapClear ||
                     role == CompilerIntrinsicRole::HashSetClear ||
                     role == CompilerIntrinsicRole::HashMapDrop ||
                     role == CompilerIntrinsicRole::HashSetDrop) {
            if (parameters.size() != 1 || instruction_type != void_type_) {
              error = "container mutation has an invalid signature";
              return false;
            }
          }
        }
      }
      TypeId scalar_type;
      TypeId atomic_type;
      if (role == CompilerIntrinsicRole::AtomicInit) {
        scalar_type = parameters[0];
        atomic_type = instruction_type;
      } else if (isAtomicCompilerIntrinsic(role)) {
        if (type(parameters[0]).kind != SemTypeKind::Reference ||
            referenceMutability(parameters[0]) !=
                SemReferenceMutability::ReadOnly) {
          error = "atomic intrinsic receiver is not a read-only reference";
          return false;
        }
        atomic_type = referencePointee(parameters[0]);
        if (type(atomic_type).kind == SemTypeKind::Nominal) {
          const auto &nominal =
              nominalType(NominalTypeId(type(atomic_type).arg0));
          if (nominal.fields.size() == 1)
            scalar_type = nominalFieldType(atomic_type, 0);
        }
      } else if (role == CompilerIntrinsicRole::VolatileLoad) {
        scalar_type = instruction_type;
      } else if (role == CompilerIntrinsicRole::VolatileStore) {
        scalar_type = parameters[1];
      } else if (role == CompilerIntrinsicRole::WrappingMul) {
        scalar_type = parameters[0];
      } else if (role == CompilerIntrinsicRole::FloatHash ||
                 role == CompilerIntrinsicRole::FloatEqual) {
        scalar_type = parameters[0];
      } else if (role == CompilerIntrinsicRole::PointerHash ||
                 role == CompilerIntrinsicRole::PointerEqual) {
        scalar_type = parameters[0];
      }
      const auto scalar_kind = scalar_type.hasValue() ? type(scalar_type).kind
                                                      : SemTypeKind::Invalid;
      const auto dependent_scalar = scalar_kind == SemTypeKind::TypeParameter ||
                                    scalar_kind == SemTypeKind::TypeProjection;
      if (!dependent_scalar && ((compilerIntrinsicRequiresInteger(role) &&
                                 scalar_kind != SemTypeKind::Integer) ||
                                (isAtomicCompilerIntrinsic(role) &&
                                 scalar_kind != SemTypeKind::Integer &&
                                 scalar_kind != SemTypeKind::Bool))) {
        error = "compiler intrinsic has an unsupported scalar type";
        return false;
      }
      if (isAtomicCompilerIntrinsic(role)) {
        if (type(atomic_type).kind != SemTypeKind::Nominal) {
          error = "atomic intrinsic has no concrete atomic object type";
          return false;
        }
        const auto &atomic_nominal =
            nominalType(NominalTypeId(type(atomic_type).arg0));
        if (atomic_nominal.fields.size() != 1 ||
            nominalFieldType(atomic_type, 0) != scalar_type) {
          error = "atomic intrinsic object does not wrap its scalar type";
          return false;
        }
        const auto copy =
            static_cast<SemLifecycleCopyPolicy>(atomic_nominal.lifecycle_copy);
        const auto move =
            static_cast<SemLifecycleMovePolicy>(atomic_nominal.lifecycle_move);
        const auto drop =
            static_cast<SemLifecycleDropPolicy>(atomic_nominal.lifecycle_drop);
        if (copy != SemLifecycleCopyPolicy::Delete ||
            move != SemLifecycleMovePolicy::Default ||
            drop != SemLifecycleDropPolicy::Default) {
          error = "atomic intrinsic object has invalid lifecycle policies";
          return false;
        }
      }
      if (role == CompilerIntrinsicRole::AtomicLoad) {
        if (instruction_type != scalar_type) {
          error = "atomic load result does not match its scalar type";
          return false;
        }
      } else if (role == CompilerIntrinsicRole::AtomicStore) {
        if (instruction_type != void_type_ || parameters[1] != scalar_type) {
          error = "atomic store has inconsistent value or result types";
          return false;
        }
      } else if (role == CompilerIntrinsicRole::AtomicExchange ||
                 isAtomicFetchCompilerIntrinsic(role)) {
        if (instruction_type != scalar_type || parameters[1] != scalar_type) {
          error = "atomic read-modify-write has inconsistent scalar types";
          return false;
        }
      } else if (role == CompilerIntrinsicRole::AtomicCompareExchange) {
        if (parameters[1] != scalar_type || parameters[2] != scalar_type ||
            type(instruction_type).kind != SemTypeKind::Nominal) {
          error = "atomic compare-exchange has inconsistent scalar types";
          return false;
        }
        const auto &result_nominal =
            nominalType(NominalTypeId(type(instruction_type).arg0));
        if (result_nominal.fields.size() != 2 ||
            nominalFieldType(instruction_type, 0) != scalar_type ||
            nominalFieldType(instruction_type, 1) != bool_type_) {
          error = "atomic compare-exchange has an invalid result shape";
          return false;
        }
      } else if (role == CompilerIntrinsicRole::VolatileLoad) {
        if (type(parameters[0]).kind != SemTypeKind::RawPointer ||
            !rawPointerPointeeConst(parameters[0]) ||
            rawPointerPointee(parameters[0]) != scalar_type) {
          error = "volatile load has an invalid pointer type";
          return false;
        }
      } else if (role == CompilerIntrinsicRole::VolatileStore) {
        if (instruction_type != void_type_ ||
            type(parameters[0]).kind != SemTypeKind::RawPointer ||
            rawPointerPointeeConst(parameters[0]) ||
            rawPointerPointee(parameters[0]) != scalar_type) {
          error = "volatile store has an invalid pointer type";
          return false;
        }
      } else if (role == CompilerIntrinsicRole::WrappingMul) {
        if (parameters.size() != 2 || instruction_type != parameters[0] ||
            parameters[1] != parameters[0] ||
            type(parameters[0]).kind != SemTypeKind::Integer) {
          error = "wrapping multiplication requires two equal integer operands";
          return false;
        }
      } else if (role == CompilerIntrinsicRole::FloatHash) {
        if (parameters.size() != 2 || !is_u64(instruction_type) ||
            type(parameters[0]).kind != SemTypeKind::Reference ||
            referenceMutability(parameters[0]) !=
                SemReferenceMutability::ReadOnly ||
            type(referencePointee(parameters[0])).kind != SemTypeKind::Float ||
            !is_u64(parameters[1])) {
          error = "float hash requires const float reference and u64 seed";
          return false;
        }
      } else if (role == CompilerIntrinsicRole::FloatEqual) {
        if (parameters.size() != 2 || instruction_type != bool_type_ ||
            type(parameters[0]).kind != SemTypeKind::Reference ||
            type(parameters[1]).kind != SemTypeKind::Reference ||
            referenceMutability(parameters[0]) !=
                SemReferenceMutability::ReadOnly ||
            referenceMutability(parameters[1]) !=
                SemReferenceMutability::ReadOnly ||
            referencePointee(parameters[0]) !=
                referencePointee(parameters[1]) ||
            type(referencePointee(parameters[0])).kind != SemTypeKind::Float) {
          error = "float equality requires two const references of one float type";
          return false;
        }
      } else if (role == CompilerIntrinsicRole::PointerHash) {
        if (parameters.size() != 2 || !is_u64(instruction_type) ||
            type(parameters[0]).kind != SemTypeKind::RawPointer ||
            !is_u64(parameters[1])) {
          error = "pointer hash requires raw pointer and u64 seed";
          return false;
        }
      } else if (role == CompilerIntrinsicRole::PointerEqual) {
        if (parameters.size() != 2 || instruction_type != bool_type_ ||
            type(parameters[0]).kind != SemTypeKind::RawPointer ||
            type(parameters[1]).kind != SemTypeKind::RawPointer ||
            parameters[0] != parameters[1]) {
          error = "pointer equality requires two equal raw pointer types";
          return false;
        }
      }
      std::array<std::uint8_t, 2> orderings{};
      for (std::uint8_t ordering = 0;
           ordering < compilerIntrinsicOrderingCount(role); ++ordering) {
        const auto parameter =
            compilerIntrinsicOrderingParameter(role, ordering);
        const auto order = constant_enum_discriminant(arguments[parameter]);
        if (!order ||
            !isValidCompilerIntrinsicOrdering(role, ordering, *order)) {
          error = "compiler intrinsic has an invalid memory ordering";
          return false;
        }
        orderings[ordering] = *order;
      }
      if (compilerIntrinsicOrderingCount(role) == 2 &&
          !isValidCompareExchangeOrderingPair(orderings[0], orderings[1])) {
        error = "compare-exchange failure ordering exceeds success ordering";
        return false;
      }
      return true;
}

} // namespace chtholly::compiler::internal
