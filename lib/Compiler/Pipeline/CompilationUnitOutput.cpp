#include "CompilationUnitInternal.h"

#include "chtholly/Compiler/Token.h"

#include <algorithm>
#include <sstream>

namespace chtholly::compiler {

std::string CompilationUnit::printTokens() const {
  if (impl_->kind == CompilationUnitKind::ForeignBinding)
    return printCFDLTokens(impl_->source);
  if (!impl_->tokens)
    return {};
  std::ostringstream out;
  for (std::uint32_t index = 0; index < impl_->tokens->size(); ++index) {
    const auto id = TokenId(index);
    out << index << ' ' << tokenKindName(impl_->tokens->get(id).kind);
    const auto text = impl_->tokens->text(id);
    if (!text.empty())
      out << " '" << text << '\'';
    out << '\n';
  }
  return out.str();
}

std::string CompilationUnit::printParseTree() const {
  if (impl_->cfdl_syntax)
    return printCFDLSyntax(*impl_->cfdl_syntax);
  return impl_->parse_tree ? impl_->parse_tree->print() : std::string{};
}

std::string CompilationUnit::printSemIR() const {
  return impl_->sem_ir ? impl_->sem_ir->print() : std::string{};
}

std::string CompilationUnit::printPublicInterface() const {
  const auto *interface = publicInterface();
  return interface ? interface->print() : std::string{};
}

std::string CompilationUnit::printLowIR() const {
  return impl_->low_ir ? impl_->low_ir->print() : std::string{};
}

std::string CompilationUnit::printForeignProtocols() const {
  if (!impl_->sem_ir)
    return {};
  std::ostringstream out;
  for (std::uint32_t index = 0; index < impl_->sem_ir->nominalTypeCount();
       ++index) {
    const auto &nominal = impl_->sem_ir->nominalType(NominalTypeId(index));
    if (nominal.kind != NominalKind::ForeignResource ||
        !nominal.foreign_resource_protocol.hasValue())
      continue;
    const auto name =
        impl_->sem_ir->identifier(impl_->sem_ir->name(nominal.name).text);
    const auto &protocol =
        impl_->sem_ir->genericValues()
            .foreignResourceProtocol(nominal.foreign_resource_protocol)
            .facts;
    out << "resource " << name << '\n'
        << "  normalized " << encodeForeignResourceProtocol(protocol) << '\n';
    for (const auto &operation : nominal.foreign_resource_operations)
      out << "  operation "
          << impl_->sem_ir->identifier(impl_->sem_ir->name(operation.name).text)
          << " role=" << static_cast<unsigned>(operation.role) << '\n';
  }
  return out.str();
}

std::string CompilationUnit::printLLVM() const {
  return impl_->llvm_module ? impl_->llvm_module->print() : std::string{};
}

std::string CompilationUnit::emitObject(std::string &error) const {
  if (!impl_->object_bytes.empty()) {
    error.clear();
    return impl_->object_bytes;
  }
  if (!impl_->llvm_module) {
    error = "compiler compilation did not produce an LLVM module";
    return {};
  }
  return impl_->llvm_module->emitObject(error);
}

std::string CompilationUnit::metricsJson() const {
  core::CompilerMetrics metrics;
  impl_->values->collectMetrics(metrics, "shared_values");
  impl_->arena.collectMetrics(metrics, "unit.arena");
  if (impl_->tokens)
    impl_->tokens->collectMetrics(metrics, "unit.tokens");
  if (impl_->parse_tree)
    impl_->parse_tree->collectMetrics(metrics, "unit.parse_tree");
  if (impl_->sem_ir)
    impl_->sem_ir->collectMetrics(metrics, "unit.sem_ir");
  if (impl_->low_ir)
    impl_->low_ir->collectMetrics(metrics, "unit.low_ir");
  return metrics.toJson(impl_->source.filename());
}

std::string CompilationUnit::analysisMetricsJson() const {
  const UnitAnalysisMetrics unit{
      impl_->source.filename(), impl_->reused,
      impl_->sem_ir ? &impl_->sem_ir->analysisMetrics() : nullptr};
  return chtholly::compiler::analysisMetricsJson(std::span(&unit, 1));
}

std::string CompilationSession::printForeignProtocols() const {
  std::ostringstream out;
  for (const auto &unit : impl_->units) {
    const auto active = unit->printForeignProtocols();
    if (!active.empty()) {
      out << active;
      continue;
    }
    if (!impl_->package_check_artifact)
      continue;
    const auto *module =
        impl_->package_check_artifact->findModule(unit->moduleName());
    if (!module)
      continue;
    for (const auto &nominal : module->public_interface.nominalTypes()) {
      if (nominal.kind != NominalKind::ForeignResource ||
          nominal.foreign_resource_protocol.roles.empty())
        continue;
      out << "resource " << nominal.entity.canonical_name << '\n'
          << "  normalized "
          << encodeForeignResourceProtocol(nominal.foreign_resource_protocol)
          << '\n';
      for (const auto &operation : nominal.foreign_resource_operations)
        out << "  operation " << operation.name
            << " role=" << static_cast<unsigned>(operation.role) << '\n';
    }
  }
  return out.str();
}

std::string CompilationSession::metricsJson() const {
  core::CompilerMetrics metrics;
  impl_->values->collectMetrics(metrics, "shared_values");
  for (const auto &unit : impl_->units) {
    const auto prefix = "unit" + std::to_string(unit->checkIRId().index);
    unit->impl_->arena.collectMetrics(
        metrics, core::CompilerMetrics::childLabel(prefix, "arena"));
    if (unit->impl_->tokens)
      unit->impl_->tokens->collectMetrics(
          metrics, core::CompilerMetrics::childLabel(prefix, "tokens"));
    if (unit->impl_->parse_tree)
      unit->impl_->parse_tree->collectMetrics(
          metrics, core::CompilerMetrics::childLabel(prefix, "parse_tree"));
    if (unit->impl_->sem_ir)
      unit->impl_->sem_ir->collectMetrics(
          metrics, core::CompilerMetrics::childLabel(prefix, "sem_ir"));
    if (unit->impl_->low_ir)
      unit->impl_->low_ir->collectMetrics(
          metrics, core::CompilerMetrics::childLabel(prefix, "low_ir"));
  }
  impl_->public_interfaces->collectMetrics(metrics, "public_interfaces");
  if (impl_->compilation_plan)
    impl_->compilation_plan->collectMetrics(metrics, "incremental_plan");
  if (impl_->package_manifest)
    impl_->package_manifest->collectMetrics(metrics, "package_manifest");
  return metrics.toJson("compilation-session");
}

std::string CompilationSession::analysisMetricsJson() const {
  return chtholly::compiler::analysisMetricsJson(analysisMetricUnits());
}

std::vector<UnitAnalysisMetrics>
CompilationSession::analysisMetricUnits() const {
  std::vector<UnitAnalysisMetrics> units;
  units.reserve(impl_->units.size());
  for (const auto &unit : impl_->units)
    units.push_back(
        {unit->sourcePath(), unit->wasReused(),
         unit->semIR() ? &unit->semIR()->analysisMetrics() : nullptr});
  std::ranges::sort(units, {}, &UnitAnalysisMetrics::unit);
  return units;
}

} // namespace chtholly::compiler
