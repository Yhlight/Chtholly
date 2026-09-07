#include "chtholly/Driver/CFFITool.h"
#include "chtholly/Compiler/CFDL.h"
#include "chtholly/Compiler/Source.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <map>
#include <ranges>
#include <set>
#include <sstream>

namespace chtholly {
namespace {

constexpr std::string_view StateHeader = "CHCFFIS5";
constexpr std::size_t MaxStateBytes = 64U * 1024U * 1024U;
constexpr std::size_t MaxGeneratedBytes = 32U * 1024U * 1024U;

struct RegenerationState {
  std::string target;
  std::string module;
  std::string config_digest;
  std::string toolchain_digest;
  std::string sdk_digest;
  std::string generated;
};

struct Patch {
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  std::string replacement;
};

std::string normalizedType(std::string value) {
  value.erase(std::remove_if(value.begin(), value.end(),
                             [](unsigned char ch) {
                               return ch == ' ' || ch == '\t' || ch == '\r' ||
                                      ch == '\n';
                             }),
              value.end());
  return value;
}

std::string flowPhysical(const compiler::CFDLFlowTypeSyntax &type) {
  auto value = normalizedType(type.physical_type);
  using Q = compiler::CFDLFlowQualifier;
  if (type.qualifier == Q::Ref || type.qualifier == Q::RefMut ||
      type.qualifier == Q::Out || type.qualifier == Q::InOut)
    value.push_back('*');
  return value;
}

bool parseCFDLText(std::string_view path, std::string_view text,
                   compiler::CFDLSyntaxFile &file, std::string &error) {
  compiler::SourceBuffer source{
      compiler::SourceInput(std::string(path), std::string(text))};
  std::vector<compiler::CFDLDiagnostic> diagnostics;
  if (!compiler::parseCFDL(source, file, diagnostics)) {
    error = "CFDL regeneration input failed epoch-13 parsing";
    return false;
  }
  return true;
}

std::string encodeState(const CFFIConfig &config, std::string_view generated,
                        std::string &error) {
  auto config_text = readTextFile(config.path, error);
  if (!config_text)
    return {};
  if (generated.size() > MaxGeneratedBytes) {
    error = "CFFI regeneration baseline exceeds its input budget";
    return {};
  }
  std::ostringstream out;
  out << StateHeader << '\n'
      << "target\t" << config.target << '\n'
      << "module\t" << config.module << '\n'
      << "config\t" << sha256Hex(*config_text) << '\n'
      << "toolchain\t" << config.toolchain.fingerprint << '\n'
      << "sdk\t" << config.toolchain.sdk_fingerprint << '\n'
      << "generated\t" << sha256Hex(generated) << '\n'
      << "payload-size\t" << generated.size() << "\n\n";
  out.write(generated.data(), static_cast<std::streamsize>(generated.size()));
  return out.str();
}

bool decodeState(std::string_view text, RegenerationState &state,
                 std::string &error) {
  if (text.size() > MaxStateBytes) {
    error = "CFFI regeneration state exceeds its input budget";
    return false;
  }
  const auto separator = text.find("\n\n");
  if (separator == std::string_view::npos) {
    error = "CFFI regeneration state is truncated";
    return false;
  }
  std::vector<std::string> lines;
  std::istringstream input{std::string(text.substr(0, separator))};
  for (std::string line; std::getline(input, line);)
    lines.push_back(std::move(line));
  if (lines.size() != 8 || lines[0] != StateHeader) {
    error = "CFFI regeneration state has an unsupported header";
    return false;
  }
  const auto field = [&](std::size_t index, std::string_view name,
                         std::string &value) {
    const auto prefix = std::string(name) + '\t';
    if (!lines[index].starts_with(prefix))
      return false;
    value = lines[index].substr(prefix.size());
    return !value.empty();
  };
  std::string generated_digest, payload_size_text;
  if (!field(1, "target", state.target) || !field(2, "module", state.module) ||
      !field(3, "config", state.config_digest) ||
      !field(4, "toolchain", state.toolchain_digest) ||
      !field(5, "sdk", state.sdk_digest) ||
      !field(6, "generated", generated_digest) ||
      !field(7, "payload-size", payload_size_text)) {
    error = "CFFI regeneration state fields are invalid";
    return false;
  }
  const auto is_digest = [](std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](char ch) {
             return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
           });
  };
  if (!is_digest(state.config_digest) || !is_digest(state.toolchain_digest) ||
      !is_digest(state.sdk_digest) || !is_digest(generated_digest)) {
    error = "CFFI regeneration state contains an invalid digest";
    return false;
  }
  std::size_t payload_size = 0;
  const auto parsed = std::from_chars(
      payload_size_text.data(),
      payload_size_text.data() + payload_size_text.size(), payload_size);
  const auto payload = text.substr(separator + 2);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != payload_size_text.data() + payload_size_text.size() ||
      payload_size != payload.size() || payload_size > MaxGeneratedBytes ||
      sha256Hex(payload) != generated_digest) {
    error = "CFFI regeneration state payload identity is invalid";
    return false;
  }
  state.generated.assign(payload);
  return true;
}

