#include "chtholly/Compiler/LowIR.h"

#include "chtholly/Compiler/BuiltinOperator.h"
#include "chtholly/Compiler/CallableOwnership.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cctype>
#include <deque>
#include <functional>
#include <limits>
#include <sstream>
#include <unordered_set>

namespace chtholly::compiler {
namespace {
#include "LowIRForeignABIHelpersInternal.h"
} // namespace

std::optional<ForeignAbiFunctionLayout>
LowIR::buildCallbackAbiLayout(TypeId callback_type, std::string &error) const {
  error.clear();
  if (!callback_type.hasValue() ||
      callback_type.index >= sem_ir_->typeCount()) {
    error = "callback ABI query has an invalid semantic type";
    return std::nullopt;
  }
  const auto &function_type = sem_ir_->type(callback_type);
  if (function_type.kind != SemTypeKind::CFunctionPointer &&
      function_type.kind != SemTypeKind::CVariadicFunctionPointer) {
    error = "callback ABI query does not name a C function-pointer type";
    return std::nullopt;
  }
  const auto target_kind = classifyForeignTarget(normalized_target_triple_);
  const auto calling_convention =
      sem_ir_->cFunctionCallingConvention(callback_type);
  const auto convention_valid =
      calling_convention == ForeignCallingConvention::C ||
      (calling_convention == ForeignCallingConvention::Win64 &&
       target_kind == ForeignAbiTargetKind::WindowsX64) ||
      (calling_convention == ForeignCallingConvention::SysV64 &&
       target_kind == ForeignAbiTargetKind::SysVAMD64);
  if (!convention_valid) {
    error = "foreign calling convention is incompatible with target '" +
            normalized_target_triple_ + "'";
    return std::nullopt;
  }
  ForeignAbiFunctionLayout layout;
  layout.callback_type = callback_type;
  layout.target_kind = target_kind;
  layout.is_variadic =
      function_type.kind == SemTypeKind::CVariadicFunctionPointer;
  layout.calling_convention = calling_convention;
  auto result =
      buildForeignAbiValueLayout(TypeId(function_type.arg1), true, error);
  if (!result)
    return std::nullopt;
  layout.result = std::move(*result);
  for (const auto parameter :
       sem_ir_->typeBlock(TypeBlockId(function_type.arg0))) {
    auto value = buildForeignAbiValueLayout(parameter, false, error);
    if (!value)
      return std::nullopt;
    layout.parameters.push_back(std::move(*value));
  }
  return layout;
}

std::optional<ForeignAbiValueLayout>
LowIR::buildForeignAbiValueLayout(TypeId type, bool result,
                                  std::string &error) const {
  ForeignAbiValueLayout layout;
  layout.semantic_type = type;
  const auto physical = sem_ir_->foreignRepresentationType(type);
  const auto &semantic = sem_ir_->type(physical.hasValue() ? physical : type);
  if (semantic.kind == SemTypeKind::Void) {
    if (!result) {
      error = "foreign ABI parameter cannot have void type";
      return std::nullopt;
    }
    layout.kind = ForeignPassKind::Ignore;
    return layout;
  }
  const auto target_kind = classifyForeignTarget(normalized_target_triple_);
  if (semantic.kind != SemTypeKind::Nominal &&
      semantic.kind != SemTypeKind::Tuple &&
      semantic.kind != SemTypeKind::Array) {
    if (semantic.kind != SemTypeKind::Bool &&
        semantic.kind != SemTypeKind::Char &&
        semantic.kind != SemTypeKind::Integer &&
        semantic.kind != SemTypeKind::Float &&
        semantic.kind != SemTypeKind::Reference &&
        semantic.kind != SemTypeKind::RawPointer &&
        semantic.kind != SemTypeKind::CFunctionPointer &&
        semantic.kind != SemTypeKind::CVariadicFunctionPointer) {
      error =
          "foreign ABI value is not a transport scalar or repr(C) aggregate";
      return std::nullopt;
    }
    layout.kind = ForeignPassKind::Scalar;
    if (semantic.kind == SemTypeKind::Bool || semantic.kind == SemTypeKind::Char)
      layout.extension = ForeignExtensionKind::Zero;
    else if (semantic.kind == SemTypeKind::Integer && semantic.arg0 < 32 &&
             (target_kind == ForeignAbiTargetKind::SysVAMD64 ||
              target_kind == ForeignAbiTargetKind::AAPCS64))
      layout.extension = semantic.arg1 != 0 ? ForeignExtensionKind::Sign
                                            : ForeignExtensionKind::Zero;
    return layout;
  }
  if (semantic.kind == SemTypeKind::Nominal) {
    const auto &root_nominal =
        sem_ir_->nominalType(NominalTypeId(semantic.arg0));
    if (root_nominal.representation_policy != NominalRepresentationPolicy::C) {
      error =
          "foreign aggregate does not have a verified repr(C) representation";
      return std::nullopt;
    }
  }

  struct Leaf {
    TypeId type;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint64_t alignment = 1;
  };
  struct ObjectLayout {
    std::uint64_t size = 0;
    std::uint64_t alignment = 1;
    std::vector<Leaf> leaves;
    bool contains_union = false;
  };
  std::unordered_set<std::uint32_t> active;
  const auto checked_align = [](std::uint64_t value, std::uint64_t alignment,
                                std::uint64_t &aligned) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0)
      return false;
    const auto mask = alignment - 1;
    if (value > std::numeric_limits<std::uint64_t>::max() - mask)
      return false;
    aligned = (value + mask) & ~mask;
    return true;
  };
  std::function<std::optional<ObjectLayout>(TypeId)> object_layout;
  object_layout = [&](TypeId object_type) -> std::optional<ObjectLayout> {
    const auto &object = sem_ir_->type(object_type);
    switch (object.kind) {
    case SemTypeKind::Bool:
      return ObjectLayout{1, 1, {{object_type, 0, 1, 1}}};
    case SemTypeKind::Char:
      return ObjectLayout{4, 4, {{object_type, 0, 4, 4}}};
    case SemTypeKind::Integer:
    case SemTypeKind::Float: {
      const auto bytes = object.arg0 / 8U;
      return ObjectLayout{bytes, bytes, {{object_type, 0, bytes, bytes}}};
    }
    case SemTypeKind::Reference:
    case SemTypeKind::RawPointer:
    case SemTypeKind::CFunctionPointer:
    case SemTypeKind::CVariadicFunctionPointer:
      return ObjectLayout{8, 8, {{object_type, 0, 8, 8}}};
    case SemTypeKind::Slice:
      return ObjectLayout{16, 8, {{object_type, 0, 16, 8}}};
    case SemTypeKind::Tuple: {
      ObjectLayout value;
      std::uint64_t cursor = 0;
      const auto is_union = sem_ir_->isCUnionType(object_type);
      value.contains_union = is_union;
      for (const auto element_type :
           sem_ir_->typeBlock(TypeBlockId(object.arg0))) {
        auto element = object_layout(element_type);
        if (!element)
          return std::nullopt;
        std::uint64_t offset = 0;
        if (!is_union && !checked_align(cursor, element->alignment, offset)) {
          error = "tuple has no finite target layout";
          return std::nullopt;
        }
        cursor =
            is_union ? std::max(cursor, element->size) : offset + element->size;
        value.alignment = std::max(value.alignment, element->alignment);
        for (const auto &leaf : element->leaves)
          value.leaves.push_back(
              {leaf.type, leaf.offset + offset, leaf.size, leaf.alignment});
      }
      if (!checked_align(cursor, value.alignment, value.size)) {
        error = "tuple has no finite target layout";
        return std::nullopt;
      }
      return value;
    }
    case SemTypeKind::Array: {
      auto element = object_layout(TypeId(object.arg0));
      if (!element || object.arg1 == 0 ||
          element->size >
              std::numeric_limits<std::uint64_t>::max() / object.arg1) {
        error = "foreign array has no finite target layout";
        return std::nullopt;
      }
      ObjectLayout value{element->size * object.arg1,
                         element->alignment,
                         {},
                         element->contains_union};
      for (std::uint32_t index = 0; index < object.arg1; ++index)
        for (const auto &leaf : element->leaves)
          value.leaves.push_back({leaf.type,
                                  leaf.offset + index * element->size,
                                  leaf.size, leaf.alignment});
      return value;
    }
    case SemTypeKind::Nominal: {
      if (!active.insert(object_type.index).second) {
        error = "foreign aggregate layout has a recursive value dependency";
        return std::nullopt;
      }
      ObjectLayout value;
      std::uint64_t cursor = 0;
      const auto &nominal = sem_ir_->nominalType(NominalTypeId(object.arg0));
      value.contains_union = nominal.kind == NominalKind::Union;
      for (const auto field : typeRepresentation(object_type).object_fields) {
        auto child = object_layout(field);
        std::uint64_t offset = 0;
        if (!child ||
            (nominal.kind == NominalKind::Struct &&
             !checked_align(cursor, child->alignment, offset)) ||
            child->size > std::numeric_limits<std::uint64_t>::max() - offset) {
          active.erase(object_type.index);
          if (error.empty())
            error = "foreign aggregate field has no target layout";
          return std::nullopt;
        }
        for (const auto &leaf : child->leaves)
          value.leaves.push_back(
              {leaf.type, offset + leaf.offset, leaf.size, leaf.alignment});
        cursor = nominal.kind == NominalKind::Union
                     ? std::max(cursor, child->size)
                     : offset + child->size;
        value.alignment = std::max(value.alignment, child->alignment);
        value.contains_union |= child->contains_union;
      }
      active.erase(object_type.index);
      if (!checked_align(cursor, value.alignment, value.size)) {
        error = "foreign aggregate target layout overflows";
        return std::nullopt;
      }
      return value;
    }
    default:
      error = "foreign aggregate field has no concrete target layout";
      return std::nullopt;
    }
  };

  auto object = object_layout(physical.hasValue() ? physical : type);
  if (!object)
    return std::nullopt;
  layout.size = object->size;
  layout.alignment = object->alignment;
  const auto indirect = [&] {
    layout.kind = ForeignPassKind::Indirect;
    layout.lanes.push_back({ForeignPhysicalKind::Pointer, 0, 1, 0});
    layout.by_value = !result && target_kind == ForeignAbiTargetKind::SysVAMD64;
  };
  if (target_kind == ForeignAbiTargetKind::Unsupported) {
    error = "foreign aggregate ABI has no classifier for target '" +
            normalized_target_triple_ + "'";
    return std::nullopt;
  }
  if (target_kind == ForeignAbiTargetKind::WindowsX64) {
    if (layout.size == 1 || layout.size == 2 || layout.size == 4 ||
        layout.size == 8) {
      layout.kind = ForeignPassKind::Direct;
      layout.lanes.push_back({ForeignPhysicalKind::Integer,
                              static_cast<std::uint32_t>(layout.size * 8), 1,
                              0});
    } else
      indirect();
    return layout;
  }
  if (target_kind == ForeignAbiTargetKind::AAPCS64) {
    bool homogeneous = !object->contains_union && !object->leaves.empty() &&
                       object->leaves.size() <= 4;
    std::uint32_t homogeneous_width = 0;
    for (const auto &leaf : object->leaves) {
      const auto &leaf_type = sem_ir_->type(leaf.type);
      if (leaf_type.kind != SemTypeKind::Float ||
          (leaf_type.arg0 != 32 && leaf_type.arg0 != 64) ||
          (homogeneous_width != 0 && homogeneous_width != leaf_type.arg0)) {
        homogeneous = false;
        break;
      }
      homogeneous_width = leaf_type.arg0;
    }
    if (homogeneous) {
      layout.kind = ForeignPassKind::Direct;
      layout.lanes.push_back(
          {ForeignPhysicalKind::HomogeneousFloat, homogeneous_width,
           static_cast<std::uint32_t>(object->leaves.size()), 0});
    } else if (layout.size != 0 && layout.size <= 16) {
      layout.kind = ForeignPassKind::Direct;
      if (layout.size <= 8)
        layout.lanes.push_back({ForeignPhysicalKind::Integer,
                                static_cast<std::uint32_t>(layout.size * 8), 1,
                                0});
      else
        layout.lanes.push_back({ForeignPhysicalKind::Integer, 64, 2, 0});
    } else
      indirect();
    return layout;
  }
  if (layout.size == 0 || layout.size > 16) {
    indirect();
    return layout;
  }
  enum class LaneClass : std::uint8_t { None, Integer, Sse, Memory };
  struct LaneInfo {
    LaneClass kind = LaneClass::None;
    std::uint8_t f32_count = 0;
    bool has_f64 = false;
  };
  std::array<LaneInfo, 2> lanes;
  for (const auto &leaf : object->leaves) {
    if (leaf.size == 0 || leaf.offset % leaf.alignment != 0 ||
        leaf.offset / 8 != (leaf.offset + leaf.size - 1) / 8) {
      lanes[std::min<std::size_t>(leaf.offset / 8, 1)].kind = LaneClass::Memory;
      continue;
    }
    auto &lane = lanes[leaf.offset / 8];
    const auto &leaf_type = sem_ir_->type(leaf.type);
    const auto incoming = leaf_type.kind == SemTypeKind::Float
                              ? LaneClass::Sse
                              : LaneClass::Integer;
    if (lane.kind == LaneClass::None)
      lane.kind = incoming;
    else if (lane.kind != incoming)
      lane.kind = LaneClass::Integer;
    if (incoming == LaneClass::Sse) {
      lane.has_f64 |= leaf_type.arg0 == 64;
      lane.f32_count += leaf_type.arg0 == 32 ? 1U : 0U;
    }
  }
  if (std::ranges::any_of(lanes, [](const auto &lane) {
        return lane.kind == LaneClass::Memory;
      })) {
    indirect();
    return layout;
  }
  layout.kind = ForeignPassKind::Direct;
  const auto lane_count = static_cast<std::size_t>((layout.size + 7) / 8);
  for (std::size_t index = 0; index < lane_count; ++index) {
    const auto bytes = std::min<std::uint64_t>(8, layout.size - index * 8);
    if (lanes[index].kind == LaneClass::Sse)
      layout.lanes.push_back(
          {lanes[index].has_f64          ? ForeignPhysicalKind::Float64
           : lanes[index].f32_count >= 2 ? ForeignPhysicalKind::Float32Vector2
                                         : ForeignPhysicalKind::Float32,
           lanes[index].has_f64 ? 64U : 32U, 1,
           static_cast<std::uint64_t>(index * 8)});
    else
      layout.lanes.push_back({ForeignPhysicalKind::Integer,
                              static_cast<std::uint32_t>(bytes * 8), 1,
                              static_cast<std::uint64_t>(index * 8)});
  }
  return layout;
}

