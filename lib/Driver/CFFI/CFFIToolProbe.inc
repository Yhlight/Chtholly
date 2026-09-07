// Included inside the CFFI tool implementation namespace; all helpers
// retain the original Clang model and generator state.
std::string flowPhysical(const compiler::CFDLFlowTypeSyntax &type) {
  auto value = normalizedType(type.physical_type);
  using Q = compiler::CFDLFlowQualifier;
  if (type.qualifier == Q::Ref || type.qualifier == Q::RefMut ||
      type.qualifier == Q::Out || type.qualifier == Q::InOut)
    value.push_back('*');
  return value;
}

bool sameEnumConstants(const std::vector<compiler::CFDLEnumConstantSyntax> &lhs,
                       const std::vector<compiler::CFDLEnumConstantSyntax> &rhs) {
  if (lhs.size() != rhs.size())
    return false;
  for (std::size_t index = 0; index < lhs.size(); ++index)
    if (lhs[index].name != rhs[index].name ||
        lhs[index].value != rhs[index].value)
      return false;
  return true;
}

bool compareCFDL(const std::string &expected, const std::string &actual_path,
                 std::string &actual_text, std::string &error) {
  auto loaded = readTextFile(actual_path, error);
  if (!loaded)
    return false;
  actual_text = *loaded;
  compiler::SharedValueStores values;
  compiler::SourceBuffer expected_source(
      compiler::SourceInput("generated.cfdl", expected));
  compiler::SourceBuffer actual_source(compiler::SourceInput(actual_path, actual_text));
  compiler::CFDLSyntaxFile expected_file, actual_file;
  std::vector<compiler::CFDLDiagnostic> expected_diagnostics, actual_diagnostics;
  if (!compiler::parseCFDL(expected_source, expected_file, expected_diagnostics) ||
      !compiler::parseCFDL(actual_source, actual_file, actual_diagnostics)) {
    error = "generated or supplied CFDL failed epoch-13 parsing";
    return false;
  }
  const std::array<const compiler::CFDLSyntaxFile *, 1> package_files{&actual_file};
  std::vector<compiler::CFDLPackageDiagnostic> package_diagnostics;
  if (!compiler::validateCFDLPackageProtocol(package_files, package_diagnostics)) {
    error = "supplied CFDL has an incomplete resource protocol";
    return false;
  }
  if (expected_file.module_name != actual_file.module_name) {
    error = "CFDL module identity disagrees with CFFI config";
    return false;
  }
  for (const auto &expected_type : expected_file.foreign_types) {
    const auto found =
        std::ranges::find(actual_file.foreign_types, expected_type.name,
                          &compiler::CFDLForeignTypeSyntax::name);
    if (found == actual_file.foreign_types.end() ||
        found->carrier_kind != expected_type.carrier_kind ||
        normalizedType(found->scalar_carrier) !=
            normalizedType(expected_type.scalar_carrier) ||
        found->fields.size() != expected_type.fields.size() ||
        !sameEnumConstants(found->enum_constants,
                           expected_type.enum_constants)) {
      error =
          "CFDL carrier disagrees with C declaration: " + expected_type.name;
      return false;
    }
    for (std::size_t index = 0; index < found->fields.size(); ++index)
      if (found->fields[index].name != expected_type.fields[index].name ||
          normalizedType(found->fields[index].physical_type) !=
              normalizedType(expected_type.fields[index].physical_type)) {
        error = "CFDL record field disagrees with C declaration: " +
                expected_type.name;
        return false;
      }
  }
  for (const auto &expected_fn : expected_file.callables) {
    const auto found =
        std::ranges::find(actual_file.callables, expected_fn.name,
                          &compiler::CFDLCallableSyntax::name);
    if (found == actual_file.callables.end() ||
        found->external_symbol != expected_fn.external_symbol ||
        found->calling_convention != expected_fn.calling_convention ||
        found->parameters.size() != expected_fn.parameters.size() ||
        flowPhysical(found->result) != flowPhysical(expected_fn.result)) {
      error = "CFDL callable disagrees with C declaration: " + expected_fn.name;
      return false;
    }
    for (std::size_t index = 0; index < found->parameters.size(); ++index) {
      auto actual_type = flowPhysical(found->parameters[index].type);
      auto expected_type = flowPhysical(expected_fn.parameters[index].type);
      if (actual_type != expected_type) {
        error = "CFDL callable parameter disagrees with C declaration: " +
                expected_fn.name;
        return false;
      }
    }
  }
  for (const auto &expected_constant : expected_file.foreign_constants) {
    const auto found =
        std::ranges::find(actual_file.foreign_constants, expected_constant.name,
                          &compiler::CFDLForeignConstantSyntax::name);
    if (found == actual_file.foreign_constants.end() ||
        normalizedType(found->physical_type) !=
            normalizedType(expected_constant.physical_type) ||
        found->kind != expected_constant.kind ||
        found->integer_payload != expected_constant.integer_payload ||
        found->integer_negative != expected_constant.integer_negative ||
        found->bool_value != expected_constant.bool_value) {
      error = "CFDL constant disagrees with C macro: " + expected_constant.name;
      return false;
    }
  }
  return true;
}