bool sameTypeMechanical(const compiler::CFDLForeignTypeSyntax &lhs,
                        const compiler::CFDLForeignTypeSyntax &rhs) {
  if (lhs.name != rhs.name || lhs.carrier_kind != rhs.carrier_kind ||
      normalizedType(lhs.scalar_carrier) !=
          normalizedType(rhs.scalar_carrier) ||
      lhs.fields.size() != rhs.fields.size() ||
      lhs.enum_constants.size() != rhs.enum_constants.size())
    return false;
  for (std::size_t index = 0; index < lhs.fields.size(); ++index)
    if (lhs.fields[index].name != rhs.fields[index].name ||
        normalizedType(lhs.fields[index].physical_type) !=
            normalizedType(rhs.fields[index].physical_type))
      return false;
  for (std::size_t index = 0; index < lhs.enum_constants.size(); ++index)
    if (lhs.enum_constants[index].name != rhs.enum_constants[index].name ||
        lhs.enum_constants[index].value != rhs.enum_constants[index].value)
      return false;
  return true;
}

bool sameCallableMechanical(const compiler::CFDLCallableSyntax &lhs,
                            const compiler::CFDLCallableSyntax &rhs) {
  if (lhs.name != rhs.name || lhs.external_symbol != rhs.external_symbol ||
      lhs.calling_convention != rhs.calling_convention ||
      lhs.parameters.size() != rhs.parameters.size() ||
      flowPhysical(lhs.result) != flowPhysical(rhs.result))
    return false;
  for (std::size_t index = 0; index < lhs.parameters.size(); ++index)
    if (lhs.parameters[index].name != rhs.parameters[index].name ||
        flowPhysical(lhs.parameters[index].type) !=
            flowPhysical(rhs.parameters[index].type))
      return false;
  return true;
}

bool sameConstantMechanical(const compiler::CFDLForeignConstantSyntax &lhs,
                            const compiler::CFDLForeignConstantSyntax &rhs) {
  return lhs.name == rhs.name &&
         normalizedType(lhs.physical_type) ==
             normalizedType(rhs.physical_type) &&
         lhs.kind == rhs.kind && lhs.integer_payload == rhs.integer_payload &&
         lhs.integer_negative == rhs.integer_negative &&
         lhs.bool_value == rhs.bool_value;
}

bool hasTypeOverlay(const compiler::CFDLForeignTypeSyntax &type) {
  return type.invalid_kind != compiler::CFDLForeignInvalidKind::None;
}

bool hasCallableOverlay(const compiler::CFDLCallableSyntax &callable) {
  if (callable.result.qualifier != compiler::CFDLFlowQualifier::Value ||
      !callable.where_facts.empty() || callable.error_contract ||
      callable.outcome_contract)
    return true;
  return std::ranges::any_of(callable.parameters, [](const auto &parameter) {
    return parameter.type.qualifier != compiler::CFDLFlowQualifier::Value;
  });
}

bool factReferences(const compiler::CFDLWhereFactSyntax &fact,
                    std::string_view name) {
  if (fact.subject == name)
    return true;
  return (fact.kind == compiler::CFDLWhereFactKind::Stores ||
          fact.kind == compiler::CFDLWhereFactKind::Derives) &&
         fact.target == name;
}

void renameFact(compiler::CFDLWhereFactSyntax &fact, std::string_view old_name,
                std::string_view new_name) {
  if (fact.subject == old_name)
    fact.subject = new_name;
  if ((fact.kind == compiler::CFDLWhereFactKind::Stores ||
       fact.kind == compiler::CFDLWhereFactKind::Derives) &&
      fact.target == old_name)
    fact.target = new_name;
}