std::optional<ForeignAbiFunctionLayout>
LowIR::buildForeignAbiLayout(FunctionRefId target, std::string &error) const {
  error.clear();
  const auto *signature = foreignSignature(*sem_ir_, target);
  const auto &reference = sem_ir_->functionRef(target);
  const auto &function_type = sem_ir_->type(reference.local_type);
  if (!signature || function_type.kind != SemTypeKind::Function) {
    error = "foreign callable has no valid semantic ABI signature";
    return std::nullopt;
  }
  const auto parameters = sem_ir_->typeBlock(TypeBlockId(function_type.arg0));
  const auto *operation = foreignOperation(*sem_ir_, target);
  const auto hidden_outcome =
      operation &&
      operation->outcome_projection ==
          interop::ForeignOperationArtifact::OutcomeProjection::Win32Read;
  const auto mapped_public_parameters = static_cast<std::size_t>(
      hidden_outcome ? std::ranges::count_if(
                           operation->argument_sources,
                           [](auto source) {
                             return source.kind ==
                                    interop::ForeignOperationArtifact::
                                        ArgumentSourceKind::PublicArgument;
                           })
                     : 0);
  if ((!hidden_outcome && parameters.size() != signature->parameters.size()) ||
      (hidden_outcome &&
       (operation->argument_sources.size() != signature->parameters.size() ||
        mapped_public_parameters != parameters.size() ||
        operation->outcome_buffer_lane >=
            operation->argument_sources.size()))) {
    error = "foreign callable signature does not match its semantic type";
    return std::nullopt;
  }

  const auto target_kind = classifyForeignTarget(normalized_target_triple_);
  const auto convention_valid =
      signature->calling_convention == ForeignCallingConvention::C ||
      (signature->calling_convention == ForeignCallingConvention::Win64 &&
       target_kind == ForeignAbiTargetKind::WindowsX64) ||
      (signature->calling_convention == ForeignCallingConvention::SysV64 &&
       target_kind == ForeignAbiTargetKind::SysVAMD64);
  if (!convention_valid) {
    error = "foreign calling convention is incompatible with target '" +
            normalized_target_triple_ + "'";
    return std::nullopt;
  }
  ForeignAbiFunctionLayout layout;
  layout.target = target;
  layout.callback_type = TypeId::invalid();
  layout.target_kind = target_kind;
  layout.is_variadic = signature->is_variadic;
  layout.calling_convention = signature->calling_convention;
  auto physical_result = TypeId(function_type.arg1);
  if (const auto shape = sem_ir_->canonicalResultShape(physical_result))
    physical_result = shape->success;
  auto result_layout = buildForeignAbiValueLayout(physical_result, true, error);
  if (!result_layout)
    return std::nullopt;
  layout.result = std::move(*result_layout);
  std::vector<TypeId> physical_parameters;
  const auto find_type = [&](const auto &predicate) {
    for (std::uint32_t index = 0; index < sem_ir_->typeCount(); ++index) {
      const auto type = TypeId(index);
      if (predicate(type))
        return type;
    }
    return TypeId::invalid();
  };
  const auto count_value_type = find_type([&](TypeId type) {
    const auto &value = sem_ir_->type(type);
    return value.kind == SemTypeKind::Integer && value.arg0 == 32 &&
           value.arg1 == 0;
  });
  const auto count_reference_type = find_type([&](TypeId type) {
    return sem_ir_->type(type).kind == SemTypeKind::Reference &&
           sem_ir_->referenceMutability(type) ==
               SemReferenceMutability::Mutable &&
           sem_ir_->referencePointee(type) == count_value_type;
  });
  if (!hidden_outcome) {
    physical_parameters.assign(parameters.begin(), parameters.end());
  } else {
    for (const auto source : operation->argument_sources) {
      if (source.kind == interop::ForeignOperationArtifact::ArgumentSourceKind::
                             PublicArgument &&
          source.index < parameters.size()) {
        physical_parameters.push_back(parameters[source.index]);
      } else if (source.kind == interop::ForeignOperationArtifact::
                                    ArgumentSourceKind::OutcomeStorage) {
        physical_parameters.push_back(count_reference_type);
      } else if (source.kind == interop::ForeignOperationArtifact::
                                    ArgumentSourceKind::NullPointer) {
        const auto buffer_source =
            operation->argument_sources[operation->outcome_buffer_lane];
        physical_parameters.push_back(
            buffer_source.kind == interop::ForeignOperationArtifact::
                                      ArgumentSourceKind::PublicArgument &&
                    buffer_source.index < parameters.size()
                ? parameters[buffer_source.index]
                : TypeId::invalid());
      } else {
        error = "foreign outcome has an invalid physical argument source";
        return std::nullopt;
      }
    }
  }
  if (std::ranges::any_of(physical_parameters,
                          [](TypeId type) { return !type.hasValue(); })) {
    error = "foreign outcome hidden lane type was not materialized";
    return std::nullopt;
  }
  for (std::size_t index = 0; index < physical_parameters.size(); ++index) {
    auto parameter =
        buildForeignAbiValueLayout(physical_parameters[index], false, error);
    if (!parameter)
      return std::nullopt;
    layout.parameters.push_back(std::move(*parameter));
  }
  return layout;
}

