#include "PublicInterfaceConstructionInternal.h"

#include <algorithm>
#include <numeric>
#include <ranges>
#include <unordered_set>
#include <utility>

namespace chtholly::compiler::internal {

std::optional<PublicInterfaceFunctionConstructionResult>
PublicInterfaceFunctionConstructionService::build(
    const SemIR &sem_ir, PublicInterfaceRegistry &registry,
    interop::ArtifactRegistry &interop_registry, IdentifierId package_name,
    std::string &error, PublicInterfaceTypeConstructionContext &types,
    PublicInterfaceFunctionConstructionCallbacks callbacks) {
  std::function<std::optional<PublicType>(TypeId)> map_type = [&](TypeId type) {
    return types.mapType(type);
  };
  std::function<std::optional<PublicConstantValue>(ConstantId)> map_constant =
      [&](ConstantId id) { return types.mapConstant(id); };
  auto &mapped_generic = types.mappedGeneric();
  auto &mapped_constraints = types.mappedConstraints();
  const auto &nominal_artifacts = types.nominalArtifacts();
  std::vector<PublicFunctionBindingSpec> functions;
  std::vector<PublicNominalTypeArtifact> public_nominals;
  std::vector<PublicValueArtifact> public_values;
  std::vector<PublicInterfaceDeclarationArtifact> public_interfaces;
  std::vector<PublicTypeAliasArtifact> public_aliases;
  std::vector<PublicInterfaceWitnessArtifact> public_witnesses;
  std::unordered_map<std::uint32_t, std::size_t> local_function_specs;
  std::unordered_set<std::uint32_t> resource_closure_nominals;
  std::unordered_map<std::uint32_t, IdentifierId> hidden_resource_targets;
  std::unordered_map<std::uint32_t, IdentifierId> hidden_evaluator_targets;
  // A public operation artifact owns its physical signature dependencies even
  // when no nominal resource facade refers to them. Keep such handles as
  // unspellable artifact dependencies rather than source-level declarations.
  const auto add_operation_handle = [&](TypeId type) {
    if (!type.hasValue() || sem_ir.type(type).kind != SemTypeKind::Nominal)
      return;
    const auto nominal = NominalTypeId(sem_ir.type(type).arg0);
    if (sem_ir.nominalType(nominal).kind == NominalKind::ForeignHandle)
      resource_closure_nominals.insert(nominal.index);
  };
  for (std::uint32_t index = 0; index < sem_ir.functionCount(); ++index) {
    const auto function = FunctionId(index);
    if (!sem_ir.functionDeclaration(function).interop_artifact)
      continue;
    const auto &type = sem_ir.type(sem_ir.function(function).type);
    for (const auto parameter : sem_ir.typeBlock(TypeBlockId(type.arg0)))
      add_operation_handle(parameter);
    add_operation_handle(TypeId(type.arg1));
  }
  for (std::uint32_t index = 0; index < sem_ir.nominalTypeCount(); ++index)
    if ((sem_ir.nominalType(NominalTypeId(index)).flags &
         (SemNominalTypePublic | SemNominalTypeArtifactDependency)) != 0) {
      const auto &nominal = sem_ir.nominalType(NominalTypeId(index));
      public_nominals.push_back(*nominal_artifacts[index]);
      if (nominal.kind != NominalKind::ForeignResource)
        continue;
      const auto add_handle = [&](TypeId handle) {
        if (handle.hasValue() &&
            sem_ir.type(handle).kind == SemTypeKind::Nominal)
          resource_closure_nominals.insert(sem_ir.type(handle).arg0);
      };
      add_handle(nominal.foreign_handle_type);
      add_handle(nominal.foreign_completion_handle_type);
      for (const auto &operation : nominal.foreign_resource_operations) {
        const auto &target = sem_ir.functionRef(operation.target);
        if (!target.local_function.hasValue() ||
            (sem_ir.function(target.local_function).flags &
             SemFunctionPublic) != 0)
          continue;
        hidden_resource_targets.emplace(
            target.local_function.index,
            registry.internIdentifier(
                "$foreign$" +
                std::string(sem_ir.identifier(sem_ir.name(nominal.name).text)) +
                "$" +
                std::string(
                    sem_ir.identifier(sem_ir.name(operation.name).text))));
      }
    }
  for (const auto index : resource_closure_nominals)
    if ((sem_ir.nominalType(NominalTypeId(index)).flags &
         SemNominalTypePublic) == 0)
      public_nominals.push_back(*nominal_artifacts[index]);

  // A public const function carries a closed evaluator dependency graph. A
  // private dependency is assigned an unspellable, signature-derived identity
  // and is registered in the artifact without exposing its source name.
  std::vector<FunctionId> evaluator_work;
  std::unordered_set<std::uint32_t> evaluator_scanned;
  for (std::uint32_t index = 0; index < sem_ir.functionCount(); ++index) {
    const auto &function = sem_ir.function(FunctionId(index));
    if ((function.flags & (SemFunctionPublic | SemFunctionConst)) ==
        (SemFunctionPublic | SemFunctionConst))
      evaluator_work.push_back(FunctionId(index));
  }
  for (std::size_t work_index = 0; work_index < evaluator_work.size();
       ++work_index) {
    const auto function_id = evaluator_work[work_index];
    if (!evaluator_scanned.insert(function_id.index).second)
      continue;
    const auto &function = sem_ir.function(function_id);
    std::vector<InstBlockId> blocks{function.body};
    for (std::size_t block_index = 0; block_index < blocks.size();
         ++block_index) {
      for (const auto inst_id : sem_ir.instBlock(blocks[block_index])) {
        const auto &inst = sem_ir.inst(inst_id);
        const auto add_block = [&](InstBlockId block) {
          if (block.hasValue() &&
              std::ranges::find(blocks, block) == blocks.end())
            blocks.push_back(block);
        };
        if (inst.kind == SemInstKind::If || inst.kind == SemInstKind::Switch ||
            inst.kind == SemInstKind::SwitchArm ||
            inst.kind == SemInstKind::While || inst.kind == SemInstKind::For ||
            inst.kind == SemInstKind::ForClause ||
            inst.kind == SemInstKind::DoWhile)
          add_block(InstBlockId(inst.arg1));
        if (inst.kind == SemInstKind::Defer)
          add_block(InstBlockId(inst.arg0));
        if (inst.kind == SemInstKind::IfArm ||
            inst.kind == SemInstKind::While || inst.kind == SemInstKind::For ||
            inst.kind == SemInstKind::DoWhile)
          add_block(InstBlockId(inst.arg0));
        if (inst.kind != SemInstKind::Call)
          continue;
        const auto &reference = sem_ir.functionRef(FunctionRefId(inst.arg0));
        if (!reference.local_function.hasValue())
          continue;
        auto target_id = reference.local_function;
        const auto &target = sem_ir.function(target_id);
        if ((target.flags & SemFunctionPublic) != 0)
          continue;
        const auto &target_type = sem_ir.type(target.type);
        std::vector<PublicType> parameters;
        bool valid_signature = target_type.kind == SemTypeKind::Function;
        if (valid_signature) {
          for (const auto parameter :
               sem_ir.typeBlock(TypeBlockId(target_type.arg0))) {
            auto mapped = map_type(parameter);
            if (!mapped) {
              valid_signature = false;
              break;
            }
            parameters.push_back(std::move(*mapped));
          }
        }
        auto result =
            valid_signature ? map_type(TypeId(target_type.arg1)) : std::nullopt;
        if (!valid_signature || !result) {
          error = "const evaluator closure has an unsupported private "
                  "function signature";
          return std::nullopt;
        }
        const auto &declaration = sem_ir.functionDeclaration(target_id);
        const auto source_name =
            sem_ir.identifier(sem_ir.name(target.name).text);
        const auto signature = callbacks.entity_fingerprint(
            sem_ir.identifier(package_name),
            sem_ir.identifier(sem_ir.moduleName()), source_name, std::nullopt,
            PublicFunctionArtifact::MemberKind::None,
            target.generic.hasValue()
                ? sem_ir.genericValues().generic(target.generic).binding_count
                : 0,
            parameters, *result, std::nullopt,
            PublicFunctionExecutionKind::Immediate, {}, {}, {},
            CompilerIntrinsicRole::None, sem_ir.functionOwnership(target_id),
            std::nullopt, PublicCallableDeclarationKind::Definition,
            declaration.is_unsafe, declaration.is_const, {}, {}, {}, {}, {}, {},
            {});
        const auto hidden =
            registry.internIdentifier("$const-evaluator$" + signature.hex());
        hidden_evaluator_targets.emplace(target_id.index, hidden);
        if (declaration.is_const)
          evaluator_work.push_back(target_id);
      }
    }
  }
  for (std::uint32_t index = 0; index < sem_ir.functionCount(); ++index) {
    const auto &function = sem_ir.function(FunctionId(index));
    PublicTypeMappingScope mapping_scope(
        mapped_generic, mapped_constraints, function.generic,
        sem_ir.functionConstraints(FunctionId(index)));
    if ((function.flags & SemFunctionSpecific) != 0)
      continue;
    if ((function.flags & SemFunctionInterfaceMember) != 0 &&
        (function.flags & SemFunctionPublic) == 0 &&
        function.semantic_owner.hasValue() &&
        (sem_ir.nominalType(function.semantic_owner).flags &
         (SemNominalTypePublic | SemNominalTypeArtifactDependency)) == 0)
      continue;
    bool imported_function = false;
    for (std::uint32_t reference_index = 0;
         reference_index < sem_ir.functionRefCount(); ++reference_index) {
      const auto &reference =
          sem_ir.functionRef(FunctionRefId(reference_index));
      if (reference.local_function == FunctionId(index) &&
          reference.public_entity.hasValue()) {
        imported_function = true;
        break;
      }
    }
    if (imported_function)
      continue;
    auto name = sem_ir.name(function.name).text;
    const auto canonical_name = name;
    const auto hidden_target = hidden_resource_targets.find(index);
    if (hidden_target != hidden_resource_targets.end())
      name = hidden_target->second;
    const auto evaluator_target = hidden_evaluator_targets.find(index);
    if (evaluator_target != hidden_evaluator_targets.end())
      name = evaluator_target->second;
    const auto &local_contract =
        sem_ir.functionSemanticContract(FunctionId(index));
    std::optional<PublicEntityReferenceArtifact> member_owner;
    if ((local_contract.domain == CallableSemanticDomain::Ordinary ||
         local_contract.domain ==
             CallableSemanticDomain::NominalConstruction) &&
        function.semantic_owner.hasValue()) {
      const auto owner_index = function.semantic_owner.index;
      if (owner_index >= nominal_artifacts.size() ||
          !nominal_artifacts[owner_index])
        return std::nullopt;
      member_owner = nominal_artifacts[owner_index]->entity;
    }
    const auto semantic_role = local_contract.role;
    if (local_contract.domain != CallableSemanticDomain::Ordinary &&
        local_contract.domain != CallableSemanticDomain::NominalConstruction) {
      const auto &owner = sem_ir.nominalType(local_contract.owner);
      const auto representation =
          local_contract.domain == CallableSemanticDomain::ValueRepresentation;
      const auto projection =
          local_contract.domain == CallableSemanticDomain::ObjectProjection;
      const auto object_lifecycle =
          local_contract.domain == CallableSemanticDomain::ObjectShell;
      const auto role =
          semantic_role == SemCanonicalFunctionRole::Copy              ? "copy"
          : semantic_role == SemCanonicalFunctionRole::Drop            ? "drop"
          : semantic_role == SemCanonicalFunctionRole::Pack            ? "pack"
          : semantic_role == SemCanonicalFunctionRole::Init            ? "init"
          : semantic_role == SemCanonicalFunctionRole::ProjectionLoad  ? "load"
          : semantic_role == SemCanonicalFunctionRole::ProjectionStore ? "store"
          : semantic_role == SemCanonicalFunctionRole::ProjectionTake  ? "take"
          : semantic_role == SemCanonicalFunctionRole::ProjectionInit  ? "init"
          : semantic_role == SemCanonicalFunctionRole::ProjectionBorrow
              ? "borrow"
          : semantic_role == SemCanonicalFunctionRole::ProjectionBorrowMut
              ? "borrow_mut"
          : semantic_role == SemCanonicalFunctionRole::ObjectInit
              ? "object_init"
          : semantic_role == SemCanonicalFunctionRole::ObjectCopyInit
              ? "object_copy_init"
          : semantic_role == SemCanonicalFunctionRole::ObjectMoveInit
              ? "object_move_init"
              : "object_drop";
      std::string projector_name;
      if (projection && local_contract.projector_field < owner.fields.size())
        projector_name = std::string(sem_ir.identifier(
            sem_ir
                .name(
                    owner.fields[local_contract.projector_field].projector_name)
                .text));
      if (projection && function.generic.hasValue()) {
        name = sem_ir.genericValues().generic(function.generic).name;
      } else {
        name = registry.internIdentifier(
            std::string(representation     ? "$representation$"
                        : projection       ? "$projection$"
                        : object_lifecycle ? "$object$"
                                           : "$lifecycle$") +
            std::string(sem_ir.identifier(sem_ir.name(owner.name).text)) + "$" +
            (projection ? projector_name + "$" : std::string{}) + role);
      }
    }
    const auto &type = sem_ir.type(function.type);
    const auto is_async = type.kind == SemTypeKind::AsyncFunction;
    if (is_async && ((function.flags & SemFunctionPublic) == 0 ||
                     sem_ir.identifier(name) == "main"))
      name = registry.internIdentifier("$async$" +
                                       std::string(sem_ir.identifier(name)));
    if ((function.flags & (SemFunctionPublic | SemFunctionInterfaceMember)) ==
            0 &&
        !is_async &&
        (local_contract.domain == CallableSemanticDomain::Ordinary ||
         local_contract.domain ==
             CallableSemanticDomain::NominalConstruction) &&
        hidden_target == hidden_resource_targets.end() &&
        evaluator_target == hidden_evaluator_targets.end())
      continue;
    if (sem_ir.identifier(name) == "main")
      continue;
    const auto success_type =
        is_async ? sem_ir.asyncSuccessType(function.type) : TypeId(type.arg1);
    const auto mapped_return = map_type(success_type);
    if (!mapped_return)
      return std::nullopt;
    std::optional<PublicType> mapped_error;
    if (is_async) {
      if (const auto error_type = sem_ir.asyncErrorType(function.type)) {
        mapped_error = map_type(*error_type);
        if (!mapped_error)
          return std::nullopt;
      }
    }
    CallableSemanticContract semantic_contract;
    semantic_contract.domain = local_contract.domain;
    semantic_contract.role = local_contract.role;
    semantic_contract.capability = local_contract.capability;
    semantic_contract.projector_field = local_contract.projector_field;
    semantic_contract.whole_carrier = local_contract.whole_carrier;
    semantic_contract.carrier_path = local_contract.carrier_path;
    semantic_contract.has_bit_range = local_contract.has_bit_range;
    semantic_contract.bit_begin = local_contract.bit_begin;
    semantic_contract.bit_end = local_contract.bit_end;
    if (local_contract.owner.hasValue() &&
        local_contract.domain == CallableSemanticDomain::NominalConstruction) {
      if (local_contract.owner.index >= nominal_artifacts.size() ||
          !nominal_artifacts[local_contract.owner.index])
        return std::nullopt;
      if (local_contract.owner.index < nominal_artifacts.size() &&
          nominal_artifacts[local_contract.owner.index]) {
        semantic_contract.owner =
            PublicType(nominal_artifacts[local_contract.owner.index]->entity);
      } else if (mapped_return->kind == PublicTypeKind::Nominal) {
        semantic_contract.owner = *mapped_return;
      } else {
        return std::nullopt;
      }
    } else if (local_contract.owner.hasValue()) {
      const auto parameters = sem_ir.typeBlock(TypeBlockId(type.arg0));
      if (parameters.empty() ||
          sem_ir.type(parameters.front()).kind != SemTypeKind::Reference)
        return std::nullopt;
      const auto mapped_owner =
          map_type(sem_ir.referencePointee(parameters.front()));
      if (!mapped_owner || mapped_owner->kind != PublicTypeKind::Nominal)
        return std::nullopt;
      semantic_contract.owner = *mapped_owner;
    }
    PublicFunctionBindingSpec spec{
        .name = name,
        .member_owner = member_owner,
        .member_kind = member_owner ? ([&] {
          const auto owner = function.semantic_owner;
          if (!owner.hasValue())
            return PublicFunctionArtifact::MemberKind::None;
          const auto &nominal = sem_ir.nominalType(owner);
          const auto found = std::ranges::find_if(
              nominal.member_functions, [&](const auto &member) {
                const auto &reference = sem_ir.functionRef(member.target);
                return reference.local_function == FunctionId(index);
              });
          return found != nominal.member_functions.end() &&
                         (found->flags & SemNominalMemberFunctionAssociated) !=
                             0
                     ? PublicFunctionArtifact::MemberKind::Associated
                     : PublicFunctionArtifact::MemberKind::Instance;
        }())
                                    : PublicFunctionArtifact::MemberKind::None,
        .generic_parameter_count =
            function.generic.hasValue()
                ? sem_ir.genericValues().generic(function.generic).binding_count
                : 0,
        .return_type = *mapped_return,
        .error_type = std::move(mapped_error),
        .execution_kind = is_async ? PublicFunctionExecutionKind::Async
                                   : PublicFunctionExecutionKind::Immediate,
        .coroutine_constructor =
            is_async ? PublicCoroutineConstructorABI{1, true, true, true, true}
                     : PublicCoroutineConstructorABI{},
        .nominal_constructor =
            local_contract.role == CallableSemanticRole::Constructor
                ? PublicNominalConstructorABI{1,
                                              sem_ir.canonicalResultShape(
                                                  TypeId(type.arg1))
                                                  ? PublicNominalConstructorResultKind::
                                                        FallibleSelf
                                                  : PublicNominalConstructorResultKind::
                                                        DirectSelf}
                : PublicNominalConstructorABI{},
        .semantic_contract = std::move(semantic_contract),
        .intrinsic_role = function.intrinsic_role,
        .ownership_summary = sem_ir.functionOwnership(FunctionId(index))};
    const auto &declaration = sem_ir.functionDeclaration(FunctionId(index));
    spec.declaration_kind =
        declaration.kind == SemCallableDeclarationKind::Foreign
            ? PublicCallableDeclarationKind::Foreign
        : declaration.kind == SemCallableDeclarationKind::Forward
            ? PublicCallableDeclarationKind::Forward
            : PublicCallableDeclarationKind::Definition;
    spec.is_unsafe = declaration.is_unsafe;
    spec.is_const = declaration.is_const;
    if (declaration.foreign_abi.hasValue())
      spec.foreign_abi = sem_ir.identifier(declaration.foreign_abi);
    if (declaration.external_symbol.hasValue())
      spec.external_symbol = sem_ir.identifier(declaration.external_symbol);
    spec.foreign_signature = declaration.foreign_signature;
    spec.interop_artifact = declaration.interop_artifact;
    for (const auto parameter : sem_ir.typeBlock(TypeBlockId(type.arg0))) {
      auto mapped = map_type(parameter);
      if (!mapped)
        return std::nullopt;
      spec.parameters.push_back(std::move(*mapped));
    }
    spec.ownership_summary = callbacks.normalize_ownership(
        std::move(spec.ownership_summary), spec.parameters);
    spec.parameter_names.reserve(declaration.parameter_names.size());
    for (const auto parameter_name : declaration.parameter_names)
      spec.parameter_names.emplace_back(sem_ir.identifier(parameter_name));
    spec.default_arguments.reserve(declaration.default_arguments.size());
    for (const auto entity : declaration.default_arguments) {
      if (!entity.hasValue()) {
        spec.default_arguments.emplace_back();
        continue;
      }
      const auto &constant = sem_ir.constantEntity(entity);
      if (!constant.result.isConcrete())
        return std::nullopt;
      auto value = map_constant(constant.result.value);
      if (!value)
        return std::nullopt;
      spec.default_arguments.push_back(std::move(*value));
    }
    if (function.generic.hasValue())
      spec.generic = function.generic;
    // Bodyless generic declarations (notably interface requirements) publish
    // their signature but have no template region to materialize.
    if ((function.generic.hasValue() || spec.is_const) &&
        declaration.kind == SemCallableDeclarationKind::Definition) {
      spec.generic_template = callbacks.build_generic_template(
          sem_ir, function, sem_ir.identifier(package_name), map_type,
          hidden_evaluator_targets, error);
      if (!spec.generic_template)
        return std::nullopt;
    }
    if (hidden_target != hidden_resource_targets.end())
      spec.canonical_name = canonical_name;
    else if (member_owner)
      spec.canonical_name =
          registry.internIdentifier(member_owner->canonical_name + "::" +
                                    std::string(sem_ir.identifier(name)));
    local_function_specs.emplace(index, functions.size());
    functions.push_back(std::move(spec));
  }

  // Complete operation-reference shells only after every ordinary operation
  // has a stable fingerprint. Completion-family producers depend on members,
  // while members deliberately never acquire a reverse dependency.
  std::unordered_map<std::string, std::size_t> operation_specs;
  for (std::size_t index = 0; index < functions.size(); ++index) {
    if (!functions[index].interop_artifact)
      continue;
    const auto canonical = functions[index].canonical_name.hasValue()
                               ? functions[index].canonical_name
                               : functions[index].name;
    operation_specs.emplace(std::string(sem_ir.identifier(canonical)), index);
  }
  const auto operation_reference = [&](std::size_t index) {
    const auto &spec = functions[index];
    const auto canonical =
        spec.canonical_name.hasValue() ? spec.canonical_name : spec.name;
    return PublicEntityReferenceArtifact{
        PublicEntityKind::ForeignOperation,
        std::string(sem_ir.identifier(package_name)),
        std::string(sem_ir.identifier(sem_ir.moduleName())),
        std::string(sem_ir.identifier(canonical)),
        spec.interop_artifact->fingerprint};
  };
  std::vector<std::uint8_t> operation_completion_state(functions.size());
  std::function<bool(std::size_t)> complete_operation;
  const auto resolve_operation =
      [&](const std::optional<interop::ArtifactReference> &reference)
      -> const interop::ForeignOperationArtifact * {
    return reference ? interop_registry.resolve(*reference) : nullptr;
  };
  complete_operation = [&](std::size_t producer_index) {
    if (operation_completion_state[producer_index] == 2)
      return true;
    if (operation_completion_state[producer_index] == 1) {
      error = "CFDL operation completion dependency cycle";
      return false;
    }
    operation_completion_state[producer_index] = 1;
    auto &producer_spec = functions[producer_index];
    const auto *producer_payload =
        resolve_operation(producer_spec.interop_artifact);
    if (!producer_payload) {
      error = "CFDL operation reference is missing from the interop registry";
      return false;
    }
    auto producer = *producer_payload;
    using ArgumentKind = interop::ForeignCapability::ArgumentKind;
    std::unordered_map<std::string, std::size_t> roles;
    for (auto &capability : producer.capabilities) {
      if (!capability.path.starts_with("async.completion."))
        continue;
      const auto role =
          capability.path.substr(std::string_view("async.completion.").size());
      if (capability.arguments.size() != 1 ||
          capability.arguments.front().kind !=
              ArgumentKind::OperationReference ||
          !roles.emplace(role, 0).second) {
        error = "CFDL completion family has a duplicate or malformed role";
        return false;
      }
      const auto found = operation_specs.find(
          capability.arguments.front().entity.canonical_name);
      if (found == operation_specs.end() || found->second == producer_index) {
        error = found == operation_specs.end()
                    ? "CFDL completion family references an unknown operation"
                    : "CFDL completion family cannot reference itself";
        return false;
      }
      roles[role] = found->second;
    }
    if (roles.empty()) {
      operation_completion_state[producer_index] = 2;
      return true;
    }
    for (const auto role : {"source", "wait", "poll", "arm", "detach"})
      if (!roles.contains(role)) {
        error = "CFDL completion family is missing role `" + std::string(role) +
                "`";
        return false;
      }
    for (const auto &[_, member] : roles)
      if (!complete_operation(member))
        return false;
    const auto has_unique_capability = [&](std::size_t index,
                                           std::string_view path) {
      const auto *operation =
          resolve_operation(functions[index].interop_artifact);
      if (!operation)
        return false;
      return std::ranges::count(operation->capabilities, path,
                                &interop::ForeignCapability::path) == 1;
    };
    if (!has_unique_capability(roles["source"], "callback.action.register") ||
        !has_unique_capability(roles["source"],
                               "callback.result.subscription") ||
        !has_unique_capability(roles["wait"], "async.action.wait") ||
        !has_unique_capability(roles["poll"], "async.action.poll") ||
        !has_unique_capability(roles["arm"], "async.action.arm") ||
        !has_unique_capability(roles["detach"], "async.action.detach")) {
      error = "CFDL completion family member has the wrong operation role";
      return false;
    }
    const auto callback_layout =
        [&](std::size_t index) -> std::optional<PublicType> {
      const auto &spec = functions[index];
      const auto *operation = resolve_operation(spec.interop_artifact);
      if (!operation)
        return std::nullopt;
      const auto &stored = index == roles["source"]
                               ? operation->callback_adapter_layout
                               : operation->waker_adapter_layout;
      if (stored && stored->kind == PublicTypeKind::CallbackAdapter)
        return stored;
      const auto port = std::ranges::find(operation->ports,
                                          interop::ForeignPortKind::Callback,
                                          &interop::ForeignLogicalPort::kind);
      if (port == operation->ports.end() || port->lanes.size() != 3 ||
          std::ranges::any_of(port->lanes, [&](auto lane) {
            return lane >= spec.parameters.size();
          }))
        return std::nullopt;
      return PublicType::callbackAdapter(spec.parameters[port->lanes[0]],
                                         spec.parameters[port->lanes[1]],
                                         spec.parameters[port->lanes[2]]);
    };
    const auto registration_callback = callback_layout(roles["source"]);
    const auto waker_callback = callback_layout(roles["arm"]);
    if (!registration_callback || !waker_callback ||
        producer_spec.return_type.kind == PublicTypeKind::Void) {
      error = "CFDL completion family has an incomplete carrier or callback "
              "adapter layout";
      return false;
    }
    interop::CompletionFamily family;
    family.source = operation_reference(roles["source"]);
    family.wait = operation_reference(roles["wait"]);
    family.poll = operation_reference(roles["poll"]);
    family.arm = operation_reference(roles["arm"]);
    family.detach = operation_reference(roles["detach"]);
    family.completion_carrier = producer_spec.return_type;
    family.registration_callback_adapter = *registration_callback;
    family.waker_callback_adapter = *waker_callback;
    family.source_handle_lane = static_cast<std::uint32_t>(
        functions[roles["source"]].parameters.size());
    family.token_result_lane =
        static_cast<std::uint32_t>(producer_spec.parameters.size());
    family.authority =
        resolve_operation(functions[roles["source"]].interop_artifact)
            ->callback_authority;
    family.input_effect = producer.completion_input_effect;
    family.readiness_literal = producer.readiness_success_literal;
    family.arm_lane_map.resize(functions[roles["arm"]].parameters.size());
    std::iota(family.arm_lane_map.begin(), family.arm_lane_map.end(), 0U);
    family.detach_lane_map.resize(functions[roles["detach"]].parameters.size());
    std::iota(family.detach_lane_map.begin(), family.detach_lane_map.end(), 0U);
    family.abi_epoch = 1;
    producer.completion_descriptor = std::move(family);
    for (auto &capability : producer.capabilities) {
      if (!capability.path.starts_with("async.completion."))
        continue;
      const auto role =
          capability.path.substr(std::string_view("async.completion.").size());
      capability.arguments.front().entity = operation_reference(roles[role]);
    }
    producer.abi_epoch = 2;
    producer.fingerprint = callbacks.foreign_operation_fingerprint(
        producer_spec.parameters, producer_spec.return_type, producer);
    operation_completion_state[producer_index] = 2;
    return true;
  };
  for (const auto &[_, index] : operation_specs)
    if (!complete_operation(index))
      return std::nullopt;

  // Publish the final canonical operation (including any completed callback
  // or completion family) before constructing the public interface. The
  // public binding retains only the stable reference identity at the wire
  // boundary; the registry owns the verified session value.
  for (auto &spec : functions) {
    if (!spec.interop_artifact)
      continue;
    const auto canonical_name =
        spec.canonical_name.hasValue() ? spec.canonical_name : spec.name;
    const auto *artifact_payload = resolve_operation(spec.interop_artifact);
    if (!artifact_payload) {
      error = "CFDL operation reference is missing from the interop registry";
      return std::nullopt;
    }
    const auto artifact = *artifact_payload;
    auto reference = interop_registry.publish(
        sem_ir.identifier(package_name), sem_ir.identifier(sem_ir.moduleName()),
        sem_ir.identifier(canonical_name), artifact, error);
    if (!reference.fingerprint.hasValue())
      return std::nullopt;
    spec.interop_artifact = std::move(reference);
  }

  return PublicInterfaceFunctionConstructionResult{
      .functions = std::move(functions),
      .nominals = std::move(public_nominals),
      .values = std::move(public_values),
      .interfaces = std::move(public_interfaces),
      .aliases = std::move(public_aliases),
      .witnesses = std::move(public_witnesses),
      .local_function_specs = std::move(local_function_specs)};
}

} // namespace chtholly::compiler::internal