template <typename T>
std::map<std::string, const T *> byName(const std::vector<T> &values) {
  std::map<std::string, const T *> result;
  for (const auto &value : values)
    result.emplace(value.name, &value);
  return result;
}

bool hasUniqueDeclarationKeys(const compiler::CFDLSyntaxFile &file) {
  return byName(file.foreign_types).size() == file.foreign_types.size() &&
         byName(file.foreign_constants).size() ==
             file.foreign_constants.size() &&
         byName(file.callables).size() == file.callables.size();
}

void addChange(CFFIRegeneration &result, CFFIRegenerationChangeKind kind,
               std::string declaration_kind, std::string key,
               std::string detail = {}) {
  result.changes.push_back(
      {kind, std::move(declaration_kind), std::move(key), std::move(detail)});
}

bool validateMerged(std::string_view source,
                    const compiler::CFDLSyntaxFile &expected, std::string &error) {
  compiler::CFDLSyntaxFile merged;
  if (!parseCFDLText("regenerated.cfdl", source, merged, error))
    return false;
  const std::array<const compiler::CFDLSyntaxFile *, 1> files{&merged};
  std::vector<compiler::CFDLPackageDiagnostic> diagnostics;
  if (!compiler::validateCFDLPackageProtocol(files, diagnostics)) {
    error = "regenerated CFDL has an incomplete resource protocol";
    return false;
  }
  if (!hasUniqueDeclarationKeys(merged)) {
    error = "regenerated CFDL has duplicate declaration keys";
    return false;
  }
  for (const auto &type : expected.foreign_types) {
    const auto found = std::ranges::find(merged.foreign_types, type.name,
                                         &compiler::CFDLForeignTypeSyntax::name);
    if (found == merged.foreign_types.end() ||
        !sameTypeMechanical(type, *found)) {
      error = "regenerated CFDL lost mechanical type '" + type.name + "'";
      return false;
    }
  }
  for (const auto &callable : expected.callables) {
    const auto found = std::ranges::find(merged.callables, callable.name,
                                         &compiler::CFDLCallableSyntax::name);
    if (found == merged.callables.end() ||
        !sameCallableMechanical(callable, *found)) {
      error =
          "regenerated CFDL lost mechanical callable '" + callable.name + "'";
      return false;
    }
  }
  for (const auto &constant : expected.foreign_constants) {
    const auto found =
        std::ranges::find(merged.foreign_constants, constant.name,
                          &compiler::CFDLForeignConstantSyntax::name);
    if (found == merged.foreign_constants.end() ||
        !sameConstantMechanical(constant, *found)) {
      error =
          "regenerated CFDL lost mechanical constant '" + constant.name + "'";
      return false;
    }
  }
  return true;
}

bool currentMatchesNew(const compiler::CFDLSyntaxFile &current,
                       const compiler::CFDLSyntaxFile &expected) {
  if (current.module_name != expected.module_name)
    return false;
  for (const auto &type : expected.foreign_types) {
    const auto found = std::ranges::find(current.foreign_types, type.name,
                                         &compiler::CFDLForeignTypeSyntax::name);
    if (found == current.foreign_types.end() ||
        !sameTypeMechanical(type, *found))
      return false;
  }
  for (const auto &callable : expected.callables) {
    const auto found = std::ranges::find(current.callables, callable.name,
                                         &compiler::CFDLCallableSyntax::name);
    if (found == current.callables.end() ||
        !sameCallableMechanical(callable, *found))
      return false;
  }
  for (const auto &constant : expected.foreign_constants) {
    const auto found =
        std::ranges::find(current.foreign_constants, constant.name,
                          &compiler::CFDLForeignConstantSyntax::name);
    if (found == current.foreign_constants.end() ||
        !sameConstantMechanical(constant, *found))
      return false;
  }
  return true;
}