std::optional<ForeignAbiThunkPlan>
LowIR::buildForeignAbiThunkPlan(FunctionRefId source, TypeId callback_type,
                                std::string &error) const {
  error.clear();
  if (!source.hasValue() || source.index >= sem_ir_->functionRefCount() ||
      !callback_type.hasValue() ||
      callback_type.index >= sem_ir_->typeCount()) {
    error = "callback thunk plan has an invalid source or callback type";
    return std::nullopt;
  }
  const auto layout_id = foreignAbiLayoutForCallback(callback_type);
  if (!layout_id.hasValue()) {
    error = "callback thunk plan has no verified target ABI layout";
    return std::nullopt;
  }
  const auto &layout = foreignAbiLayout(layout_id);
  const auto &callback = sem_ir_->type(callback_type);
  const auto &reference = sem_ir_->functionRef(source);
  if (callback.kind != SemTypeKind::CFunctionPointer || layout.is_variadic ||
      reference.generic.hasValue()) {
    error = "callback thunk plan requires a fixed, concrete definition";
    return std::nullopt;
  }
  const auto &source_type = sem_ir_->type(reference.local_type);
  const auto callback_parameters =
      sem_ir_->typeBlock(TypeBlockId(callback.arg0));
  const auto source_parameters =
      source_type.kind == SemTypeKind::Function
          ? sem_ir_->typeBlock(TypeBlockId(source_type.arg0))
          : std::span<const TypeId>{};
  const auto canonical_witnesses = sem_ir_->reverseTargetWitnesses(source);
  if (source_type.kind != SemTypeKind::Function ||
      !sem_ir_->isConcreteReverseTarget(source) ||
      source_type.arg1 != callback.arg1 ||
      !std::ranges::equal(source_parameters, callback_parameters) ||
      !callbackAdapterOwnershipSubstitutes(*sem_ir_, source, callback_type)) {
    error = "callback thunk source violates its semantic signature or contract";
    return std::nullopt;
  }

  const auto context_parameter =
      sem_ir_->callbackContextParameter(callback_type);
  const auto context_carrier = context_parameter < callback_parameters.size()
                                   ? callback_parameters[context_parameter]
                                   : TypeId::invalid();
  const auto is_release = [&]() {
    if (!context_carrier.hasValue() || callback_parameters.size() != 1 ||
        TypeId(callback.arg1) != sem_ir_->voidType())
      return false;
    const OwnershipRegion root{.parameter_index = context_parameter};
    const auto &contract = sem_ir_->callbackContract(callback_type);
    return std::ranges::any_of(contract.effects,
                               [&](const auto &effect) {
                                 return effect.kind ==
                                            CallableEffectKind::Take &&
                                        effect.region == root;
                               }) &&
           std::ranges::any_of(
               contract.postconditions, [&](const auto &postcondition) {
                 return postcondition.region == root &&
                        postcondition.outcomes == CallableOutcomeInvalidate;
               });
  }();
  ForeignAbiThunkPlan plan{
      .source = source,
      .callback_type = callback_type,
      .callback_layout = layout_id,
      .target_kind = canonical_witnesses.empty()
                         ? ReverseThunkTargetKind::OrdinaryFunction
                         : ReverseThunkTargetKind::InterfaceWitness,
      .role = !context_carrier.hasValue() ? ReverseThunkRole::Direct
              : is_release                ? ReverseThunkRole::ContextRelease
                                          : ReverseThunkRole::ContextEntry,
      .context_parameter = context_parameter,
      .context_carrier = context_carrier,
      .canonical_witnesses = canonical_witnesses};
  for (const auto &parameter : layout.parameters) {
    auto kind = ForeignAbiThunkParameterKind::Count;
    if (parameter.kind == ForeignPassKind::Scalar)
      kind = ForeignAbiThunkParameterKind::Scalar;
    else if (parameter.kind == ForeignPassKind::Direct)
      kind = ForeignAbiThunkParameterKind::DirectLanes;
    else if (parameter.kind == ForeignPassKind::Indirect)
      kind = ForeignAbiThunkParameterKind::IndirectObject;
    if (kind == ForeignAbiThunkParameterKind::Count) {
      error = "callback thunk parameter has no physical conversion strategy";
      return std::nullopt;
    }
    plan.parameters.push_back(
        {.semantic_type = parameter.semantic_type,
         .kind = kind,
         .semantic_uses_object_pointer =
             typeRepresentation(parameter.semantic_type).facts.value_repr ==
             ValueReprKind::Pointer});
  }
  plan.result.semantic_type = layout.result.semantic_type;
  plan.result.semantic_uses_return_slot =
      layout.result.kind != ForeignPassKind::Ignore &&
      typeRepresentation(layout.result.semantic_type).facts.init_repr ==
          InitReprKind::InPlace;
  if (layout.result.kind == ForeignPassKind::Ignore)
    plan.result.kind = ForeignAbiThunkResultKind::Ignore;
  else if (layout.result.kind == ForeignPassKind::Scalar)
    plan.result.kind = ForeignAbiThunkResultKind::Scalar;
  else if (layout.result.kind == ForeignPassKind::Direct)
    plan.result.kind = ForeignAbiThunkResultKind::DirectLanes;
  else if (layout.result.kind == ForeignPassKind::Indirect)
    plan.result.kind = ForeignAbiThunkResultKind::IndirectReturnSlot;
  if (plan.result.kind == ForeignAbiThunkResultKind::Count ||
      (plan.result.semantic_uses_return_slot !=
       (plan.result.kind == ForeignAbiThunkResultKind::DirectLanes ||
        plan.result.kind == ForeignAbiThunkResultKind::IndirectReturnSlot))) {
    error = "callback thunk result disagrees with the semantic return form";
    return std::nullopt;
  }
  return plan;
}

