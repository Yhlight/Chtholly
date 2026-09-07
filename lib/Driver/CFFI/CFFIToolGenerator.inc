// Included inside the CFFI tool implementation namespace; all helpers
// retain the original Clang model and generator state.
class Generator {
public:
  Generator(const CFFIConfig &config, const CModel &model)
      : config_(config), model_(model) {}

  bool run(std::string &source, std::string &error) {
    for (const auto &mapping : config_.type_mappings) {
      mapped_[normalizedType(mapping.c_type)] = mapping.cfdl_name;
      type_declarations_[mapping.cfdl_name] =
          "foreign type " + mapping.cfdl_name + ": " + mapping.carrier + ";";
    }
    for (const auto &root : config_.roots) {
      if (root.kind == CFFIRootKind::Constant) {
        if (!emitConstant(root.name, error))
          return false;
        continue;
      }
      const auto *declaration = findDecl(model_, root.kind, root.name);
      if (!declaration) {
        error = "configured CFFI root was not found: " + root.name;
        return false;
      }
      if (root.kind == CFFIRootKind::Function) {
        if (!emitFunction(*declaration, error))
          return false;
      } else if (!emitType(*declaration, error)) {
        return false;
      }
    }
    while (!pending_types_.empty()) {
      const auto name = *pending_types_.begin();
      pending_types_.erase(pending_types_.begin());
      if (type_declarations_.contains(name))
        continue;
      const auto *declaration = findDecl(model_, CFFIRootKind::Type, name);
      if (!declaration || !emitType(*declaration, error)) {
        if (error.empty())
          error = "C type dependency cannot be materialized: " + name;
        return false;
      }
    }
    std::ostringstream out;
    out << "module " << config_.module << ";\n\n";
    for (const auto &[name, text] : type_declarations_)
      out << text << "\n\n";
    for (const auto &[name, text] : constants_)
      out << text << "\n";
    if (!constants_.empty())
      out << '\n';
    for (const auto &[name, text] : functions_)
      out << text << "\n";
    source = out.str();
    return true;
  }

  const std::map<std::string, CDecl> &selectedFunctions() const {
    return selected_functions_;
  }
  const std::map<std::string, CDecl> &selectedTypes() const {
    return selected_types_;
  }
  const std::map<std::string, CDecl> &selectedConstants() const {
    return selected_constants_;
  }

private:
  bool emitConstant(std::string_view name, std::string &error) {
    const auto found = model_.evaluated_constants.find(std::string(name));
    if (found == model_.evaluated_constants.end()) {
      error =
          "configured CFFI constant was not evaluated: " + std::string(name);
      return false;
    }
    const auto type = clang_getCursorType(found->second.cursor);
    const auto rendered = renderType(type, error);
    if (rendered.empty())
      return false;
    const auto canonical = clang_getCanonicalType(type);
    if (canonical.kind != CXType_Bool && canonical.kind != CXType_Char_S &&
        canonical.kind != CXType_Char_U && canonical.kind != CXType_SChar &&
        canonical.kind != CXType_UChar && canonical.kind != CXType_Short &&
        canonical.kind != CXType_UShort && canonical.kind != CXType_Int &&
        canonical.kind != CXType_UInt && canonical.kind != CXType_Long &&
        canonical.kind != CXType_ULong && canonical.kind != CXType_LongLong &&
        canonical.kind != CXType_ULongLong) {
      error = "object-like C macro does not evaluate to an integer or bool: " +
              std::string(name);
      return false;
    }
    auto evaluation = clang_Cursor_Evaluate(found->second.cursor);
    if (!evaluation || clang_EvalResult_getKind(evaluation) != CXEval_Int) {
      if (evaluation)
        clang_EvalResult_dispose(evaluation);
      error = "object-like C macro is not an integer constant expression: " +
              std::string(name);
      return false;
    }
    std::string value;
    if (canonical.kind == CXType_Bool) {
      value =
          clang_EvalResult_getAsLongLong(evaluation) != 0 ? "true" : "false";
    } else if (clang_EvalResult_isUnsignedInt(evaluation)) {
      value = std::to_string(clang_EvalResult_getAsUnsigned(evaluation));
    } else {
      value = std::to_string(clang_EvalResult_getAsLongLong(evaluation));
    }
    clang_EvalResult_dispose(evaluation);
    constants_[std::string(name)] = "foreign const " + std::string(name) +
                                    ": " + rendered + " = " + value + ";";
    selected_constants_[std::string(name)] = found->second;
    return true;
  }

