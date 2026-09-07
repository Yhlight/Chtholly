#pragma once

#include "chtholly/Core/ValueStore.h"
#include "chtholly/Compiler/PublicInterface.h"

#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>

namespace chtholly::compiler {

struct ImportIRId : core::IndexBase<ImportIRId> {
  using IndexBase::IndexBase;
};

struct ImportIRInstId : core::IndexBase<ImportIRInstId> {
  using IndexBase::IndexBase;
};

struct ImportIR {
  PublicInterfaceId interface_id;
  bool is_export = false;
  IdentifierId alias;
};

struct ImportIRInst {
  ImportIRId ir;
  PublicBindingId binding;

  friend bool operator==(const ImportIRInst &, const ImportIRInst &) = default;
};

struct ImportIRInstHash {
  std::size_t operator()(const ImportIRInst &value) const noexcept;
};

class ImportIRTable {
public:
  ImportIRTable(CheckIRId owner, const PublicInterfaceRegistry &registry,
                const interop::ArtifactRegistry &interop_registry,
                std::span<const ImportIR> imports = {});

  [[nodiscard]] ImportIRId findByModule(IdentifierId module) const;
  [[nodiscard]] const ImportIR *tryGet(ImportIRId id) const;
  [[nodiscard]] const PublicInterface *tryGetInterface(ImportIRId id) const;
  [[nodiscard]] ImportIRInstId addInst(ImportIRInst inst);
  [[nodiscard]] const ImportIRInst *tryGetInst(ImportIRInstId id) const;
  [[nodiscard]] PublicEntityId canonicalEntity(ImportIRInstId id,
                                               std::string &error) const;
  [[nodiscard]] const PublicFunctionBinding *
  tryGetFunction(ImportIRInst inst) const;
  [[nodiscard]] const PublicEntity *tryGetEntity(PublicEntityId id) const {
    return registry_->tryGetEntity(id);
  }
  [[nodiscard]] const PublicInterfaceRegistry &registry() const {
    return *registry_;
  }
  [[nodiscard]] const interop::ArtifactRegistry &interopRegistry() const {
    return *interop_registry_;
  }
  [[nodiscard]] std::size_t size() const {
    return imports_.size();
  }
  [[nodiscard]] std::size_t instCount() const {
    return insts_.size();
  }
  [[nodiscard]] bool verify(std::string &error) const;
  void collectMetrics(core::CompilerMetrics &metrics,
                      std::string_view label) const;

private:
  CheckIRId owner_;
  const PublicInterfaceRegistry *registry_;
  const interop::ArtifactRegistry *interop_registry_;
  core::ValueStore<ImportIRId, ImportIR> imports_;
  core::CanonicalValueStore<ImportIRInstId, ImportIRInst, ImportIRInstHash>
      insts_;
  std::unordered_map<std::uint32_t, ImportIRId> modules_;
  std::unordered_map<std::uint32_t, ImportIRId> check_irs_;
};

} // namespace chtholly::compiler