std::string makeProbe(const CFFIConfig &config, const Generator &generator) {
  std::ostringstream out;
  out << "#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n";
  for (const auto &header : config.headers)
    out << "#include \"" << header << "\"\n";
  for (const auto &[name, declaration] : generator.selectedTypes()) {
    const auto canonical =
        clang_getCanonicalType(clang_getCursorType(declaration.cursor));
    if (canonical.kind == CXType_Record && clang_Type_getSizeOf(canonical) >= 0)
      out << "static " << name << " chtholly_roundtrip_" << name << '(' << name
          << " value) { return value; }\n";
  }
  out << "int main(void) {\n  volatile uintptr_t symbols = 0;\n";
  for (const auto &[name, declaration] : generator.selectedTypes()) {
    const auto type = clang_getCursorType(declaration.cursor);
    const auto canonical = clang_getCanonicalType(type);
    const auto size = clang_Type_getSizeOf(type);
    const auto align = clang_Type_getAlignOf(type);
    if (size >= 0)
      out << "  if (sizeof(" << name << ") != " << size << ") return 10;\n";
    if (align >= 0)
      out << "  if (_Alignof(" << name << ") != " << align << ") return 11;\n";
    const auto type_cursor = clang_getTypeDeclaration(canonical);
    if (canonical.kind == CXType_Record && size >= 0) {
      std::vector<std::pair<std::string, long long>> fields;
      clang_visitChildren(
          type_cursor,
          [](CXCursor field, CXCursor, CXClientData data) {
            if (clang_getCursorKind(field) == CXCursor_FieldDecl) {
              const auto name = CXText(clang_getCursorSpelling(field)).str();
              if (!name.empty())
                static_cast<decltype(fields) *>(data)->emplace_back(
                    name, clang_Type_getOffsetOf(
                              clang_getCursorType(
                                  clang_getCursorSemanticParent(field)),
                              name.c_str()));
            }
            return CXChildVisit_Continue;
          },
          &fields);
      for (const auto &[field, bits] : fields)
        if (bits >= 0)
          out << "  if (offsetof(" << name << ", " << field
              << ") != " << bits / 8 << ") return 12;\n";
      out << "  { " << name << " input = {0}; " << name
          << " output = chtholly_roundtrip_" << name
          << "(input); if (memcmp(&input, &output, sizeof(input)) != 0) "
             "return 13; }\n";
    } else if (canonical.kind == CXType_Enum) {
      std::vector<std::pair<std::string, long long>> values;
      clang_visitChildren(
          type_cursor,
          [](CXCursor child, CXCursor, CXClientData data) {
            if (clang_getCursorKind(child) == CXCursor_EnumConstantDecl)
              static_cast<decltype(values) *>(data)->emplace_back(
                  CXText(clang_getCursorSpelling(child)).str(),
                  clang_getEnumConstantDeclValue(child));
            return CXChildVisit_Continue;
          },
          &values);
      for (const auto &[constant, value] : values)
        out << "  if ((long long)" << constant << " != " << value
            << ") return 14;\n";
    }
  }
  for (const auto &[name, declaration] : generator.selectedFunctions())
    out << "  symbols ^= (uintptr_t)(void*)&" << name << ";\n";
  for (const auto &[name, declaration] : generator.selectedConstants()) {
    auto evaluation = clang_Cursor_Evaluate(declaration.cursor);
    if (evaluation && clang_EvalResult_getKind(evaluation) == CXEval_Int) {
      if (clang_EvalResult_isUnsignedInt(evaluation))
        out << "  if ((unsigned long long)(" << name
            << ") != " << clang_EvalResult_getAsUnsigned(evaluation)
            << "ULL) return 15;\n";
      else
        out << "  if ((long long)(" << name
            << ") != " << clang_EvalResult_getAsLongLong(evaluation)
            << "LL) return 15;\n";
    }
    if (evaluation)
      clang_EvalResult_dispose(evaluation);
  }
  out << "  return symbols == (uintptr_t)-1 ? 99 : 0;\n}\n";
  return out.str();
}