ForeignAbiThunkPlanId LowIR::addForeignAbiThunkPlan(FunctionRefId source,
                                                    TypeId callback_type,
                                                    std::string &error) {
  auto plan = buildForeignAbiThunkPlan(source, callback_type, error);
  if (!plan)
    return ForeignAbiThunkPlanId::invalid();
  for (std::uint32_t index = 0; index < foreign_abi_thunk_plans_.size();
       ++index)
    if (foreign_abi_thunk_plans_[index] == *plan)
      return ForeignAbiThunkPlanId(index);
  const auto id = ForeignAbiThunkPlanId(
      static_cast<std::uint32_t>(foreign_abi_thunk_plans_.size()));
  foreign_abi_thunk_plans_.push_back(std::move(*plan));
  return id;
}

std::optional<CallbackAdapterPlan>
LowIR::buildCallbackAdapterPlan(TypeId adapter_type, std::string &error) const {
  error.clear();
  if (!adapter_type.hasValue() || adapter_type.index >= sem_ir_->typeCount() ||
      sem_ir_->type(adapter_type).kind != SemTypeKind::CallbackAdapter) {
    error = "callback adapter plan has an invalid semantic type";
    return std::nullopt;
  }
  const auto fields =
      sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(adapter_type).arg0));
  if (fields.size() != 3 ||
      sem_ir_->type(fields[0]).kind != SemTypeKind::CFunctionPointer ||
      sem_ir_->type(fields[1]).kind != SemTypeKind::RawPointer ||
      sem_ir_->type(fields[2]).kind != SemTypeKind::CFunctionPointer) {
    error = "callback adapter plan has invalid entry/context/release types";
    return std::nullopt;
  }
  const auto find_call_layout = [&](TypeId callback) {
    const auto fixed = foreignAbiLayoutForCallback(callback);
    for (std::uint32_t index = 0; index < foreign_abi_call_layouts_.size();
         ++index) {
      const auto &call = foreign_abi_call_layouts_[index];
      if (call.function_layout == fixed && call.source_suffix_types.empty() &&
          call.suffix.empty())
        return ForeignAbiCallLayoutId(index);
    }
    return ForeignAbiCallLayoutId::invalid();
  };
  const auto entry_call = find_call_layout(fields[0]);
  const auto release_call = find_call_layout(fields[2]);
  const auto context_parameter = sem_ir_->callbackContextParameter(fields[0]);
  const auto release_context = sem_ir_->callbackContextParameter(fields[2]);
  const auto entry_parameters =
      sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(fields[0]).arg0));
  const auto release_parameters =
      sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(fields[2]).arg0));
  if (!entry_call.hasValue() || !release_call.hasValue() ||
      context_parameter >= entry_parameters.size() ||
      entry_parameters[context_parameter] != fields[1] ||
      release_context != 0 || release_parameters.size() != 1 ||
      release_parameters[0] != fields[1] ||
      TypeId(sem_ir_->type(fields[2]).arg1) != sem_ir_->voidType()) {
    error = "callback adapter plan disagrees with its explicit context ABI";
    return std::nullopt;
  }
  return CallbackAdapterPlan{.adapter_type = adapter_type,
                             .entry_call_layout = entry_call,
                             .release_call_layout = release_call,
                             .context_parameter = context_parameter,
                             .context_carrier = fields[1]};
}