bool isRecoveredWrite(const compiler::CFDLSyntaxFile &base,
                      const compiler::CFDLSyntaxFile &current,
                      const compiler::CFDLSyntaxFile &expected) {
  if (!currentMatchesNew(current, expected))
    return false;
  const auto expected_types = byName(expected.foreign_types);
  const auto current_types = byName(current.foreign_types);
  for (const auto &type : base.foreign_types)
    if (!expected_types.contains(type.name) &&
        current_types.contains(type.name))
      return false;
  const auto expected_calls = byName(expected.callables);
  const auto current_calls = byName(current.callables);
  for (const auto &callable : base.callables)
    if (!expected_calls.contains(callable.name) &&
        current_calls.contains(callable.name))
      return false;
  const auto expected_constants = byName(expected.foreign_constants);
  const auto current_constants = byName(current.foreign_constants);
  for (const auto &constant : base.foreign_constants)
    if (!expected_constants.contains(constant.name) &&
        current_constants.contains(constant.name))
      return false;
  return true;
}

std::string applyPatches(std::string source, std::vector<Patch> patches,
                         std::string &error) {
  std::ranges::sort(patches, std::greater{}, &Patch::begin);
  for (const auto &patch : patches) {
    if (patch.begin > patch.end || patch.end > source.size()) {
      error = "CFDL declaration source range is invalid";
      return {};
    }
    source.replace(patch.begin, patch.end - patch.begin, patch.replacement);
  }
  return source;
}

std::string uniqueTemporaryPath(std::string_view path) {
  static std::atomic<std::uint64_t> sequence = 0;
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::string(path) + ".tmp-cffi-" + std::to_string(stamp) + "-" +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

bool atomicWrite(std::string_view path, std::string_view text,
                 std::string &error) {
  const auto temporary = uniqueTemporaryPath(path);
  if (!writeTextFile(temporary, std::string(text), error))
    return false;
  std::error_code file_error;
  if (!replaceFile(temporary, std::string(path), file_error)) {
    const auto replace_error = file_error;
    std::error_code cleanup_error;
    removeFile(temporary, cleanup_error);
    error =
        "failed to atomically replace CFFI output: " + replace_error.message();
    return false;
  }
  return true;
}

} // namespace

std::string_view cffiRegenerationChangeName(CFFIRegenerationChangeKind kind) {
  switch (kind) {
  case CFFIRegenerationChangeKind::Add:
    return "add";
  case CFFIRegenerationChangeKind::Remove:
    return "remove";
  case CFFIRegenerationChangeKind::MechanicalUpdate:
    return "mechanical-update";
  case CFFIRegenerationChangeKind::ParameterRename:
    return "parameter-rename";
  case CFFIRegenerationChangeKind::SemanticPreserved:
    return "semantic-preserved";
  case CFFIRegenerationChangeKind::ManualRetained:
    return "manual-retained";
  case CFFIRegenerationChangeKind::StateBootstrap:
    return "state-bootstrap";
  case CFFIRegenerationChangeKind::StateUpdate:
    return "state-update";
  case CFFIRegenerationChangeKind::Count:
    return "invalid";
  }
  return "invalid";
}

std::string defaultCFFIStatePath(std::string_view cfdl_path) {
  std::string path(cfdl_path);
  const auto separator = path.find_last_of("/\\");
  const auto extension = path.rfind(".cfdl");
  if (extension != std::string::npos && extension + 5 == path.size() &&
      (separator == std::string::npos || extension > separator))
    path.replace(extension, 5, ".cffi-state");
  else
    path += ".cffi-state";
  return path;
}

bool generateCFFIWithState(const CFFIConfig &config, CFFIGeneration &generation,
                           std::string &error) {
  generation = {};
  if (!generateCFFI(config, generation.source, error))
    return false;
  generation.state = encodeState(config, generation.source, error);
  return !generation.state.empty();
}

