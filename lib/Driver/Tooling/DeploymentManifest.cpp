#include "chtholly/Driver/DeploymentManifest.h"

#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <fstream>
#include <map>

namespace chtholly {
namespace {
bool required(const std::map<std::string, std::string, std::less<>> &values,
             std::string_view key, std::string &out) {
  const auto it = values.find(std::string(key));
  if (it == values.end() || it->second.empty())
    return false;
  out = it->second;
  return true;
}
}

bool loadDeploymentManifest(const std::filesystem::path &path,
                            DeploymentManifest &result, std::string &error) {
  result = {};
  std::ifstream input(path, std::ios::binary);
  if (!input) { error = "unable to open deployment manifest"; return false; }
  std::map<std::string, std::string, std::less<>> values;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line == "[component]" || line.starts_with('#')) continue;
    const auto equal = line.find('=');
    if (equal == std::string::npos) { error = "invalid deployment manifest assignment"; return false; }
    auto key = line.substr(0, equal), value = line.substr(equal + 1);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') { error = "deployment manifest values must be quoted"; return false; }
    value = value.substr(1, value.size() - 2);
    if (key != "identity" && key != "version" && key != "target" && key != "runtime" &&
        key != "library" && key != "contract" && key != "contract_digest") {
      error = "unknown deployment manifest key: " + key; return false;
    }
    if (!values.emplace(std::move(key), std::move(value)).second) { error = "duplicate deployment manifest key"; return false; }
  }
  if (!required(values, "identity", result.identity) || !required(values, "version", result.version) ||
      !required(values, "target", result.target) || !required(values, "runtime", result.runtime) ||
      !required(values, "contract_digest", result.contract_digest)) {
    error = "deployment manifest is missing required component fields"; return false;
  }
  std::string library, contract;
  if (!required(values, "library", library) || !required(values, "contract", contract)) {
    error = "deployment manifest is missing library or contract"; return false;
  }
  if (!parseDeploymentDigest(result.contract_digest)) { error = "deployment manifest contract_digest is not lowercase SHA-256"; return false; }
  result.root = std::filesystem::absolute(path).parent_path().lexically_normal();
  const auto resolve = [&](std::string_view value, std::filesystem::path &destination) {
    const std::filesystem::path relative(value);
    if (relative.empty() || relative.is_absolute()) return false;
    destination = (result.root / relative).lexically_normal();
    auto root_it = result.root.begin(); auto path_it = destination.begin();
    for (; root_it != result.root.end(); ++root_it, ++path_it)
      if (path_it == destination.end() || *root_it != *path_it) return false;
    return true;
  };
  if (!resolve(library, result.library) || !resolve(contract, result.contract)) {
    error = "deployment manifest path escapes its root"; return false;
  }
  return true;
}

std::optional<std::array<std::uint8_t, 32>> parseDeploymentDigest(std::string_view text) {
  if (text.size() != 64) return std::nullopt;
  std::array<std::uint8_t, 32> result{};
  const auto digit = [](char c) -> int { if (c >= '0' && c <= '9') return c - '0'; if (c >= 'a' && c <= 'f') return c - 'a' + 10; return -1; };
  for (std::size_t i = 0; i < result.size(); ++i) { const auto hi = digit(text[2*i]), lo = digit(text[2*i+1]); if (hi < 0 || lo < 0) return std::nullopt; result[i] = static_cast<std::uint8_t>((hi << 4) | lo); }
  return result;
}

bool validateDeploymentFiles(const DeploymentManifest &manifest, std::string &error) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(manifest.library, ec) || ec) { error = "deployment library is missing"; return false; }
  if (!std::filesystem::is_regular_file(manifest.contract, ec) || ec) { error = "deployment contract is missing"; return false; }
  std::string contract_text;
  if (const auto text = readTextFile(manifest.contract.string(), error))
    contract_text = *text;
  const auto marker = contract_text.find("contract-digest\t");
  if (marker != std::string::npos) {
    const auto begin = marker + std::string("contract-digest\t").size();
    const auto end = contract_text.find_first_of("\r\n", begin);
    if (contract_text.substr(begin, end == std::string::npos ? std::string::npos : end - begin) != manifest.contract_digest) {
      error = "deployment contract digest mismatch"; return false;
    }
  } else {
    const auto digest = sha256File(manifest.contract.string());
    if (!digest || *digest != manifest.contract_digest) {
      error = "deployment contract digest mismatch"; return false;
    }
  }
  return true;
}

} // namespace chtholly