std::optional<CallbackRegistrationPlan>
LowIR::buildCallbackRegistrationPlan(TypeId type, std::string &error) const {
  error.clear();
  if (!type.hasValue() || type.index >= sem_ir_->typeCount() ||
      sem_ir_->type(type).kind != SemTypeKind::CallbackRegistration) {
    error = "callback registration plan has an invalid semantic type";
    return std::nullopt;
  }
  const auto fields = sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(type).arg0));
  if ((fields.size() != 5 && fields.size() != 7 && fields.size() != 8 &&
       fields.size() != 10) ||
      sem_ir_->type(fields[1]).kind != SemTypeKind::RawPointer ||
      sem_ir_->rawPointerPointee(fields[1]) != sem_ir_->voidType() ||
      sem_ir_->rawPointerPointeeConst(fields[1])) {
    error = "callback registration plan has invalid handle storage";
    return std::nullopt;
  }
  ForeignResourceProtocolId protocol_id;
  const auto *protocol = verifiedForeignResourceProtocol(*sem_ir_, type, false,
                                                         protocol_id, error);
  if (!protocol)
    return std::nullopt;
  const auto role_type = [&](ForeignResourceRoleKind kind) {
    const auto *role = protocol->facts.findRole(kind);
    return role ? fields[role->callable_type_index] : TypeId::invalid();
  };
  const auto callback_plan = callbackAdapterPlanFor(fields[0]);
  if (!callback_plan.hasValue()) {
    error = "callback registration plan has no callback adapter plan";
    return std::nullopt;
  }
  const auto find_call_layout = [&](TypeId callback) {
    const auto fixed = foreignAbiLayoutForCallback(callback);
    for (std::uint32_t index = 0; index < foreign_abi_call_layouts_.size();
         ++index) {
      const auto &call = foreign_abi_call_layouts_[index];
      if (call.function_layout == fixed && call.source_suffix_types.empty() &&
          call.suffix.empty())
        return ForeignAbiCallLayoutId(index);
    }
    return ForeignAbiCallLayoutId::invalid();
  };
  const auto reg =
      find_call_layout(role_type(ForeignResourceRoleKind::AcquireOwned));
  const auto unreg =
      find_call_layout(role_type(ForeignResourceRoleKind::CloseQuiescent));
  const auto cancel =
      find_call_layout(role_type(ForeignResourceRoleKind::CancelQuiescent));
  const auto cancel_async =
      fields.size() >= 7
          ? find_call_layout(role_type(ForeignResourceRoleKind::CancelAsync))
          : ForeignAbiCallLayoutId::invalid();
  if (!reg.hasValue() || !unreg.hasValue() || !cancel.hasValue() ||
      (fields.size() >= 7 && !cancel_async.hasValue())) {
    error = "callback registration plan has incomplete C call layouts";
    return std::nullopt;
  }
  const auto check_terminal = [&](TypeId fn) {
    const auto &value = sem_ir_->type(fn);
    const auto parameters = sem_ir_->typeBlock(TypeBlockId(value.arg0));
    return parameters.size() == 1 && parameters.front() == fields[1] &&
           TypeId(value.arg1) == sem_ir_->voidType() &&
           sem_ir_->callbackContextParameter(fn) == core::AnyId::InvalidIndex;
  };
  const auto &register_value = sem_ir_->type(fields[2]);
  const auto register_parameters =
      sem_ir_->typeBlock(TypeBlockId(register_value.arg0));
  const auto *acquire =
      protocol->facts.findRole(ForeignResourceRoleKind::AcquireOwned);
  const auto parameter = [&](ForeignResourceParameterKind kind) {
    const auto found = std::ranges::find(
        acquire->parameters, kind, &ForeignResourceParameterBinding::kind);
    return found == acquire->parameters.end() ? core::AnyId::InvalidIndex
                                              : found->parameter_index;
  };
  const std::array marked{
      parameter(ForeignResourceParameterKind::CallbackEntry),
      parameter(ForeignResourceParameterKind::CallbackUserdata),
      parameter(ForeignResourceParameterKind::CallbackRelease)};
  const auto bindings = sem_ir_->callbackRegistrationBindings(type);
  const auto authority = sem_ir_->callbackRegistrationAuthority(type);
  auto completion_plan = CallbackCompletionPlanId::invalid();
  if (fields.size() >= 7) {
    CanonicalForeignResourceProtocol projection;
    const std::array<TypeId, 7> projected_fields{
        fields[0],
        fields[1],
        fields[1],
        fields[6],
        fields.size() >= 8 ? fields[7] : TypeId::invalid(),
        fields.size() == 10 ? fields[8] : TypeId::invalid(),
        fields.size() == 10 ? fields[9] : TypeId::invalid()};
    const auto projection_size = fields.size() == 10  ? 7U
                                 : fields.size() >= 8 ? 5U
                                                      : 4U;
    for (std::uint32_t index = 0; index < projection_size; ++index)
      projection.types.push_back(
          sem_ir_->canonicalType(projected_fields[index]));
    projection.facts = makeCallbackCompletionProtocol(
        static_cast<std::uint8_t>(authority), projection_size,
        sem_ir_->callbackArmParameters(type),
        sem_ir_->callbackDetachParameters(type));
    for (std::uint32_t index = 0; index < callback_completion_plans_.size();
         ++index) {
      const auto &candidate = callback_completion_plans_[index];
      if (sem_ir_->foreignResourceProtocol(candidate.completion_type) ==
          projection) {
        completion_plan = CallbackCompletionPlanId(index);
        break;
      }
    }
    if (!completion_plan.hasValue()) {
      error = "callback registration has no exact completion projection";
      return std::nullopt;
    }
  }
  const auto expected_register_parameters =
      (authority == CallbackReleaseAuthority::Transferred ? 3U : 2U) +
      bindings.size();
  std::vector<bool> occupied(register_parameters.size());
  if (marked[0] < occupied.size())
    occupied[marked[0]] = true;
  if (marked[1] < occupied.size())
    occupied[marked[1]] = true;
  if (authority == CallbackReleaseAuthority::Transferred &&
      marked[2] < occupied.size())
    occupied[marked[2]] = true;
  std::vector<std::uint32_t> binding_parameters;
  binding_parameters.reserve(bindings.size());
  for (const auto &binding : bindings) {
    if (binding.parameter_index >= occupied.size() ||
        occupied[binding.parameter_index]) {
      error = "callback registration binding has an invalid physical parameter";
      return std::nullopt;
    }
    occupied[binding.parameter_index] = true;
    binding_parameters.push_back(binding.parameter_index);
  }
  if (register_parameters.size() != expected_register_parameters ||
      marked[0] >= register_parameters.size() ||
      marked[1] >= register_parameters.size() || marked[0] == marked[1] ||
      register_parameters[marked[0]] !=
          sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(fields[0]).arg0))[0] ||
      register_parameters[marked[1]] != fields[1] ||
      TypeId(register_value.arg1) != fields[1] ||
      sem_ir_->callbackContextParameter(fields[2]) !=
          core::AnyId::InvalidIndex ||
      !check_terminal(fields[3]) || !check_terminal(fields[4]) ||
      (authority == CallbackReleaseAuthority::Retained &&
       marked[2] != core::AnyId::InvalidIndex) ||
      (authority == CallbackReleaseAuthority::Transferred &&
       (marked[2] >= register_parameters.size() || marked[2] == marked[0] ||
        marked[2] == marked[1] ||
        register_parameters[marked[2]] !=
            sem_ir_->typeBlock(
                TypeBlockId(sem_ir_->type(fields[0]).arg0))[2])) ||
      std::ranges::any_of(occupied, [](bool value) { return !value; })) {
    error = "callback registration plan disagrees with marked C parameters";
    return std::nullopt;
  }
  return CallbackRegistrationPlan{.registration_type = type,
                                  .protocol = protocol_id,
                                  .callback_plan = callback_plan,
                                  .register_call_layout = reg,
                                  .unregister_call_layout = unreg,
                                  .cancel_call_layout = cancel,
                                  .cancel_async_call_layout = cancel_async,
                                  .completion_plan = completion_plan,
                                  .entry_parameter = marked[0],
                                  .userdata_parameter = marked[1],
                                  .release_parameter = marked[2],
                                  .binding_parameters =
                                      std::move(binding_parameters),
                                  .authority = authority};
}