  std::string renderType(CXType type, std::string &error) {
    const auto spelling = CXText(clang_getTypeSpelling(type)).str();
    if (const auto found = mapped_.find(normalizedType(spelling));
        found != mapped_.end())
      return found->second;
    if (type.kind == CXType_Pointer) {
      const auto pointee = clang_getPointeeType(type);
      if (pointee.kind == CXType_FunctionProto) {
        const auto convention = callingConvention(config_, pointee, error);
        if (convention.empty())
          return {};
        std::ostringstream out;
        out << "c_fn " << convention << '(';
        const auto count = clang_getNumArgTypes(pointee);
        for (int index = 0; index < count; ++index) {
          if (index)
            out << ',';
          const auto argument =
              renderType(clang_getArgType(pointee, index), error);
          if (argument.empty())
            return {};
          out << argument;
        }
        const auto result = renderType(clang_getResultType(pointee), error);
        return result.empty() ? std::string{} : out.str() + ")->" + result;
      }
      auto inner = renderType(pointee, error);
      return inner.empty() ? std::string{} : inner + '*';
    }
    if (type.kind == CXType_ConstantArray) {
      auto element = renderType(clang_getArrayElementType(type), error);
      const auto size = clang_getArraySize(type);
      if (element.empty() || size <= 0) {
        error = "C array has no fixed positive extent";
        return {};
      }
      return "array<" + element + "," + std::to_string(size) + ">";
    }
    const auto canonical = clang_getCanonicalType(type);
    switch (canonical.kind) {
    case CXType_Void:
      return "void";
    case CXType_Bool:
      return "c_bool";
    case CXType_Char_S:
    case CXType_Char_U:
      return "c_char";
    case CXType_SChar:
      return "c_schar";
    case CXType_UChar:
      return "c_uchar";
    case CXType_Short:
      return "c_short";
    case CXType_UShort:
      return "c_ushort";
    case CXType_Int:
      return "c_int";
    case CXType_UInt:
      return "c_uint";
    case CXType_Long:
      return "c_long";
    case CXType_ULong:
      return "c_ulong";
    case CXType_LongLong:
      return "c_long_long";
    case CXType_ULongLong:
      return "c_ulong_long";
    case CXType_Float:
      return "f32";
    case CXType_Double:
      return "f64";
    default:
      break;
    }
    auto declaration = clang_getTypeDeclaration(type);
    auto name = CXText(clang_getCursorSpelling(declaration)).str();
    if (name.empty() && type.kind == CXType_Typedef)
      name = spelling;
    if (!name.empty()) {
      pending_types_.insert(name);
      return name;
    }
    error = "unsupported C type: " + spelling;
    return {};
  }

  bool emitType(const CDecl &declaration, std::string &error) {
    if (type_declarations_.contains(declaration.name))
      return true;
    auto &state = type_states_[declaration.name];
    if (state == 1) {
      error =
          "C type dependency cycle cannot be completed: " + declaration.name;
      return false;
    }
    if (state == 2)
      return true;
    state = 1;
    if (!emitTypeImpl(declaration, error))
      return false;
    state = 2;
    return true;
  }

