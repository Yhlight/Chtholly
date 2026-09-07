// Included inside the CFFI tool implementation namespace; all helpers
// retain the original Clang model and generator state.
constexpr std::size_t MaxConfigBytes = 1024 * 1024;
constexpr std::size_t MaxHeaderBytes = 64 * 1024 * 1024;
constexpr std::size_t MaxRoots = 4096;
constexpr std::size_t MaxDeclarations = 65536;

struct CXText {
  CXString value{};
  explicit CXText(CXString value) : value(value) {}
  ~CXText() {
    clang_disposeString(value);
  }
  std::string str() const {
    const auto *text = clang_getCString(value);
    return text ? std::string(text) : std::string{};
  }
};

struct IndexGuard {
  CXIndex value = nullptr;
  ~IndexGuard() {
    if (value)
      clang_disposeIndex(value);
  }
};

struct UnitGuard {
  CXTranslationUnit value = nullptr;
  ~UnitGuard() {
    if (value)
      clang_disposeTranslationUnit(value);
  }
};

struct CDecl {
  CXCursor cursor{};
  CXCursorKind kind = CXCursor_UnexposedDecl;
  std::string name;
};

struct CDeclKey {
  CXCursorKind kind = CXCursor_UnexposedDecl;
  std::string name;

  friend bool operator<(const CDeclKey &lhs, const CDeclKey &rhs) {
    return std::tie(lhs.kind, lhs.name) < std::tie(rhs.kind, rhs.name);
  }
};

struct CModel {
  IndexGuard index;
  UnitGuard unit;
  UnitGuard evaluation_unit;
  std::map<CDeclKey, CDecl> declarations;
  std::map<std::string, CDecl> evaluated_constants;
  std::set<CDeclKey> ambiguous_declarations;
  std::string clang_version;
  std::set<std::string> included_files;
};

std::string trim(std::string_view value) {
  return manifest_toml::trim(value);
}

std::string stripComment(std::string_view line) {
  bool quoted = false, escaped = false;
  for (std::size_t index = 0; index < line.size(); ++index) {
    const char ch = line[index];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (quoted && ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '"')
      quoted = !quoted;
    else if (!quoted && ch == '#')
      return std::string(line.substr(0, index));
  }
  return std::string(line);
}

std::string normalizedType(std::string value) {
  const auto erase_prefix = [&](std::string_view prefix) {
    if (value.starts_with(prefix))
      value.erase(0, prefix.size());
  };
  erase_prefix("struct ");
  erase_prefix("union ");
  erase_prefix("enum ");
  value.erase(
      std::remove_if(value.begin(), value.end(),
                     [](unsigned char ch) { return std::isspace(ch) != 0; }),
      value.end());
  return value;
}

std::string quoteCFDL(std::string_view value) {
  std::string out = "\"";
  for (const char ch : value) {
    if (ch == '\\' || ch == '"')
      out.push_back('\\');
    out.push_back(ch);
  }
  out.push_back('"');
  return out;
}

std::string resolvePath(const CFFIConfig &config, std::string_view value) {
  const std::filesystem::path path(value);
  return (path.is_absolute()
              ? path
              : std::filesystem::path(config.root_directory) / path)
      .lexically_normal()
      .string();
}

struct LinkClosure {
  std::string fingerprint;
  std::vector<std::string> records;
};

std::string canonicalLibraryPath(const std::filesystem::path &path) {
  std::error_code error;
  auto result = std::filesystem::weakly_canonical(path, error);
  if (error)
    result = std::filesystem::absolute(path, error);
  return (error ? path : result).lexically_normal().string();
}