std::optional<CallbackCompletionPlan>
LowIR::buildCallbackCompletionPlan(TypeId type, std::string &error) const {
  error.clear();
  if (!type.hasValue() || type.index >= sem_ir_->typeCount() ||
      sem_ir_->type(type).kind != SemTypeKind::CallbackCompletion) {
    error = "callback completion plan has an invalid semantic type";
    return std::nullopt;
  }
  const auto fields = sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(type).arg0));
  if ((fields.size() != 4 && fields.size() != 5 && fields.size() != 7) ||
      sem_ir_->type(fields[1]).kind != SemTypeKind::RawPointer ||
      sem_ir_->type(fields[2]).kind != SemTypeKind::RawPointer ||
      fields[1] != fields[2]) {
    error = "callback completion plan has invalid token storage";
    return std::nullopt;
  }
  ForeignResourceProtocolId protocol_id;
  const auto *protocol =
      verifiedForeignResourceProtocol(*sem_ir_, type, true, protocol_id, error);
  if (!protocol)
    return std::nullopt;
  const auto callback_plan = callbackAdapterPlanFor(fields[0]);
  if (!callback_plan.hasValue()) {
    error = "callback completion plan has no callback adapter plan";
    return std::nullopt;
  }
  const auto *wait_role =
      protocol->facts.findRole(ForeignResourceRoleKind::WaitCompletion);
  const auto fixed =
      foreignAbiLayoutForCallback(fields[wait_role->callable_type_index]);
  auto wait = ForeignAbiCallLayoutId::invalid();
  for (std::uint32_t index = 0; index < foreign_abi_call_layouts_.size();
       ++index) {
    const auto &call = foreign_abi_call_layouts_[index];
    if (call.function_layout == fixed && call.source_suffix_types.empty() &&
        call.suffix.empty()) {
      wait = ForeignAbiCallLayoutId(index);
      break;
    }
  }
  if (!wait.hasValue()) {
    error = "callback completion plan has no wait call layout";
    return std::nullopt;
  }
  return CallbackCompletionPlan{.completion_type = type,
                                .protocol = protocol_id,
                                .callback_plan = callback_plan,
                                .wait_call_layout = wait,
                                .authority =
                                    sem_ir_->callbackCompletionAuthority(type)};
}