  bool emitTypeImpl(const CDecl &declaration, std::string &error) {
    CXCursor cursor = declaration.cursor;
    CXType type = clang_getCursorType(cursor);
    if (declaration.kind == CXCursor_TypedefDecl) {
      type = clang_getTypedefDeclUnderlyingType(cursor);
      const auto canonical = clang_getCanonicalType(type);
      if (canonical.kind == CXType_Record || canonical.kind == CXType_Enum)
        cursor = clang_getTypeDeclaration(type);
    }
    const auto kind = clang_getCursorKind(cursor);
    selected_types_[declaration.name] = declaration;
    if ((kind == CXCursor_StructDecl || kind == CXCursor_UnionDecl) &&
        !clang_isCursorDefinition(cursor)) {
      type_declarations_[declaration.name] =
          "foreign type " + declaration.name + ";";
      return true;
    }
    if (kind == CXCursor_EnumDecl) {
      const auto carrier =
          renderType(clang_getEnumDeclIntegerType(cursor), error);
      if (carrier.empty())
        return false;
      std::vector<std::pair<std::string, long long>> values;
      clang_visitChildren(
          cursor,
          [](CXCursor child, CXCursor, CXClientData data) {
            if (clang_getCursorKind(child) == CXCursor_EnumConstantDecl)
              static_cast<decltype(values) *>(data)->emplace_back(
                  CXText(clang_getCursorSpelling(child)).str(),
                  clang_getEnumConstantDeclValue(child));
            return CXChildVisit_Continue;
          },
          &values);
      if (values.empty()) {
        error = "C enum root has no constants: " + declaration.name;
        return false;
      }
      std::ostringstream out;
      out << "foreign enum " << declaration.name << ": " << carrier << " {\n";
      std::ranges::sort(values);
      for (const auto &[name, value] : values)
        out << "  " << name << " = " << value << ";\n";
      out << "};";
      type_declarations_[declaration.name] = out.str();
      return true;
    }
    if (kind != CXCursor_StructDecl && kind != CXCursor_UnionDecl) {
      const auto carrier = renderType(type, error);
      if (carrier.empty())
        return false;
      type_declarations_[declaration.name] =
          "foreign type " + declaration.name + ": " + carrier + ";";
      return true;
    }
    std::vector<std::pair<std::string, std::string>> fields;
    bool invalid = false;
    clang_visitChildren(
        cursor,
        [](CXCursor child, CXCursor, CXClientData data) {
          auto &state = *static_cast<std::pair<
              Generator *,
              std::pair<std::vector<std::pair<std::string, std::string>> *,
                        bool *>> *>(data);
          if (clang_getCursorKind(child) != CXCursor_FieldDecl)
            return CXChildVisit_Continue;
          if (clang_Cursor_isBitField(child) ||
              clang_getCursorSpelling(child).data == nullptr) {
            *state.second.second = true;
            return CXChildVisit_Break;
          }
          std::string field_error;
          auto rendered =
              state.first->renderType(clang_getCursorType(child), field_error);
          if (rendered.empty()) {
            *state.second.second = true;
            return CXChildVisit_Break;
          }
          state.second.first->emplace_back(
              CXText(clang_getCursorSpelling(child)).str(),
              std::move(rendered));
          return CXChildVisit_Continue;
        },
        [&] {
          using State = std::pair<
              Generator *,
              std::pair<std::vector<std::pair<std::string, std::string>> *,
                        bool *>>;
          static thread_local State state;
          state = {this, {&fields, &invalid}};
          return &state;
        }());
    if (invalid || fields.empty()) {
      error =
          "record has unsupported, anonymous, bit-field, flexible, or empty "
          "shape: " +
          declaration.name;
      return false;
    }
    std::ostringstream out;
    out << (kind == CXCursor_UnionDecl ? "foreign union " : "foreign struct ")
        << declaration.name << " {\n";
    for (const auto &[name, field] : fields)
      out << "  " << name << ": " << field << ";\n";
    out << "};";
    type_declarations_[declaration.name] = out.str();
    return true;
  }

  bool emitFunction(const CDecl &declaration, std::string &error) {
    const auto type = clang_getCursorType(declaration.cursor);
    if (clang_isFunctionTypeVariadic(type)) {
      error = "variadic C function is unsupported: " + declaration.name;
      return false;
    }
    const auto convention = callingConvention(config_, type, error);
    if (convention.empty())
      return false;
    std::ostringstream out;
    out << "foreign fn " << declaration.name << '(';
    const auto count = clang_Cursor_getNumArguments(declaration.cursor);
    for (int index = 0; index < count; ++index) {
      const auto argument = clang_Cursor_getArgument(declaration.cursor, index);
      auto name = CXText(clang_getCursorSpelling(argument)).str();
      if (name.empty())
        name = "arg" + std::to_string(index);
      const auto rendered = renderType(clang_getCursorType(argument), error);
      if (rendered.empty())
        return false;
      if (index)
        out << ", ";
      out << name << ": " << rendered;
    }
    const auto result =
        renderType(clang_getCursorResultType(declaration.cursor), error);
    if (result.empty())
      return false;
    auto symbol = CXText(clang_Cursor_getMangling(declaration.cursor)).str();
    if (symbol.empty())
      symbol = declaration.name;
    out << ") -> " << result << " link " << quoteCFDL(symbol) << " call "
        << convention << ';';
    functions_[declaration.name] = out.str();
    selected_functions_[declaration.name] = declaration;
    return true;
  }

  const CFFIConfig &config_;
  const CModel &model_;
  std::map<std::string, std::string> mapped_;
  std::set<std::string> pending_types_;
  std::map<std::string, std::uint8_t> type_states_;
  std::map<std::string, std::string> type_declarations_;
  std::map<std::string, std::string> functions_;
  std::map<std::string, std::string> constants_;
  std::map<std::string, CDecl> selected_functions_;
  std::map<std::string, CDecl> selected_types_;
  std::map<std::string, CDecl> selected_constants_;
};

