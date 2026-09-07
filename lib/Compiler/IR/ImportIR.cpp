#include "chtholly/Compiler/ImportIR.h"

#include <algorithm>
#include <cstdint>
#include <unordered_set>

namespace chtholly::compiler {

std::size_t
ImportIRInstHash::operator()(const ImportIRInst &value) const noexcept {
  auto hash = static_cast<std::size_t>(value.ir.index);
  hash ^= static_cast<std::size_t>(value.binding.index) + 0x9e3779b9U +
          (hash << 6U) + (hash >> 2U);
  return hash;
}

ImportIRTable::ImportIRTable(CheckIRId owner,
                             const PublicInterfaceRegistry &registry,
                             const interop::ArtifactRegistry &interop_registry,
                             std::span<const ImportIR> imports)
    : owner_(owner), registry_(&registry), interop_registry_(&interop_registry) {
  imports_.reserve(imports.size());
  modules_.reserve(imports.size());
  check_irs_.reserve(imports.size());
  for (const auto &import : imports) {
    const auto *interface = registry.tryGet(import.interface_id);
    const auto id = imports_.add(import);
    if (interface) {
      const auto lookup_name = import.alias.hasValue()
                                   ? import.alias
                                   : interface->moduleName();
      modules_.emplace(lookup_name.index, id);
      if (interface->checkIRId().hasValue())
        check_irs_.emplace(interface->checkIRId().index, id);
    }
  }
}

ImportIRId ImportIRTable::findByModule(IdentifierId module) const {
  const auto found = modules_.find(module.index);
  return found == modules_.end() ? ImportIRId::invalid() : found->second;
}

const ImportIR *ImportIRTable::tryGet(ImportIRId id) const {
  return imports_.tryGet(id);
}

const PublicInterface *ImportIRTable::tryGetInterface(ImportIRId id) const {
  const auto *import = tryGet(id);
  return import ? registry_->tryGet(import->interface_id) : nullptr;
}

ImportIRInstId ImportIRTable::addInst(ImportIRInst inst) {
  return insts_.add(inst);
}

const ImportIRInst *ImportIRTable::tryGetInst(ImportIRInstId id) const {
  return insts_.tryGet(id);
}

const PublicFunctionBinding *
ImportIRTable::tryGetFunction(ImportIRInst inst) const {
  const auto *interface = tryGetInterface(inst.ir);
  if (!interface || inst.binding.index >= interface->bindingCount())
    return nullptr;
  return &interface->function(inst.binding);
}

PublicEntityId ImportIRTable::canonicalEntity(ImportIRInstId id,
                                              std::string &error) const {
  error.clear();
  const auto *import_inst = tryGetInst(id);
  const auto *binding = import_inst ? tryGetFunction(*import_inst) : nullptr;
  if (!import_inst || !binding) {
    error = "import instruction references a missing public binding";
    return PublicEntityId::invalid();
  }
  if (!registry_->tryGetEntity(binding->canonical_entity)) {
    error = "import instruction references a missing canonical entity";
    return PublicEntityId::invalid();
  }
  return binding->canonical_entity;
}

bool ImportIRTable::verify(std::string &error) const {
  error.clear();
  std::size_t session_local_imports = 0;
  std::unordered_set<std::uint32_t> lookup_names;
  for (std::uint32_t index = 0; index < imports_.size(); ++index) {
    const auto *import = imports_.tryGet(ImportIRId(index));
    const auto *interface =
        import ? registry_->tryGet(import->interface_id) : nullptr;
    if (!interface) {
      error = "import IR table references a missing public interface";
      return false;
    }
    const auto lookup_name =
        import->alias.hasValue() ? import->alias : interface->moduleName();
    if (!lookup_name.hasValue() ||
        !lookup_names.insert(lookup_name.index).second) {
      error = "import IR table contains a duplicate module lookup name";
      return false;
    }
    session_local_imports +=
        interface->checkIRId().hasValue() ? 1U : 0U;
  }
  if (!owner_.hasValue() || modules_.size() != imports_.size() ||
      check_irs_.size() != session_local_imports) {
    error = "import IR table has invalid or duplicate interface identities";
    return false;
  }
  for (std::uint32_t index = 0; index < imports_.size(); ++index) {
    const auto *interface = tryGetInterface(ImportIRId(index));
    if (!interface || interface->checkIRId() == owner_ ||
        !interface->verify(error)) {
      if (error.empty())
        error = "import IR table contains an invalid interface";
      return false;
    }
  }
  for (std::uint32_t index = 0; index < insts_.size(); ++index) {
    const auto id = ImportIRInstId(index);
    const auto entity = canonicalEntity(id, error);
    if (!entity.hasValue())
      return false;
    const auto *source_inst = tryGetInst(id);
    const auto *source_interface = tryGetInterface(source_inst->ir);
    const auto *source = tryGetFunction(*source_inst);
    const auto *target = registry_->tryGetEntity(entity);
    if (!source_interface || !source || !target ||
        (target->interop_artifact &&
         !interop_registry_->resolve(*target->interop_artifact)) ||
        source->return_type != target->return_type) {
      error = target && target->interop_artifact &&
                      !interop_registry_->resolve(*target->interop_artifact)
                  ? "import instruction references a missing interop artifact"
                  : "import instruction does not match its canonical entity";
      return false;
    }
    const auto source_parameters =
        source_interface->parameterTypes(source->parameters);
    if (source_parameters.size() != target->parameters.size() ||
        !std::equal(source_parameters.begin(), source_parameters.end(),
                    target->parameters.begin())) {
      error = "import instruction does not match its canonical signature";
      return false;
    }
  }
  return true;
}

void ImportIRTable::collectMetrics(core::CompilerMetrics &metrics,
                                   std::string_view label) const {
  imports_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "interfaces"));
  insts_.collectMetrics(
      metrics, core::CompilerMetrics::childLabel(label, "instructions"));
}

} // namespace chtholly::compiler
