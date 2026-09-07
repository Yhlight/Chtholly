#include "chtholly/Compiler/SemIR.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <ranges>
#include <unordered_map>

namespace chtholly::compiler {

InstBlockId SemIR::cloneSpecificBody(
    InstBlockId source_body, GenericId generic,
    std::span<const CanonicalTypeId> arguments, NodeId clone_location,
    std::span<const SemGenericSubstitution> dependent_substitutions,
    std::unordered_map<std::uint32_t, LocalId> &locals,
    const std::function<FunctionRefId(FunctionRefId, std::span<const InstId>,
                                      NodeId)> &specialize_callee,
    const std::function<FunctionRefId(std::uint64_t, std::span<const InstId>,
                                      NodeId)> &resolve_interface_call,
    std::string &error) {
  error.clear();
  const auto substitute_canonical =
      [&](auto &&self, CanonicalTypeId source) -> CanonicalTypeId {
    const auto type_value = values_->generics().type(source);
    if (type_value.kind == CanonicalTypeKind::TypeParameter &&
        type_value.arg0 == generic.index)
      return type_value.arg1 < arguments.size() ? arguments[type_value.arg1]
                                                : CanonicalTypeId::invalid();
    if (type_value.kind == CanonicalTypeKind::TypeParameter) {
      const auto substitution =
          std::ranges::find(dependent_substitutions, GenericId(type_value.arg0),
                            &SemGenericSubstitution::generic);
      if (substitution != dependent_substitutions.end())
        return type_value.arg1 < substitution->arguments.size()
                   ? substitution->arguments[type_value.arg1]
                   : CanonicalTypeId::invalid();
    }
    if (type_value.kind == CanonicalTypeKind::TypeProjection) {
      const auto projected_source =
          self(self, CanonicalTypeId(type_value.arg0));
      if (!projected_source.hasValue())
        return CanonicalTypeId::invalid();
      const auto &source_type = values_->generics().type(projected_source);
      if (type_value.projection_kind == CanonicalTypeProjectionKind::Element) {
        if (source_type.kind == CanonicalTypeKind::Array &&
            type_value.arg1 < source_type.arg1)
          return CanonicalTypeId(source_type.arg0);
        if (source_type.kind == CanonicalTypeKind::Tuple &&
            type_value.arg1 < source_type.elements.size())
          return source_type.elements[type_value.arg1];
      } else if (type_value.projection_kind ==
                     CanonicalTypeProjectionKind::Pointee &&
                 (source_type.kind == CanonicalTypeKind::Reference ||
                  source_type.kind == CanonicalTypeKind::RawPointer ||
                  source_type.kind == CanonicalTypeKind::Slice) &&
                 source_type.elements.size() == 1) {
        return source_type.elements.front();
      }
      CanonicalType projection = type_value;
      projection.arg0 = projected_source.index;
      return values_->generics().internType(std::move(projection));
    }
    if (type_value.kind == CanonicalTypeKind::Array) {
      const auto element = self(self, CanonicalTypeId(type_value.arg0));
      return values_->generics().internType({.kind = CanonicalTypeKind::Array,
                                             .arg0 = element.index,
                                             .arg1 = type_value.arg1});
    }
    if (type_value.kind == CanonicalTypeKind::Tuple ||
        type_value.kind == CanonicalTypeKind::Slice) {
      CanonicalType result = type_value;
      result.elements.clear();
      for (const auto element : type_value.elements)
        result.elements.push_back(self(self, element));
      return values_->generics().internType(std::move(result));
    }
    if (type_value.kind == CanonicalTypeKind::Function ||
        type_value.kind == CanonicalTypeKind::AsyncFunction ||
        type_value.kind == CanonicalTypeKind::Nominal ||
        type_value.kind == CanonicalTypeKind::Reference ||
        type_value.kind == CanonicalTypeKind::RawPointer ||
        type_value.kind == CanonicalTypeKind::CFunctionPointer ||
        type_value.kind == CanonicalTypeKind::CVariadicFunctionPointer ||
        (type_value.kind == CanonicalTypeKind::CallbackAdapter ||
         type_value.kind == CanonicalTypeKind::CallbackRegistration ||
         type_value.kind == CanonicalTypeKind::CallbackCompletion ||
         type_value.kind == CanonicalTypeKind::CallbackWake)) {
      CanonicalType result = type_value;
      result.elements.clear();
      result.elements.reserve(type_value.elements.size());
      for (const auto element : type_value.elements)
        result.elements.push_back(self(self, element));
      return values_->generics().internType(std::move(result));
    }
    return source;
  };
  const auto substitute_type = [&](TypeId source) {
    return materializeType(
        substitute_canonical(substitute_canonical, canonicalType(source)));
  };
  const auto remains_dependent = [&](auto &&self,
                                     CanonicalTypeId source) -> bool {
    if (!source.hasValue())
      return true;
    const auto &value = values_->generics().type(source);
    if (value.kind == CanonicalTypeKind::TypeParameter ||
        value.kind == CanonicalTypeKind::TypeProjection)
      return true;
    if (value.kind == CanonicalTypeKind::Array)
      return self(self, CanonicalTypeId(value.arg0));
    return std::ranges::any_of(value.elements, [&](CanonicalTypeId element) {
      return self(self, element);
    });
  };
  const auto clone_local = [&](LocalId source) {
    if (const auto found = locals.find(source.index); found != locals.end())
      return found->second;
    const auto &value = local(source);
    const auto result = addLocal(
        {value.name, substitute_type(value.type), clone_location, value.flags});
    locals.emplace(source.index, result);
    return result;
  };

  std::unordered_map<std::uint32_t, InstId> insts;
  std::unordered_map<std::uint32_t, InstBlockId> blocks;
  const auto clone_block = [&](auto &&self, InstBlockId source) -> InstBlockId {
    if (const auto found = blocks.find(source.index); found != blocks.end())
      return found->second;
    std::vector<InstId> result;
    for (const auto old_id : instBlock(source)) {
      const auto old = inst(old_id);
      if (old.kind == SemInstKind::FunctionDecl ||
          old.kind == SemInstKind::Invalid) {
        error =
            "generic template contains an unsupported dependent instruction";
        return InstBlockId::invalid();
      }
      SemInst value = old;
      value.type = substitute_type(TypeId(old.type)).index;
      if (old.kind == SemInstKind::TypeQuery) {
        const auto query_index =
            static_cast<std::uint32_t>(integer(IntegerId(old.arg0)));
        if (query_index >= typeQueryCount()) {
          error = "generic type query index is out of range";
          return InstBlockId::invalid();
        }
        const auto &query = typeQuery(query_index);
        const auto source_canonical = substitute_canonical(
            substitute_canonical, canonicalType(query.source));
        const auto source_type = materializeType(source_canonical);
        const auto concrete =
            source_type.hasValue() &&
            !remains_dependent(remains_dependent, source_canonical);
        if (!concrete) {
          error = "generic type query did not resolve during specialization";
          return InstBlockId::invalid();
        }
        bool bool_result = false;
        std::uint64_t integer_result = 0;
        switch (query.kind) {
        case SemTypeQueryArtifact::Kind::TypeSame: {
          const auto other = substitute_canonical(substitute_canonical,
                                                  canonicalType(query.other));
          if (remains_dependent(remains_dependent, other)) {
            error = "generic type equality query did not resolve during "
                    "specialization";
            return InstBlockId::invalid();
          }
          bool_result = source_canonical == other;
          break;
        }
        case SemTypeQueryArtifact::Kind::TypeIs: {
          const auto kind = type(source_type).kind;
          bool_result =
              (query.property == "integer" && kind == SemTypeKind::Integer) ||
              (query.property == "floating" && kind == SemTypeKind::Float) ||
              (query.property == "raw_pointer" &&
               kind == SemTypeKind::RawPointer) ||
              (query.property == "array" && kind == SemTypeKind::Array) ||
              (query.property == "nominal" && kind == SemTypeKind::Nominal) ||
              (query.property == "string" && kind == SemTypeKind::String) ||
              (query.property == "reference" && kind == SemTypeKind::Reference);
          break;
        }
        case SemTypeQueryArtifact::Kind::TypeHas: {
          const auto facts = typeRepresentation(source_type);
          bool_result = (query.property == "copy" &&
                         facts.copy != CopyReprKind::Unavailable &&
                         facts.copy != CopyReprKind::Dependent) ||
                        (query.property == "move" &&
                         facts.move != MoveReprKind::Unavailable &&
                         facts.move != MoveReprKind::Dependent) ||
                        (query.property == "drop" &&
                         facts.destroy != DestroyReprKind::None &&
                         facts.destroy != DestroyReprKind::Dependent) ||
                        (query.property == "object_representation" &&
                         facts.object_repr != ObjectReprKind::None);
          break;
        }
        case SemTypeQueryArtifact::Kind::ArrayExtent:
          if (type(source_type).kind != SemTypeKind::Array) {
            error = "array extent query source is not an array";
            return InstBlockId::invalid();
          }
          integer_result = type(source_type).arg1;
          break;
        case SemTypeQueryArtifact::Kind::TupleArity:
          if (type(source_type).kind != SemTypeKind::Tuple) {
            error = "tuple arity query source is not a tuple";
            return InstBlockId::invalid();
          }
          integer_result = tupleArity(source_type);
          break;
        case SemTypeQueryArtifact::Kind::Count:
          error = "generic type query has an invalid kind";
          return InstBlockId::invalid();
        }
        value.kind = (query.kind == SemTypeQueryArtifact::Kind::ArrayExtent ||
                      query.kind == SemTypeQueryArtifact::Kind::TupleArity)
                         ? SemInstKind::IntegerLiteral
                         : SemInstKind::BoolLiteral;
        value.arg0 = addInteger((value.kind == SemInstKind::IntegerLiteral)
                                    ? static_cast<std::int64_t>(integer_result)
                                : bool_result ? 1
                                              : 0)
                         .index;
        value.arg1 = core::AnyId::InvalidIndex;
      }
      const auto translate = [&](SemArgKind kind,
                                 std::uint32_t raw) -> std::uint32_t {
        switch (kind) {
        case SemArgKind::Inst: {
          const auto found = insts.find(raw);
          return found == insts.end() ? core::AnyId::InvalidIndex
                                      : found->second.index;
        }
        case SemArgKind::Local:
          return clone_local(LocalId(raw)).index;
        case SemArgKind::Block:
          return self(self, InstBlockId(raw)).index;
        default:
          return raw;
        }
      };
      if (old.kind == SemInstKind::Call ||
          old.kind == SemInstKind::ForeignOperationCall ||
          old.kind == SemInstKind::CompilerIntrinsicCall ||
          old.kind == SemInstKind::InterfaceCall ||
          old.kind == SemInstKind::IndirectCall) {
        std::vector<InstId> arguments;
        for (const auto argument : instBlock(InstBlockId(old.arg1))) {
          const auto found = insts.find(argument.index);
          if (found == insts.end()) {
            error = "generic call arguments are not in canonical order";
            return InstBlockId::invalid();
          }
          arguments.push_back(found->second);
        }
        if (old.kind == SemInstKind::Call ||
            old.kind == SemInstKind::ForeignOperationCall ||
            old.kind == SemInstKind::CompilerIntrinsicCall) {
          auto target = FunctionRefId(old.arg0);
          if (functionRef(target).generic.hasValue()) {
            target = specialize_callee
                         ? specialize_callee(target, arguments, clone_location)
                         : FunctionRefId::invalid();
            if (!target.hasValue()) {
              error = "generic template call specialization failed";
              return InstBlockId::invalid();
            }
          }
          if ((old.kind == SemInstKind::CompilerIntrinsicCall) !=
              (functionIntrinsicRole(target) != CompilerIntrinsicRole::None)) {
            error = "generic intrinsic call lost its verified target role";
            return InstBlockId::invalid();
          }
          value.arg0 = target.index;
        } else if (old.kind == SemInstKind::InterfaceCall) {
          const auto encoded =
              static_cast<std::uint64_t>(integer(IntegerId(old.arg0)));
          const auto target =
              resolve_interface_call
                  ? resolve_interface_call(encoded, arguments, clone_location)
                  : FunctionRefId::invalid();
          if (!target.hasValue()) {
            error = "generic interface call resolution failed";
            return InstBlockId::invalid();
          }
          value.kind = SemInstKind::Call;
          value.arg0 = target.index;
          const auto &target_type = type(functionRef(target).local_type);
          if (target_type.kind != SemTypeKind::Function) {
            error = "resolved generic interface target is not callable";
            return InstBlockId::invalid();
          }
          value.type = target_type.arg1;
        } else {
          const auto target = insts.find(old.arg0);
          if (target == insts.end()) {
            error = "generic indirect call target is not in canonical order";
            return InstBlockId::invalid();
          }
          value.arg0 = target->second.index;
        }
        value.arg1 = addInstBlock(arguments).index;
      } else if (old.kind == SemInstKind::BuiltinBinary) {
        std::vector<InstId> operands;
        for (const auto operand : instBlock(InstBlockId(old.arg0))) {
          const auto found = insts.find(operand.index);
          if (found == insts.end()) {
            error = "generic builtin operands are not in canonical order";
            return InstBlockId::invalid();
          }
          operands.push_back(found->second);
        }
        value.arg0 = addInstBlock(operands, true).index;
        value.arg1 = old.arg1;
      } else if (old.kind == SemInstKind::EndFullExpression) {
        std::vector<InstId> temporaries;
        for (const auto temporary : instBlock(InstBlockId(old.arg0))) {
          const auto found = insts.find(temporary.index);
          if (found == insts.end()) {
            error = "generic full-expression temporaries are not in canonical "
                    "order";
            return InstBlockId::invalid();
          }
          temporaries.push_back(found->second);
        }
        value.arg0 = addInstBlock(temporaries, true).index;
      } else if (old.kind == SemInstKind::Closure) {
        std::vector<InstId> captures;
        for (const auto capture : instBlock(InstBlockId(old.arg1))) {
          const auto found = insts.find(capture.index);
          if (found == insts.end()) {
            error = "generic closure captures are not in canonical order";
            return InstBlockId::invalid();
          }
          captures.push_back(found->second);
        }
        auto target = FunctionRefId(old.arg0);
        if (functionRef(target).generic.hasValue()) {
          target = specialize_callee
                       ? specialize_callee(target, {}, clone_location)
                       : FunctionRefId::invalid();
          if (!target.hasValue()) {
            error = "generic closure specialization failed";
            return InstBlockId::invalid();
          }
        }
        value.arg0 = target.index;
        value.arg1 = addInstBlock(captures, true).index;
      } else if (old.kind == SemInstKind::AggregateInit ||
                 old.kind == SemInstKind::ArrayLiteral ||
                 old.kind == SemInstKind::TupleLiteral ||
                 old.kind == SemInstKind::Slice ||
                 old.kind == SemInstKind::EnumInit) {
        std::vector<InstId> elements;
        for (const auto element : instBlock(InstBlockId(old.arg0))) {
          const auto found = insts.find(element.index);
          if (found == insts.end()) {
            error = "generic value block is not in canonical order";
            return InstBlockId::invalid();
          }
          elements.push_back(found->second);
        }
        value.arg0 = addInstBlock(elements, true).index;
        if (old.kind == SemInstKind::EnumInit)
          value.arg1 = old.arg1;
      } else if (old.kind == SemInstKind::TypeQuery) {
        // The query was folded above. Preserve the resulting literal rather
        // than remapping the original descriptor index into its operand.
      } else if (old.kind == SemInstKind::BoundMethod) {
        const auto receiver = insts.find(old.arg0);
        if (receiver == insts.end()) {
          error = "generic bound method receiver is not in canonical order";
          return InstBlockId::invalid();
        }
        auto target = FunctionRefId(old.arg1);
        if (functionRef(target).generic.hasValue()) {
          const std::array arguments{receiver->second};
          target = specialize_callee
                       ? specialize_callee(target, arguments, clone_location)
                       : FunctionRefId::invalid();
          if (!target.hasValue()) {
            error = "generic bound method specialization failed";
            return InstBlockId::invalid();
          }
        }
        value.arg0 = receiver->second.index;
        value.arg1 = target.index;
      } else {
        value.arg0 = translate(semInstArgKind(old.kind, 0), old.arg0);
        value.arg1 = translate(semInstArgKind(old.kind, 1), old.arg1);
      }
      if ((semInstArgKind(old.kind, 0) == SemArgKind::Inst &&
           value.arg0 == core::AnyId::InvalidIndex) ||
          (semInstArgKind(old.kind, 1) == SemArgKind::Inst &&
           value.arg1 == core::AnyId::InvalidIndex) ||
          (semInstArgKind(old.kind, 0) == SemArgKind::Block &&
           value.arg0 == core::AnyId::InvalidIndex) ||
          (semInstArgKind(old.kind, 1) == SemArgKind::Block &&
           value.arg1 == core::AnyId::InvalidIndex)) {
        error = "generic template instruction order is not canonical";
        return InstBlockId::invalid();
      }
      const auto id = addRawInst(value, clone_location);
      if (old.kind == SemInstKind::Closure ||
          old.kind == SemInstKind::BoundMethod) {
        const auto target = FunctionRefId(
            old.kind == SemInstKind::Closure ? value.arg0 : value.arg1);
        const auto target_type = type(functionRef(target).local_type);
        const auto target_parameters =
            target_type.kind == SemTypeKind::Function
                ? typeBlock(TypeBlockId(target_type.arg0))
                : std::span<const TypeId>{};
        const auto capability =
            !target_parameters.empty() &&
                    type(target_parameters.front()).kind ==
                        SemTypeKind::Reference
                ? referenceMutability(target_parameters.front()) ==
                          SemReferenceMutability::Mutable
                      ? SemCallableEnvironmentCapability::Mutable
                      : SemCallableEnvironmentCapability::ReadOnly
                : SemCallableEnvironmentCapability::Consuming;
        const auto *source_info = tryGetCallableEnvironment(TypeId(old.type));
        setCallableEnvironment(
            {old.kind == SemInstKind::Closure
                 ? SemCallableEnvironmentKind::Closure
                 : SemCallableEnvironmentKind::BoundMethod,
             TypeId(value.type), target, target, capability,
             source_info ? source_info->identity : StableFingerprint{}});
      }
      insts.emplace(old_id.index, id);
      result.push_back(id);
    }
    const auto id = addInstBlock(result);
    blocks.emplace(source.index, id);
    return id;
  };
  return clone_block(clone_block, source_body);
}



} // namespace chtholly::compiler
