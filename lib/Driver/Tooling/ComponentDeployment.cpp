#include "chtholly/Driver/ComponentDeployment.h"

#include "chtholly/Support/FileSystem.h"

#include <fstream>

namespace chtholly {
namespace {
std::filesystem::path rootPath(const std::string &root) { return pathForFileSystem(root); }
std::filesystem::path activePath(const std::filesystem::path &root) { return root / "active"; }
bool readActive(const std::filesystem::path &root, std::string &id) {
  std::ifstream in(activePath(root)); return static_cast<bool>(in >> id);
}
bool writeAtomic(const std::filesystem::path &path, std::string_view text, std::string &error) {
  const auto tmp = path.string() + ".tmp";
  if (!writeTextFile(tmp, std::string(text), error)) return false;
  std::error_code ec; std::filesystem::rename(tmp, path, ec);
  if (ec) { std::filesystem::remove(path, ec); ec.clear(); std::filesystem::rename(tmp, path, ec); }
  if (ec) { std::filesystem::remove(tmp, ec); error = "failed to atomically update deployment state"; return false; }
  return true;
}
bool loadGeneration(const std::filesystem::path &root, std::string_view id,
                    ComponentGenerationInfo &generation, std::string &error) {
  const auto dir = root / "generations" / std::string(id);
  if (!std::filesystem::is_directory(dir)) { error = "component generation is missing"; return false; }
  if (!loadDeploymentManifest(dir / "component.toml", generation.manifest, error) ||
      !validateDeploymentFiles(generation.manifest, error)) return false;
  generation.id = std::string(id); return true;
}
}

bool installComponentGeneration(const std::string &root_text, const std::string &manifest_path,
                                ComponentGenerationInfo &generation, std::string &error) {
  DeploymentManifest source;
  if (!loadDeploymentManifest(pathForFileSystem(manifest_path), source, error) ||
      !validateDeploymentFiles(source, error)) return false;
  const auto root = rootPath(root_text); std::error_code ec;
  std::filesystem::create_directories(root / "generations", ec);
  if (ec) { error = "failed to create component deployment root"; return false; }
  const auto id = source.version + "-" + source.contract_digest;
  const auto dir = root / "generations" / id;
  std::filesystem::create_directories(dir, ec);
  if (ec) { error = "failed to create component generation"; return false; }
  std::filesystem::copy_file(pathForFileSystem(manifest_path), dir / "component.toml", std::filesystem::copy_options::overwrite_existing, ec);
  const auto library_rel = std::filesystem::relative(source.library, source.root, ec);
  const auto contract_rel = std::filesystem::relative(source.contract, source.root, ec);
  if (ec || library_rel.empty() || contract_rel.empty()) { error = "failed to resolve component generation paths"; return false; }
  std::filesystem::create_directories((dir / library_rel).parent_path(), ec);
  std::filesystem::create_directories((dir / contract_rel).parent_path(), ec);
  std::filesystem::copy_file(source.library, dir / library_rel, std::filesystem::copy_options::overwrite_existing, ec);
  std::filesystem::copy_file(source.contract, dir / contract_rel, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) { error = "failed to copy component generation files"; return false; }
  if (!loadGeneration(root, id, generation, error)) return false;
  return true;
}

bool activateComponentGeneration(const std::string &root_text, std::string_view id, std::string &error) {
  const auto root = rootPath(root_text); ComponentGenerationInfo generation;
  if (!loadGeneration(root, id, generation, error)) return false;
  std::error_code ec; std::filesystem::create_directories(root, ec);
  if (ec) { error = "failed to create component deployment root"; return false; }
  std::string previous; (void)readActive(root, previous);
  if (!writeAtomic(activePath(root), id, error)) return false;
  std::ofstream history(root / "history.jsonl", std::ios::app);
  if (history) history << "{\"event\":\"activate\",\"generation\":\"" << id << "\",\"previous\":\"" << previous << "\"}\n";
  return true;
}

bool activeComponentGeneration(const std::string &root_text, ComponentGenerationInfo &generation, std::string &error) {
  const auto root = rootPath(root_text); std::string id;
  if (!readActive(root, id)) { error = "no active component generation"; return false; }
  return loadGeneration(root, id, generation, error);
}

bool rollbackComponentGeneration(const std::string &root_text, std::string &error) {
  const auto root = rootPath(root_text); std::ifstream history(root / "history.jsonl");
  if (!history) { error = "component deployment history is missing"; return false; }
  std::string line, candidate;
  while (std::getline(history, line)) {
    const auto marker = line.find("\"previous\":\"");
    if (marker != std::string::npos) { const auto begin = marker + std::string("\"previous\":\"").size(); const auto end = line.find('"', begin); if (end != std::string::npos && end > begin) candidate = line.substr(begin, end - begin); }
  }
  if (candidate.empty()) { error = "no previous component generation"; return false; }
  return activateComponentGeneration(root_text, candidate, error);
}

bool removeComponentGeneration(const std::string &root_text, std::string_view id, std::string &error) {
  const auto root = rootPath(root_text); std::string active;
  if (readActive(root, active) && active == id) { error = "cannot remove active component generation"; return false; }
  std::error_code ec; std::filesystem::remove_all(root / "generations" / std::string(id), ec);
  if (ec) { error = "failed to remove component generation"; return false; }
  return true;
}
} // namespace chtholly