bool computeLinkClosure(const CFFIConfig &config, LinkClosure &closure,
                        std::string &error) {
  std::vector<std::string> requested = config.libraries;
  for (const auto &argument : config.link_arguments) {
    const auto lower = [&] {
      std::string value = argument;
      std::ranges::transform(value, value.begin(),
                             [](unsigned char ch) {
                               return static_cast<char>(std::tolower(ch));
                             });
      return value;
    }();
    if (lower.ends_with(".lib") || lower.ends_with(".a"))
      requested.push_back(argument);
  }

  std::vector<std::string> search_paths;
  search_paths.reserve(config.library_paths.size() +
                       config.toolchain.system_library_paths.size());
  for (const auto &path : config.library_paths)
    search_paths.push_back(resolvePath(config, path));
  search_paths.insert(search_paths.end(),
                      config.toolchain.system_library_paths.begin(),
                      config.toolchain.system_library_paths.end());
  std::ranges::sort(search_paths);
  search_paths.erase(std::unique(search_paths.begin(), search_paths.end()),
                     search_paths.end());

  std::ostringstream canonical;
  canonical << "chtholly.cffi.link-closure.v1\n";
  for (const auto &request : requested) {
    std::vector<std::filesystem::path> candidates;
    const auto raw_input = pathForFileSystem(request);
    const auto input = pathForFileSystem(resolvePath(config, request));
    const bool explicit_path = raw_input.is_absolute() ||
                               request.find_first_of("\\/") !=
                                   std::string_view::npos;
    if (explicit_path) {
      candidates.push_back(input);
    } else {
      std::vector<std::filesystem::path> names = {raw_input};
      if (!raw_input.has_extension()) {
#ifdef _WIN32
        names.push_back(raw_input.string() + ".lib");
#else
        names.push_back("lib" + raw_input.string() + ".a");
        names.push_back("lib" + raw_input.string() + ".so");
#endif
      }
      for (const auto &directory : search_paths)
        for (const auto &name : names)
          candidates.push_back(pathForFileSystem(directory) / name);
    }
    std::vector<std::pair<std::string, std::string>> matches;
    for (const auto &candidate : candidates) {
      std::error_code file_error;
      if (!std::filesystem::is_regular_file(candidate, file_error) ||
          file_error)
        continue;
      const auto normalized = canonicalLibraryPath(candidate);
      const auto digest = sha256File(normalized);
      if (digest)
        matches.emplace_back(normalized, *digest);
    }
    std::ranges::sort(matches, {}, &std::pair<std::string, std::string>::first);
    matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
    if (matches.empty()) {
      error = "CFFI link closure is incomplete: missing library '" + request +
              "' (searched " + std::to_string(search_paths.size()) +
              " directories)";
      return false;
    }
    if (matches.size() > 1) {
      bool distinct = false;
      for (std::size_t index = 1; index < matches.size(); ++index)
        distinct |= matches[index].second != matches.front().second;
      if (distinct) {
        error = "CFFI link closure has conflicting candidates for '" + request +
                "': ";
        for (std::size_t index = 0; index < matches.size(); ++index) {
          if (index != 0)
            error += "; ";
          error += matches[index].first + " (sha256=" + matches[index].second +
                   ")";
        }
        return false;
      }
    }
    const auto &selected = matches.front();
    canonical << request.size() << ':' << request << '\n'
              << selected.first.size() << ':' << selected.first << '\n'
              << selected.second << '\n';
    closure.records.push_back("selected=" + request + "=" + selected.first +
                              ":sha256=" + selected.second);
    if (matches.size() > 1) {
      std::string alternatives;
      for (std::size_t index = 1; index < matches.size(); ++index) {
        if (!alternatives.empty())
          alternatives += ',';
        alternatives += matches[index].first + ":sha256=" + matches[index].second;
      }
      closure.records.push_back("alternates=" + request + "=" + alternatives);
    }
  }
  closure.fingerprint = sha256Hex(canonical.str());
  return true;
}

bool parseStringValue(std::string_view value, std::string &out,
                      std::string &error, std::size_t line) {
  if (!manifest_toml::parseString(value, out)) {
    error = "invalid CFFI string at line " + std::to_string(line);
    return false;
  }
  return true;
}

bool parseArrayValue(std::string_view value, std::vector<std::string> &out,
                     std::string &error, std::size_t line) {
  if (!manifest_toml::parseStringArray(value, out)) {
    error = "invalid CFFI string array at line " + std::to_string(line);
    return false;
  }
  return true;
}