std::optional<CallbackReadinessPlan>
LowIR::buildCallbackReadinessPlan(TypeId type, std::string &error) const {
  error.clear();
  if (!type.hasValue() || type.index >= sem_ir_->typeCount() ||
      sem_ir_->type(type).kind != SemTypeKind::CallbackCompletion) {
    error = "callback readiness plan has an invalid semantic type";
    return std::nullopt;
  }
  const auto fields = sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(type).arg0));
  if (fields.size() != 5 && fields.size() != 7) {
    error = "callback readiness plan requires a poll-capable completion";
    return std::nullopt;
  }
  ForeignResourceProtocolId protocol_id;
  const auto *protocol =
      verifiedForeignResourceProtocol(*sem_ir_, type, true, protocol_id, error);
  if (!protocol)
    return std::nullopt;
  const auto completion_plan = callbackCompletionPlanFor(type);
  if (!completion_plan.hasValue()) {
    error = "callback readiness plan has no epoch-12 completion plan";
    return std::nullopt;
  }
  const auto *ready_role =
      protocol->facts.findRole(ForeignResourceRoleKind::InspectReady);
  const auto fixed =
      foreignAbiLayoutForCallback(fields[ready_role->callable_type_index]);
  auto poll = ForeignAbiCallLayoutId::invalid();
  for (std::uint32_t index = 0; index < foreign_abi_call_layouts_.size();
       ++index) {
    const auto &call = foreign_abi_call_layouts_[index];
    if (call.function_layout == fixed && call.source_suffix_types.empty() &&
        call.suffix.empty()) {
      poll = ForeignAbiCallLayoutId(index);
      break;
    }
  }
  if (!poll.hasValue()) {
    error = "callback readiness plan has no poll call layout";
    return std::nullopt;
  }
  return CallbackReadinessPlan{.completion_type = type,
                               .protocol = protocol_id,
                               .completion_plan = completion_plan,
                               .poll_call_layout = poll};
}