bool regenerateCFFI(const CFFIConfig &config, const std::string &cfdl_path,
                    const std::string &state_path, CFFIRegeneration &result,
                    std::string &error) {
  result = {};
  auto current_text = readTextFile(cfdl_path, error);
  if (!current_text)
    return false;
  result.input_digest = sha256Hex(*current_text);

  CFFIGeneration generated;
  if (!generateCFFIWithState(config, generated, error))
    return false;
  compiler::CFDLSyntaxFile current_file, new_file;
  if (!parseCFDLText(cfdl_path, *current_text, current_file, error) ||
      !parseCFDLText("new-generated.cfdl", generated.source, new_file, error))
    return false;
  if (!hasUniqueDeclarationKeys(current_file) ||
      !hasUniqueDeclarationKeys(new_file)) {
    error = "CFDL regeneration requires unique declaration keys";
    return false;
  }

  std::error_code file_error;
  const auto state_file = pathForFileSystem(state_path);
  const bool state_exists = std::filesystem::exists(state_file, file_error);
  if (file_error) {
    error =
        "failed to inspect CFFI regeneration state: " + file_error.message();
    return false;
  }
  const bool has_state =
      state_exists && std::filesystem::is_regular_file(state_file, file_error);
  if (file_error) {
    error =
        "failed to inspect CFFI regeneration state: " + file_error.message();
    return false;
  }
  if (!has_state) {
    if (!currentMatchesNew(current_file, new_file)) {
      error = "CFFI regeneration state is missing and the current binding "
              "does not match the configured headers";
      return false;
    }
    result.source = *current_text;
    result.state = std::move(generated.state);
    result.changed = true;
    addChange(result, CFFIRegenerationChangeKind::StateBootstrap, "state",
              config.module);
    return true;
  }

  auto state_text = readTextFile(state_path, error);
  if (!state_text)
    return false;
  result.state_input_digest = sha256Hex(*state_text);
  RegenerationState old_state;
  if (!decodeState(*state_text, old_state, error))
    return false;
  if (old_state.target != config.target || old_state.module != config.module) {
    error = "CFFI regeneration state target or module does not match config";
    return false;
  }
  compiler::CFDLSyntaxFile base_file;
  if (!parseCFDLText("state-baseline.cfdl", old_state.generated, base_file,
                     error))
    return false;
  if (!hasUniqueDeclarationKeys(base_file)) {
    error = "CFFI regeneration state has duplicate declaration keys";
    return false;
  }

  if (current_file.module_name != base_file.module_name ||
      new_file.module_name != base_file.module_name) {
    error = "CFDL module identity changed during regeneration";
    return false;
  }
  if (old_state.generated != generated.source &&
      isRecoveredWrite(base_file, current_file, new_file)) {
    result.source = *current_text;
    result.state = std::move(generated.state);
    result.changed = result.state != *state_text;
    if (result.changed)
      addChange(result, CFFIRegenerationChangeKind::StateUpdate, "state",
                config.module, "recovered-current-mechanical-model");
    return true;
  }
  const auto base_types = byName(base_file.foreign_types);
  const auto current_types = byName(current_file.foreign_types);
  const auto new_types = byName(new_file.foreign_types);
  const auto base_calls = byName(base_file.callables);
  const auto current_calls = byName(current_file.callables);
  const auto new_calls = byName(new_file.callables);
  const auto base_constants = byName(base_file.foreign_constants);
  const auto current_constants = byName(current_file.foreign_constants);
  const auto new_constants = byName(new_file.foreign_constants);
  std::vector<Patch> patches;
  std::set<std::string> new_type_keys, new_call_keys, new_constant_keys;

  for (const auto &[key, base] : base_constants) {
    const auto current = current_constants.find(key);
    const auto replacement = new_constants.find(key);
    if (current == current_constants.end()) {
      if (replacement != new_constants.end()) {
        error = "managed CFDL constant was deleted by the user: " + key;
        return false;
      }
      continue;
    }
    if (!sameConstantMechanical(*base, *current->second)) {
      error = "managed CFDL constant mechanical facts were edited: " + key;
      return false;
    }
    if (replacement == new_constants.end()) {
      patches.push_back(
          {current->second->offset, current->second->end_offset, {}});
      addChange(result, CFFIRegenerationChangeKind::Remove, "constant", key);
      continue;
    }
    const auto rendered = compiler::renderCFDLForeignConstant(*replacement->second);
    const auto existing = current_text->substr(current->second->offset,
                                               current->second->end_offset -
                                                   current->second->offset);
    if (rendered != existing) {
      patches.push_back(
          {current->second->offset, current->second->end_offset, rendered});
      addChange(result, CFFIRegenerationChangeKind::MechanicalUpdate,
                "constant", key);
    }
  }
  for (const auto &[key, constant] : new_constants) {
    if (base_constants.contains(key))
      continue;
    if (current_constants.contains(key)) {
      error = "new generated C constant conflicts with a manual declaration: " +
              key;
      return false;
    }
    new_constant_keys.insert(key);
    addChange(result, CFFIRegenerationChangeKind::Add, "constant", key);
  }

  for (const auto &[key, base] : base_types) {
    const auto current = current_types.find(key);
    const auto new_it = new_types.find(key);
    if (current == current_types.end()) {
      if (new_it != new_types.end()) {
        error = "managed CFDL type was deleted by the user: " + key;
        return false;
      }
      continue;
    }
    if (!sameTypeMechanical(*base, *current->second)) {
      error = "managed CFDL type mechanical facts were edited: " + key;
      return false;
    }
    if (new_it == new_types.end()) {
      if (hasTypeOverlay(*current->second)) {
        error = "removed C type retains an invalid-sentinel overlay: " + key;
        return false;
      }
      patches.push_back(
          {current->second->offset, current->second->end_offset, {}});
      addChange(result, CFFIRegenerationChangeKind::Remove, "type", key);
      continue;
    }
    auto merged = *new_it->second;
    if (hasTypeOverlay(*current->second)) {
      if (!sameTypeMechanical(*base, *new_it->second)) {
        error = "C type with an invalid sentinel changed carrier: " + key;
        return false;
      }
      merged.invalid_kind = current->second->invalid_kind;
      merged.invalid_integer = current->second->invalid_integer;
      addChange(result, CFFIRegenerationChangeKind::SemanticPreserved, "type",
                key, "invalid-sentinel");
    }
    const auto rendered = compiler::renderCFDLForeignType(merged);
    const auto existing = current_text->substr(current->second->offset,
                                               current->second->end_offset -
                                                   current->second->offset);
    if (rendered != existing) {
      patches.push_back(
          {current->second->offset, current->second->end_offset, rendered});
      addChange(result, CFFIRegenerationChangeKind::MechanicalUpdate, "type",
                key);
    }
  }

  for (const auto &[key, new_type] : new_types) {
    if (base_types.contains(key))
      continue;
    if (current_types.contains(key)) {
      error =
          "new generated C type conflicts with a manual declaration: " + key;
      return false;
    }
    new_type_keys.insert(key);
    addChange(result, CFFIRegenerationChangeKind::Add, "type", key);
  }

  for (const auto &[key, base] : base_calls) {
    const auto current = current_calls.find(key);
    const auto new_it = new_calls.find(key);
    if (current == current_calls.end()) {
      if (new_it != new_calls.end()) {
        error = "managed CFDL callable was deleted by the user: " + key;
        return false;
      }
      continue;
    }
    if (!sameCallableMechanical(*base, *current->second)) {
      error = "managed CFDL callable mechanical facts were edited: " + key;
      return false;
    }
    if (new_it == new_calls.end()) {
      if (hasCallableOverlay(*current->second)) {
        error = "removed C callable retains resource-flow semantics: " + key;
        return false;
      }
      patches.push_back(
          {current->second->offset, current->second->end_offset, {}});
      addChange(result, CFFIRegenerationChangeKind::Remove, "function", key);
      continue;
    }
    auto merged = *new_it->second;
    if (base->parameters.size() == new_it->second->parameters.size()) {
      for (std::size_t index = 0; index < base->parameters.size(); ++index) {
        const auto &old_parameter = base->parameters[index];
        const auto &new_parameter = new_it->second->parameters[index];
        if (old_parameter.name != new_parameter.name) {
          const auto old_name_in_new =
              std::ranges::find(new_it->second->parameters, old_parameter.name,
                                &compiler::CFDLCallableParameterSyntax::name);
          const auto new_name_in_old =
              std::ranges::find(base->parameters, new_parameter.name,
                                &compiler::CFDLCallableParameterSyntax::name);
          if ((old_name_in_new != new_it->second->parameters.end() &&
               std::distance(new_it->second->parameters.begin(),
                             old_name_in_new) !=
                   static_cast<std::ptrdiff_t>(index)) ||
              (new_name_in_old != base->parameters.end() &&
               std::distance(base->parameters.begin(), new_name_in_old) !=
                   static_cast<std::ptrdiff_t>(index))) {
            error =
                "C callable parameter rename is ambiguous or reordered: " + key;
            return false;
          }
        }
        if (old_parameter.name != new_parameter.name &&
            flowPhysical(old_parameter.type) ==
                flowPhysical(new_parameter.type))
          addChange(result, CFFIRegenerationChangeKind::ParameterRename,
                    "function", key,
                    old_parameter.name + "->" + new_parameter.name);
      }
    }
    if (hasCallableOverlay(*current->second)) {
      if (base->parameters.size() != new_it->second->parameters.size()) {
        error = "C callable with resource semantics changed arity: " + key;
        return false;
      }
      merged.where_facts = current->second->where_facts;
      merged.error_contract = current->second->error_contract;
      merged.outcome_contract = current->second->outcome_contract;
      if ((current->second->error_contract ||
           current->second->outcome_contract) &&
          flowPhysical(base->result) != flowPhysical(new_it->second->result)) {
        error = "C callable changed a result-contract lane: " + key;
        return false;
      }
      for (std::size_t index = 0; index < base->parameters.size(); ++index) {
        const auto &old_parameter = base->parameters[index];
        const auto &current_parameter = current->second->parameters[index];
        const auto &new_parameter = new_it->second->parameters[index];
        const bool referenced =
            std::ranges::any_of(current->second->where_facts,
                                [&](const auto &fact) {
                                  return factReferences(fact,
                                                        old_parameter.name);
                                }) ||
            (current->second->outcome_contract &&
             (current->second->outcome_contract->buffer == old_parameter.name ||
              current->second->outcome_contract->capacity ==
                  old_parameter.name ||
              current->second->outcome_contract->count == old_parameter.name ||
              current->second->outcome_contract->context ==
                  old_parameter.name));
        if (flowPhysical(old_parameter.type) !=
                flowPhysical(new_parameter.type) &&
            (current_parameter.type.qualifier !=
                 compiler::CFDLFlowQualifier::Value ||
             referenced)) {
          error =
              "C callable changed a resource-bearing parameter lane: " + key +
              "/" + old_parameter.name;
          return false;
        }
        if (flowPhysical(old_parameter.type) ==
            flowPhysical(new_parameter.type))
          merged.parameters[index].type = current_parameter.type;
        if (old_parameter.name != new_parameter.name) {
          for (auto &fact : merged.where_facts)
            renameFact(fact, old_parameter.name, new_parameter.name);
          if (merged.outcome_contract) {
            if (merged.outcome_contract->buffer == old_parameter.name)
              merged.outcome_contract->buffer = new_parameter.name;
            if (merged.outcome_contract->capacity == old_parameter.name)
              merged.outcome_contract->capacity = new_parameter.name;
            if (merged.outcome_contract->count == old_parameter.name)
              merged.outcome_contract->count = new_parameter.name;
            if (merged.outcome_contract->context == old_parameter.name)
              merged.outcome_contract->context = new_parameter.name;
          }
        }
      }
      const bool result_referenced = std::ranges::any_of(
          current->second->where_facts,
          [](const auto &fact) { return fact.subject == "result"; });
      if (flowPhysical(base->result) != flowPhysical(new_it->second->result) &&
          (current->second->result.qualifier !=
               compiler::CFDLFlowQualifier::Value ||
           result_referenced)) {
        error = "C callable changed a resource-bearing result lane: " + key;
        return false;
      }
      if (flowPhysical(base->result) == flowPhysical(new_it->second->result))
        merged.result = current->second->result;
      addChange(result, CFFIRegenerationChangeKind::SemanticPreserved,
                "function", key,
                current->second->outcome_contract
                    ? "flow-qualifiers-where-error-and-outcome-contract"
                : current->second->error_contract
                    ? "flow-qualifiers-where-and-error-contract"
                    : "flow-qualifiers-and-where");
    }
    const auto rendered = compiler::renderCFDLCallable(merged);
    const auto existing = current_text->substr(current->second->offset,
                                               current->second->end_offset -
                                                   current->second->offset);
    if (rendered != existing) {
      patches.push_back(
          {current->second->offset, current->second->end_offset, rendered});
      addChange(result, CFFIRegenerationChangeKind::MechanicalUpdate,
                "function", key);
    }
  }

  for (const auto &[key, new_call] : new_calls) {
    if (base_calls.contains(key))
      continue;
    if (current_calls.contains(key)) {
      error = "new generated C callable conflicts with a manual declaration: " +
              key;
      return false;
    }
    new_call_keys.insert(key);
    addChange(result, CFFIRegenerationChangeKind::Add, "function", key);
  }
  for (const auto &[key, value] : current_types)
    if (!base_types.contains(key) && !new_types.contains(key))
      addChange(result, CFFIRegenerationChangeKind::ManualRetained, "type",
                key);
  for (const auto &[key, value] : current_calls)
    if (!base_calls.contains(key) && !new_calls.contains(key))
      addChange(result, CFFIRegenerationChangeKind::ManualRetained, "function",
                key);
  for (const auto &[key, value] : current_constants)
    if (!base_constants.contains(key) && !new_constants.contains(key))
      addChange(result, CFFIRegenerationChangeKind::ManualRetained, "constant",
                key);

  result.source = applyPatches(*current_text, std::move(patches), error);
  if (!error.empty())
    return false;
  for (const auto &key : new_type_keys) {
    if (!result.source.ends_with('\n'))
      result.source.push_back('\n');
    result.source +=
        "\n" + compiler::renderCFDLForeignType(*new_types.at(key)) + "\n";
  }
  for (const auto &key : new_call_keys) {
    if (!result.source.ends_with('\n'))
      result.source.push_back('\n');
    result.source += "\n" + compiler::renderCFDLCallable(*new_calls.at(key)) + "\n";
  }
  for (const auto &key : new_constant_keys) {
    if (!result.source.ends_with('\n'))
      result.source.push_back('\n');
    result.source +=
        "\n" + compiler::renderCFDLForeignConstant(*new_constants.at(key)) + "\n";
  }
  if (!validateMerged(result.source, new_file, error))
    return false;
  result.state = std::move(generated.state);
  if (result.state != *state_text && result.changes.empty())
    addChange(result, CFFIRegenerationChangeKind::StateUpdate, "state",
              config.module, "configuration-identity");
  result.changed =
      result.source != *current_text || result.state != *state_text;
  return true;
}