std::string hostTriple() {
#if defined(_WIN32) && defined(_M_X64)
  return "x86_64-pc-windows-msvc";
#elif defined(__linux__) && defined(__x86_64__)
  return "x86_64-unknown-linux-gnu";
#else
  return "unsupported-host";
#endif
}

std::optional<std::string> cursorFile(CXCursor cursor) {
  CXFile file = nullptr;
  unsigned line = 0, column = 0, offset = 0;
  clang_getExpansionLocation(clang_getCursorLocation(cursor), &file, &line,
                             &column, &offset);
  if (!file)
    return std::nullopt;
  return CXText(clang_getFileName(file)).str();
}

CXChildVisitResult collectCursor(CXCursor cursor, CXCursor,
                                 CXClientData client) {
  auto &model = *static_cast<CModel *>(client);
  const auto kind = clang_getCursorKind(cursor);
  if (kind == CXCursor_FunctionDecl || kind == CXCursor_TypedefDecl ||
      kind == CXCursor_StructDecl || kind == CXCursor_UnionDecl ||
      kind == CXCursor_EnumDecl || kind == CXCursor_MacroDefinition) {
    const auto name = CXText(clang_getCursorSpelling(cursor)).str();
    if (!name.empty()) {
      const CDeclKey key{kind, name};
      const auto canonical = clang_getCanonicalCursor(cursor);
      if (kind == CXCursor_MacroDefinition) {
        model.declarations.insert_or_assign(
            key, CDecl{cursor, kind, std::move(name)});
        return CXChildVisit_Recurse;
      }
      const auto [found, inserted] =
          model.declarations.try_emplace(key, CDecl{canonical, kind, name});
      if (!inserted && !clang_equalCursors(found->second.cursor, canonical))
        model.ambiguous_declarations.insert(key);
    }
  }
  return model.declarations.size() > MaxDeclarations ? CXChildVisit_Break
                                                     : CXChildVisit_Recurse;
}

void collectInclusion(CXFile file, CXSourceLocation *, unsigned,
                      CXClientData client) {
  if (file) {
    auto path = CXText(clang_getFileName(file)).str();
    if (path != "chtholly_cffi_input.c")
      static_cast<CModel *>(client)->included_files.insert(std::move(path));
  }
}

std::optional<std::string> loadedLibclangPath() {
#ifdef _WIN32
  const auto module = GetModuleHandleW(L"libclang.dll");
  if (!module)
    return std::nullopt;
  std::wstring path(32768, L'\0');
  const auto size =
      GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
  if (size == 0 || size >= path.size())
    return std::nullopt;
  path.resize(size);
  return std::filesystem::path(path).string();
#else
  Dl_info info{};
  if (dladdr(reinterpret_cast<void *>(&clang_getClangVersion), &info) == 0 ||
      !info.dli_fname)
    return std::nullopt;
  return std::string(info.dli_fname);
#endif
}