bool compileAndRunProbe(const CFFIConfig &config, std::string_view source,
                        bool keep, std::string &compiler_version,
                        std::string &error) {
  static std::atomic<std::uint64_t> sequence = 0;
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
                    ("chtholly-cffi-" + std::to_string(stamp) + "-" +
                     std::to_string(sequence.fetch_add(1)));
  std::error_code file_error;
  std::filesystem::create_directories(root, file_error);
  if (file_error) {
    error = "failed to create CFFI probe directory: " + file_error.message();
    return false;
  }
  const auto source_path = (root / "probe.c").string();
#ifdef _WIN32
  const auto executable = (root / "probe.exe").string();
#else
  const auto executable = (root / "probe").string();
#endif
  if (!writeTextFile(source_path, std::string(source), error))
    return false;
  ProcessRunOptions options;
  options.timeout_milliseconds = config.timeout_ms;
  options.max_stdout_bytes = options.max_stderr_bytes = 1024 * 1024;
  options.environment_overrides = config.toolchain.environment_overrides;
  const bool msvc = config.toolchain.family == CFFICompilerFamily::MSVC;
  compiler_version = config.toolchain.compiler_version;
  std::vector<std::string> arguments = config.compile_arguments;
  if (msvc) {
    arguments.insert(arguments.end(),
                     {"/nologo", source_path, "/Fe:" + executable});
    for (const auto &path : config.include_paths)
      arguments.push_back("/I" + resolvePath(config, path));
    for (const auto &path : config.system_include_paths)
      arguments.push_back("/I" + resolvePath(config, path));
    for (const auto &define : config.defines)
      arguments.push_back("/D" + define);
    for (const auto &undefine : config.undefines)
      arguments.push_back("/U" + undefine);
    arguments.insert(arguments.end(), config.libraries.begin(),
                     config.libraries.end());
    if (!config.library_paths.empty() || !config.link_arguments.empty()) {
      arguments.push_back("/link");
      for (const auto &path : config.library_paths)
        arguments.push_back("/LIBPATH:" + resolvePath(config, path));
      arguments.insert(arguments.end(), config.link_arguments.begin(),
                       config.link_arguments.end());
    }
  } else {
    arguments.insert(arguments.end(), {source_path, "-o", executable});
    if (!config.toolchain.sysroot.empty())
      arguments.insert(arguments.begin(),
                       "--sysroot=" + config.toolchain.sysroot);
    for (const auto &path : config.include_paths)
      arguments.push_back("-I" + resolvePath(config, path));
    for (const auto &path : config.system_include_paths)
      arguments.push_back("-isystem" + resolvePath(config, path));
    for (const auto &define : config.defines)
      arguments.push_back("-D" + define);
    for (const auto &undefine : config.undefines)
      arguments.push_back("-U" + undefine);
    for (const auto &path : config.library_paths)
      arguments.push_back("-L" + resolvePath(config, path));
    for (const auto &library : config.libraries) {
      const auto path = std::filesystem::path(library);
      arguments.push_back(path.has_parent_path() || path.has_extension()
                              ? resolvePath(config, library)
                              : "-l" + library);
    }
    arguments.insert(arguments.end(), config.link_arguments.begin(),
                     config.link_arguments.end());
  }
  auto compiled =
      runProcess(config.toolchain.compiler, arguments, options, error);
  if (!compiled || compiled->exit_code != 0) {
    if (compiled)
      error = summarizeCommandFailure(config.toolchain.compiler, *compiled);
    if (keep)
      error += "\nprobe retained at " + root.string();
    else
      std::filesystem::remove_all(root, file_error);
    return false;
  }
  auto executed = runProcess(executable, {}, options, error);
  if (!executed || executed->exit_code != 0) {
    if (executed)
      error = summarizeCommandFailure(executable, *executed);
    if (keep)
      error += "\nprobe retained at " + root.string();
    else
      std::filesystem::remove_all(root, file_error);
    return false;
  }
  if (!keep)
    std::filesystem::remove_all(root, file_error);
  return true;
}

} // namespace