bool writeCFFIGeneration(const std::string &cfdl_path,
                         const std::string &state_path,
                         const CFFIGeneration &generation, std::string &error) {
  if (pathForFileSystem(cfdl_path).lexically_normal() ==
      pathForFileSystem(state_path).lexically_normal()) {
    error = "CFFI generation requires distinct CFDL and state paths";
    return false;
  }
  std::error_code file_error;
  const bool cfdl_exists =
      std::filesystem::exists(pathForFileSystem(cfdl_path), file_error);
  if (file_error) {
    error = "failed to inspect CFFI output: " + file_error.message();
    return false;
  }
  const bool state_exists =
      std::filesystem::exists(pathForFileSystem(state_path), file_error);
  if (file_error) {
    error = "failed to inspect CFFI state output: " + file_error.message();
    return false;
  }
  if (cfdl_exists || state_exists) {
    error = "CFFI generation refuses to overwrite an existing CFDL or state "
            "file";
    return false;
  }
  if (!atomicWrite(cfdl_path, generation.source, error))
    return false;
  if (!atomicWrite(state_path, generation.state, error)) {
    std::error_code cleanup_error;
    removeFile(cfdl_path, cleanup_error);
    return false;
  }
  return true;
}

bool applyCFFIRegeneration(const std::string &cfdl_path,
                           const std::string &state_path,
                           const CFFIRegeneration &regeneration,
                           std::string &error) {
  auto current = readTextFile(cfdl_path, error);
  if (!current || sha256Hex(*current) != regeneration.input_digest) {
    if (error.empty())
      error = "CFDL changed after regeneration planning";
    return false;
  }
  std::error_code file_error;
  const bool state_exists =
      std::filesystem::exists(pathForFileSystem(state_path), file_error);
  if (file_error) {
    error =
        "failed to inspect CFFI state before write: " + file_error.message();
    return false;
  }
  if (regeneration.state_input_digest) {
    auto current_state = readTextFile(state_path, error);
    if (!current_state ||
        sha256Hex(*current_state) != *regeneration.state_input_digest) {
      if (error.empty())
        error = "CFFI state changed after regeneration planning";
      return false;
    }
  } else if (state_exists) {
    error = "CFFI state appeared after regeneration planning";
    return false;
  }
  if (!regeneration.changed)
    return true;
  if (sha256Hex(regeneration.source) != regeneration.input_digest &&
      !atomicWrite(cfdl_path, regeneration.source, error))
    return false;
  return atomicWrite(state_path, regeneration.state, error);
}

} // namespace chtholly