bool buildModel(const CFFIConfig &config, CModel &model, std::string &error) {
  model.index.value = clang_createIndex(0, 0);
  if (!model.index.value) {
    error = "libclang failed to create an index";
    return false;
  }
  std::ostringstream source;
  for (const auto &header : config.headers)
    source << "#include \"" << header << "\"\n";
  const auto source_text = source.str();
  CXUnsavedFile unsaved{"chtholly_cffi_input.c", source_text.c_str(),
                        static_cast<unsigned long>(source_text.size())};
  std::vector<std::string> argument_storage = {
      "-x", "c", "-std=" + config.standard, "--target=" + config.target};
  for (const auto &path : config.include_paths) {
    argument_storage.push_back("-I" + resolvePath(config, path));
  }
  for (const auto &path : config.system_include_paths) {
    argument_storage.push_back("-isystem");
    argument_storage.push_back(resolvePath(config, path));
  }
  for (const auto &path : config.toolchain.system_include_paths) {
    argument_storage.push_back("-isystem");
    argument_storage.push_back(path);
  }
  if (!config.toolchain.sysroot.empty())
    argument_storage.push_back("--sysroot=" + config.toolchain.sysroot);
  if (!config.toolchain.resource_dir.empty())
    argument_storage.push_back("-resource-dir=" +
                               config.toolchain.resource_dir);
  for (const auto &define : config.defines)
    argument_storage.push_back("-D" + define);
  for (const auto &undefine : config.undefines)
    argument_storage.push_back("-U" + undefine);
  argument_storage.insert(argument_storage.end(),
                          config.clang_arguments.begin(),
                          config.clang_arguments.end());
  std::vector<const char *> arguments;
  for (const auto &argument : argument_storage)
    arguments.push_back(argument.c_str());
  const auto result = clang_parseTranslationUnit2(
      model.index.value, unsaved.Filename, arguments.data(),
      static_cast<int>(arguments.size()), &unsaved, 1,
      CXTranslationUnit_DetailedPreprocessingRecord |
          CXTranslationUnit_KeepGoing,
      &model.unit.value);
  if (result != CXError_Success || !model.unit.value) {
    error = "libclang failed to parse the configured C translation unit";
    return false;
  }
  std::ostringstream diagnostics;
  const auto count = clang_getNumDiagnostics(model.unit.value);
  bool failed = false;
  for (unsigned index = 0; index < count; ++index) {
    auto diagnostic = clang_getDiagnostic(model.unit.value, index);
    const auto severity = clang_getDiagnosticSeverity(diagnostic);
    if (severity >= CXDiagnostic_Error)
      failed = true;
    diagnostics << CXText(
                       clang_formatDiagnostic(
                           diagnostic, clang_defaultDiagnosticDisplayOptions()))
                       .str()
                << '\n';
    clang_disposeDiagnostic(diagnostic);
  }
  if (failed) {
    error =
        "Clang rejected the configured C environment:\n" + diagnostics.str();
    return false;
  }
  clang_visitChildren(clang_getTranslationUnitCursor(model.unit.value),
                      collectCursor, &model);
  clang_getInclusions(model.unit.value, collectInclusion, &model);
  if (model.declarations.size() > MaxDeclarations) {
    error = "C declaration inventory exceeded its configured budget";
    return false;
  }
  if (!model.ambiguous_declarations.empty()) {
    error = "C declaration inventory has an ambiguous canonical key: " +
            model.ambiguous_declarations.begin()->name;
    return false;
  }
  std::vector<std::string> constant_roots;
  for (const auto &root : config.roots) {
    if (root.kind != CFFIRootKind::Constant)
      continue;
    const auto found =
        model.declarations.find(CDeclKey{CXCursor_MacroDefinition, root.name});
    if (found == model.declarations.end()) {
      error = "configured CFFI constant macro was not found: " + root.name;
      return false;
    }
    if (!cursorFile(found->second.cursor)) {
      error = "predefined C macro constants are unsupported: " + root.name;
      return false;
    }
    if (clang_Cursor_isMacroFunctionLike(found->second.cursor)) {
      error = "function-like C macro is unsupported: " + root.name;
      return false;
    }
    constant_roots.push_back(root.name);
  }
  if (!constant_roots.empty()) {
    std::ostringstream evaluation_source;
    for (const auto &header : config.headers)
      evaluation_source << "#include \"" << header << "\"\n";
    for (std::size_t index = 0; index < constant_roots.size(); ++index) {
      evaluation_source << "static const __typeof__((" << constant_roots[index]
                        << ")) __chtholly_cffi_constant_" << index << " = ("
                        << constant_roots[index] << ");\n";
    }
    const auto evaluation_text = evaluation_source.str();
    CXUnsavedFile evaluation_unsaved{
        "chtholly_cffi_constants.c", evaluation_text.c_str(),
        static_cast<unsigned long>(evaluation_text.size())};
    const auto evaluation_result = clang_parseTranslationUnit2(
        model.index.value, evaluation_unsaved.Filename, arguments.data(),
        static_cast<int>(arguments.size()), &evaluation_unsaved, 1,
        CXTranslationUnit_KeepGoing, &model.evaluation_unit.value);
    if (evaluation_result != CXError_Success || !model.evaluation_unit.value) {
      error = "libclang failed to parse the C macro evaluation unit";
      return false;
    }
    std::ostringstream evaluation_diagnostics;
    bool evaluation_failed = false;
    const auto evaluation_count =
        clang_getNumDiagnostics(model.evaluation_unit.value);
    for (unsigned index = 0; index < evaluation_count; ++index) {
      auto diagnostic = clang_getDiagnostic(model.evaluation_unit.value, index);
      evaluation_failed |=
          clang_getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error;
      evaluation_diagnostics
          << CXText(clang_formatDiagnostic(
                        diagnostic, clang_defaultDiagnosticDisplayOptions()))
                 .str()
          << '\n';
      clang_disposeDiagnostic(diagnostic);
    }
    if (evaluation_failed) {
      error = "Clang rejected a configured object-like macro constant:\n" +
              evaluation_diagnostics.str();
      return false;
    }
    struct EvaluationState {
      CModel *model;
      const std::vector<std::string> *roots;
    } evaluation_state{&model, &constant_roots};
    clang_visitChildren(
        clang_getTranslationUnitCursor(model.evaluation_unit.value),
        [](CXCursor cursor, CXCursor, CXClientData client) {
          if (clang_getCursorKind(cursor) != CXCursor_VarDecl)
            return CXChildVisit_Recurse;
          const auto spelling = CXText(clang_getCursorSpelling(cursor)).str();
          constexpr std::string_view prefix = "__chtholly_cffi_constant_";
          if (!spelling.starts_with(prefix))
            return CXChildVisit_Continue;
          std::size_t index = 0;
          const auto suffix = std::string_view(spelling).substr(prefix.size());
          const auto parsed = std::from_chars(
              suffix.data(), suffix.data() + suffix.size(), index);
          auto &state = *static_cast<EvaluationState *>(client);
          if (parsed.ec == std::errc{} &&
              parsed.ptr == suffix.data() + suffix.size() &&
              index < state.roots->size()) {
            const auto &name = state.roots->at(index);
            state.model->evaluated_constants.emplace(
                name, CDecl{cursor, CXCursor_VarDecl, name});
          }
          return CXChildVisit_Continue;
        },
        &evaluation_state);
    if (model.evaluated_constants.size() != constant_roots.size()) {
      error = "Clang did not materialize every configured macro constant";
      return false;
    }
  }
  model.clang_version = CXText(clang_getClangVersion()).str();
  return true;
}

const CDecl *findDecl(const CModel &model, CFFIRootKind kind,
                      std::string_view name) {
  const auto match = [&](CXCursorKind cursor_kind) -> const CDecl * {
    const auto found =
        model.declarations.find(CDeclKey{cursor_kind, std::string(name)});
    return found == model.declarations.end() ? nullptr : &found->second;
  };
  if (kind == CFFIRootKind::Function)
    return match(CXCursor_FunctionDecl);
  if (kind != CFFIRootKind::Type)
    return nullptr;
  for (const auto cursor_kind : {CXCursor_TypedefDecl, CXCursor_StructDecl,
                                 CXCursor_UnionDecl, CXCursor_EnumDecl})
    if (const auto *found = match(cursor_kind))
      return found;
  return nullptr;
}

std::string callingConvention(const CFFIConfig &config, CXType type,
                              std::string &error) {
  const auto value = clang_getFunctionTypeCallingConv(type);
  if (value == CXCallingConv_Default || value == CXCallingConv_C)
    return "c";
  if (value == CXCallingConv_Win64)
    return "win64";
  if (value == CXCallingConv_X86_64SysV)
    return "sysv64";
  error = "unsupported C calling convention " + std::to_string(value) +
          " for target " + config.target;
  return {};
}

