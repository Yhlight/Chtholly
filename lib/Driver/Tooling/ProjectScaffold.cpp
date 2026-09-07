#include "chtholly/Driver/ProjectScaffold.h"

#include "chtholly/Basic/LanguageVersion.h"
#include "chtholly/Support/FileSystem.h"

#include <cctype>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <vector>

namespace chtholly {
namespace {

bool validProjectName(std::string_view name) {
  if (name.empty() ||
      (!std::isalpha(static_cast<unsigned char>(name.front())) &&
       name.front() != '_'))
    return false;
  for (const char character : name)
    if (!std::isalnum(static_cast<unsigned char>(character)) &&
        character != '_')
      return false;
  return true;
}

} // namespace

bool createProjectScaffold(const ProjectScaffoldRequest &request,
                           std::string &created_root, std::string &error) {
  namespace fs = std::filesystem;
  error.clear();
  created_root.clear();
  if (!validProjectName(request.package_name)) {
    error = "project name must start with an ASCII letter or underscore and "
            "contain only ASCII letters, digits, or underscores";
    return false;
  }

  const auto root = pathForFileSystemTreeRoot(request.root_path);
  std::error_code file_error;
  if (fs::exists(root, file_error)) {
    if (file_error || !fs::is_directory(root, file_error)) {
      error = "project target is not a directory: " + request.root_path;
      return false;
    }
    if (file_error ||
        fs::directory_iterator(root, file_error) != fs::directory_iterator{}) {
      error = file_error
                  ? "failed to inspect project target: " + file_error.message()
                  : "project target is not empty: " + request.root_path;
      return false;
    }
  } else if (file_error) {
    error = "failed to inspect project target: " + file_error.message();
    return false;
  }

  const auto source_path =
      root / "src" / (request.library ? "lib.cns" : "main.cns");
  const auto manifest_path = root / "chtholly.toml";
  const auto ignore_path = root / ".gitignore";
  std::vector<fs::path> outputs{manifest_path, source_path, ignore_path};
  for (const auto &output : outputs) {
    if (fs::exists(output, file_error) || file_error) {
      error = file_error
                  ? "failed to inspect project output: " + file_error.message()
                  : "project output already exists: " + output.string();
      return false;
    }
  }

  const bool created_directory = !fs::exists(root);
  fs::create_directories(source_path.parent_path(), file_error);
  if (file_error) {
    error = "failed to create project directories: " + file_error.message();
    return false;
  }

  const auto rollback = [&] {
    std::error_code ignored;
    for (const auto &output : outputs)
      fs::remove(output, ignored);
    fs::remove(source_path.parent_path(), ignored);
    if (created_directory)
      fs::remove(root, ignored);
  };
  auto manifest = "[package]\nname = \"" + request.package_name +
                  "\"\nlanguage = \"" + LatestLanguageVersion.str() +
                  "\"\n\n[build]\n";
  if (!request.library)
    manifest += "entry = \"src/main.cns\"\n";
  manifest += "module_paths = [\"src\"]\n";
  const auto source =
      request.library
          ? "module " + request.package_name +
                ";\n\npub fn identity(value: i32): i32 {\n  return value;\n}\n"
          : "module main;\n\nfn main(): i32 {\n  return 0;\n}\n";
  if (!writeTextFile(manifest_path.string(), manifest, error) ||
      !writeTextFile(source_path.string(), source, error) ||
      !writeTextFile(ignore_path.string(), "/.chtholly/\n", error)) {
    rollback();
    return false;
  }
  created_root = fs::absolute(root, file_error).string();
  if (file_error)
    created_root = root.string();
  return true;
}

} // namespace chtholly
