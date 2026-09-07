// Included inside the CFFI tool implementation namespace; all helpers
// retain the original Clang model and generator state.
bool loadCFFIConfig(const std::string &path, CFFIConfig &config,
                    std::string &error, std::string_view cache_directory) {
  auto text = readTextFile(path, error);
  if (!text)
    return false;
  if (text->size() > MaxConfigBytes) {
    error = "CFFI config exceeds its input budget";
    return false;
  }
  config = {};
  config.path = std::filesystem::absolute(pathForFileSystem(path)).string();
  config.root_directory =
      std::filesystem::path(config.path).parent_path().string();
  config.cache_directory =
      (std::filesystem::path(config.root_directory) / ".chtholly" /
       "cffi-toolchain")
          .string();
  if (!cache_directory.empty())
    config.cache_directory = std::string(cache_directory);
  std::string table;
  CFFIRoot *root = nullptr;
  CFFITypeMapping *mapping = nullptr;
  std::set<std::string> scalar_keys;
  std::size_t line_number = 0;
  std::istringstream input(*text);
  std::string raw;
  while (std::getline(input, raw)) {
    ++line_number;
    auto line = trim(stripComment(raw));
    if (line.empty())
      continue;
    if (line.starts_with("[[") && line.ends_with("]]")) {
      table = trim(std::string_view(line).substr(2, line.size() - 4));
      root = nullptr;
      mapping = nullptr;
      if (table == "roots") {
        if (config.roots.size() >= MaxRoots) {
          error = "CFFI root count exceeds its budget";
          return false;
        }
        config.roots.emplace_back();
        root = &config.roots.back();
      } else if (table == "type_mappings") {
        config.type_mappings.emplace_back();
        mapping = &config.type_mappings.back();
      } else {
        error =
            "unknown CFFI array table at line " + std::to_string(line_number);
        return false;
      }
      continue;
    }
    if (line.starts_with('[') && line.ends_with(']')) {
      table = trim(std::string_view(line).substr(1, line.size() - 2));
      root = nullptr;
      mapping = nullptr;
      if (table != "toolchain" && table != "clang" && table != "probe") {
        error = "unknown CFFI table at line " + std::to_string(line_number);
        return false;
      }
      continue;
    }
    const auto equal = line.find('=');
    if (equal == std::string::npos) {
      error = "expected CFFI assignment at line " + std::to_string(line_number);
      return false;
    }
    const auto key = trim(std::string_view(line).substr(0, equal));
    const auto value = trim(std::string_view(line).substr(equal + 1));
    const auto full_key = table.empty() ? key : table + "." + key;
    if (!root && !mapping && !scalar_keys.insert(full_key).second) {
      error = "duplicate CFFI key: " + full_key;
      return false;
    }
    if (root) {
      std::string parsed;
      if (!parseStringValue(value, parsed, error, line_number))
        return false;
      if (key == "kind") {
        if (parsed == "type")
          root->kind = CFFIRootKind::Type;
        else if (parsed == "function")
          root->kind = CFFIRootKind::Function;
        else if (parsed == "constant")
          root->kind = CFFIRootKind::Constant;
        else {
          error = "CFFI root kind must be type, function, or constant";
          return false;
        }
      } else if (key == "name")
        root->name = std::move(parsed);
      else {
        error = "unknown CFFI root key: " + key;
        return false;
      }
      continue;
    }
    if (mapping) {
      std::string parsed;
      if (!parseStringValue(value, parsed, error, line_number))
        return false;
      if (key == "c_type")
        mapping->c_type = std::move(parsed);
      else if (key == "cfdl_name")
        mapping->cfdl_name = std::move(parsed);
      else if (key == "carrier")
        mapping->carrier = std::move(parsed);
      else {
        error = "unknown CFFI type mapping key: " + key;
        return false;
      }
      continue;
    }
    std::uint64_t unsigned_value = 0;
    if (full_key == "version") {
      if (!manifest_toml::parseUnsigned(value, unsigned_value) ||
          unsigned_value != 3) {
        error = "CFFI config requires version = 3";
        return false;
      }
      config.version = 3;
    } else if (full_key == "module") {
      if (!parseStringValue(value, config.module, error, line_number))
        return false;
    } else if (full_key == "target") {
      if (!parseStringValue(value, config.target, error, line_number))
        return false;
    } else if (full_key == "headers") {
      if (!parseArrayValue(value, config.headers, error, line_number))
        return false;
    } else if (full_key == "toolchain.compiler") {
      if (!parseStringValue(value, config.toolchain_request.compiler, error,
                            line_number))
        return false;
    } else if (full_key == "toolchain.msvc_install") {
      if (!parseStringValue(value, config.toolchain_request.msvc_install, error,
                            line_number))
        return false;
    } else if (full_key == "toolchain.sysroot") {
      if (!parseStringValue(value, config.toolchain_request.sysroot, error,
                            line_number))
        return false;
    } else if (full_key == "clang.language") {
      if (!parseStringValue(value, config.language, error, line_number))
        return false;
    } else if (full_key == "clang.standard") {
      if (!parseStringValue(value, config.standard, error, line_number))
        return false;
    } else if (full_key == "clang.include_paths") {
      if (!parseArrayValue(value, config.include_paths, error, line_number))
        return false;
    } else if (full_key == "clang.system_include_paths") {
      if (!parseArrayValue(value, config.system_include_paths, error,
                           line_number))
        return false;
    } else if (full_key == "clang.defines") {
      if (!parseArrayValue(value, config.defines, error, line_number))
        return false;
    } else if (full_key == "clang.undefines") {
      if (!parseArrayValue(value, config.undefines, error, line_number))
        return false;
    } else if (full_key == "clang.arguments") {
      if (!parseArrayValue(value, config.clang_arguments, error, line_number))
        return false;
    } else if (full_key == "probe.compile_arguments") {
      if (!parseArrayValue(value, config.compile_arguments, error, line_number))
        return false;
    } else if (full_key == "probe.link_arguments") {
      if (!parseArrayValue(value, config.link_arguments, error, line_number))
        return false;
    } else if (full_key == "probe.library_paths") {
      if (!parseArrayValue(value, config.library_paths, error, line_number))
        return false;
    } else if (full_key == "probe.libraries") {
      if (!parseArrayValue(value, config.libraries, error, line_number))
        return false;
    } else if (full_key == "probe.timeout_ms") {
      if (!manifest_toml::parseUnsigned(value, unsigned_value) ||
          unsigned_value == 0 || unsigned_value > 300000) {
        error = "probe.timeout_ms must be between 1 and 300000";
        return false;
      }
      config.timeout_ms = unsigned_value;
    } else {
      error = "unknown CFFI config key: " + full_key;
      return false;
    }
  }
  if (config.version != 3 || config.module.empty() || config.target.empty() ||
      config.headers.empty() || config.roots.empty() ||
      config.language != "c" || config.standard != "c17" ||
      config.target != hostTriple()) {
    error = "CFFI config is incomplete or target is not the native Tier-1 host";
    return false;
  }
  if (!std::ranges::all_of(config.module,
                           [](unsigned char ch) {
                             return std::isalnum(ch) || ch == '_' || ch == ':';
                           }) ||
      config.module.starts_with(':') || config.module.ends_with(':')) {
    error = "CFFI module is not a stable CFDL module path";
    return false;
  }
  std::set<std::string> unique_roots;
  for (const auto &root_value : config.roots)
    if (root_value.kind >= CFFIRootKind::Count || root_value.name.empty() ||
        !(std::isalpha(static_cast<unsigned char>(root_value.name.front())) ||
          root_value.name.front() == '_') ||
        !std::ranges::all_of(root_value.name,
                             [](unsigned char ch) {
                               return std::isalnum(ch) != 0 || ch == '_';
                             }) ||
        !unique_roots.emplace(root_value.name).second) {
      error = "CFFI roots must have unique names and valid C identifiers";
      return false;
    }
  std::set<std::string> unique_mappings;
  for (const auto &value : config.type_mappings)
    if (value.c_type.empty() || value.cfdl_name.empty() ||
        value.carrier.empty() ||
        !unique_mappings.insert(normalizedType(value.c_type)).second) {
      error = "CFFI type mappings must be complete and unique";
      return false;
    }
  for (const auto &header : config.headers)
    if (header.empty() || header.find_first_of("\"\r\n") != std::string::npos) {
      error = "CFFI header name is empty or unsafe";
      return false;
    }
  config.toolchain_request.target = config.target;
  const auto resolve_toolchain_path = [&](std::string &value) {
    if (value.empty() || value == "auto")
      return;
    const std::filesystem::path path(value);
    if (path.has_parent_path() && !path.is_absolute())
      value = resolvePath(config, value);
  };
  resolve_toolchain_path(config.toolchain_request.compiler);
  resolve_toolchain_path(config.toolchain_request.msvc_install);
  resolve_toolchain_path(config.toolchain_request.sysroot);
  if (!resolveCFFIToolchain(
          config.toolchain_request, config.toolchain, error,
          CFFIToolchainCacheOptions{.directory = config.cache_directory}))
    return false;
  return true;
}