std::optional<CallbackWakePlan>
LowIR::buildCallbackWakePlan(TypeId type, std::string &error) const {
  error.clear();
  if (!type.hasValue() || type.index >= sem_ir_->typeCount() ||
      sem_ir_->type(type).kind != SemTypeKind::CallbackCompletion) {
    error = "callback wake plan has an invalid completion type";
    return std::nullopt;
  }
  const auto fields = sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(type).arg0));
  if (fields.size() != 7) {
    error = "callback wake plan requires the full epoch-14 capability";
    return std::nullopt;
  }
  ForeignResourceProtocolId protocol_id;
  const auto *protocol =
      verifiedForeignResourceProtocol(*sem_ir_, type, true, protocol_id, error);
  if (!protocol)
    return std::nullopt;
  const auto completion_plan = callbackCompletionPlanFor(type);
  const auto readiness_plan = callbackReadinessPlanFor(type);
  if (!completion_plan.hasValue() || !readiness_plan.hasValue()) {
    error = "callback wake plan has unresolved epoch-12/13 dependencies";
    return std::nullopt;
  }
  const auto find_call_layout = [&](TypeId callback) {
    const auto fixed = foreignAbiLayoutForCallback(callback);
    for (std::uint32_t index = 0; index < foreign_abi_call_layouts_.size();
         ++index) {
      const auto &call = foreign_abi_call_layouts_[index];
      if (call.function_layout == fixed && call.source_suffix_types.empty() &&
          call.suffix.empty())
        return ForeignAbiCallLayoutId(index);
    }
    return ForeignAbiCallLayoutId::invalid();
  };
  const auto *arm_role =
      protocol->facts.findRole(ForeignResourceRoleKind::ArmOneShot);
  const auto *detach_role =
      protocol->facts.findRole(ForeignResourceRoleKind::DetachCompletion);
  const auto arm = find_call_layout(fields[arm_role->callable_type_index]);
  const auto detach =
      find_call_layout(fields[detach_role->callable_type_index]);
  const auto arm_parameters = sem_ir_->callbackArmParameters(type);
  const auto detach_parameters = sem_ir_->callbackDetachParameters(type);
  const auto arm_values =
      sem_ir_->typeBlock(TypeBlockId(sem_ir_->type(fields[5]).arg0));
  if (arm_parameters[3] >= arm_values.size()) {
    error = "callback wake plan has an invalid release role";
    return std::nullopt;
  }
  const auto wake_release = find_call_layout(arm_values[arm_parameters[3]]);
  if (!arm.hasValue() || !detach.hasValue() || !wake_release.hasValue()) {
    error = "callback wake plan has incomplete foreign call layouts";
    return std::nullopt;
  }
  return CallbackWakePlan{.completion_type = type,
                          .protocol = protocol_id,
                          .completion_plan = completion_plan,
                          .readiness_plan = readiness_plan,
                          .arm_call_layout = arm,
                          .detach_call_layout = detach,
                          .wake_release_call_layout = wake_release,
                          .arm_parameters = arm_parameters,
                          .detach_parameters = detach_parameters,
                          .authority =
                              sem_ir_->callbackCompletionAuthority(type)};
}




} // namespace chtholly::compiler
