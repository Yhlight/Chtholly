// Included inside CompilationSession::compile; this fragment aliases the
// existing session Impl and preserves phase/state ordering.
  for (auto &unit : impl_->units) {
    if (cancelled()) {
      error = "compiler compilation cancelled";
      return false;
    }
    auto &data = *unit->impl_;
    if (data.kind == CompilationUnitKind::ForeignBinding) {
      std::vector<CFDLDiagnostic> cfdl_diagnostics;
      data.cfdl_syntax.emplace();
      (void)parseCFDL(data.source, *data.cfdl_syntax, cfdl_diagnostics);
      const auto diagnostic_kind = [](CFDLDiagnostic::Kind kind) {
        switch (kind) {
        case CFDLDiagnostic::Kind::InvalidSyntax:
          return DiagnosticKind::CFDLInvalidSyntax;
        case CFDLDiagnostic::Kind::InvalidModule:
          return DiagnosticKind::CFDLInvalidModule;
        case CFDLDiagnostic::Kind::InvalidImport:
          return DiagnosticKind::CFDLInvalidImport;
        case CFDLDiagnostic::Kind::DuplicateName:
          return DiagnosticKind::CFDLDuplicateName;
        case CFDLDiagnostic::Kind::UnknownType:
          return DiagnosticKind::CFDLUnknownType;
        case CFDLDiagnostic::Kind::UnknownCallable:
          return DiagnosticKind::CFDLUnknownCallable;
        case CFDLDiagnostic::Kind::InvalidCallable:
          return DiagnosticKind::CFDLInvalidCallable;
        case CFDLDiagnostic::Kind::IncompleteCarrier:
          return DiagnosticKind::CFDLIncompleteCarrier;
        case CFDLDiagnostic::Kind::InvalidCarrier:
          return DiagnosticKind::CFDLInvalidCarrier;
        case CFDLDiagnostic::Kind::CompletionFailed:
          return DiagnosticKind::CFDLCompletionFailed;
        case CFDLDiagnostic::Kind::InvalidProtocol:
          return DiagnosticKind::CFDLInvalidProtocol;
        }
        return DiagnosticKind::CFDLInvalidSyntax;
      };
      for (const auto &diagnostic : cfdl_diagnostics)
        data.diagnostics.emit(diagnostic_kind(diagnostic.kind),
                              diagnostic.offset, diagnostic.length);
    } else {
      data.tokens.emplace(lex(data.source, *impl_->values, data.diagnostics,
                              impl_->language_contract.source));
      data.parse_tree.emplace(parse(*data.tokens, data.diagnostics));
    }
  }

  {
    std::vector<const CFDLSyntaxFile *> cfdl_files;
    std::vector<CheckIRId> cfdl_units;
    for (const auto &unit : impl_->units) {
      if (unit->impl_->kind != CompilationUnitKind::ForeignBinding ||
          !unit->impl_->cfdl_syntax)
        continue;
      cfdl_units.push_back(unit->impl_->id);
      cfdl_files.push_back(&*unit->impl_->cfdl_syntax);
    }
    std::vector<CFDLPackageDiagnostic> protocol_diagnostics;
    (void)validateCFDLPackageProtocol(cfdl_files, protocol_diagnostics);
    for (const auto &diagnostic : protocol_diagnostics) {
      if (diagnostic.unit_index >= cfdl_units.size())
        continue;
      impl_->units[cfdl_units[diagnostic.unit_index].index]
          ->impl_->diagnostics.emit(DiagnosticKind::CFDLInvalidProtocol,
                                    diagnostic.diagnostic.offset,
                                    diagnostic.diagnostic.length);
    }
  }

  for (auto &unit : impl_->units) {
    auto &data = *unit->impl_;
    if (data.kind == CompilationUnitKind::ForeignBinding) {
      if (data.cfdl_syntax && !data.cfdl_syntax->module_name.empty())
        data.module_name =
            impl_->values->internIdentifier(data.cfdl_syntax->module_name);
      continue;
    }
    if (!data.parse_tree)
      continue;
    const auto root =
        NodeId(static_cast<std::uint32_t>(data.parse_tree->size() - 1));
    const auto declarations = data.parse_tree->children(root);
    NodeId module_decl;
    for (std::size_t index = 0; index < declarations.size(); ++index) {
      const auto declaration = declarations[index];
      if (data.parse_tree->kind(declaration) != NodeKind::ModuleDecl)
        continue;
      const auto &token = data.tokens->get(data.parse_tree->token(declaration));
      if (module_decl.hasValue()) {
        data.diagnostics.emit(DiagnosticKind::DuplicateModuleDeclaration,
                              token.offset, token.length);
        continue;
      }
      module_decl = declaration;
      if (index != 0)
        data.diagnostics.emit(DiagnosticKind::MisplacedModuleDeclaration,
                              token.offset, token.length);
    }
    if (!module_decl.hasValue()) {
      data.diagnostics.emit(DiagnosticKind::MissingModuleDeclaration, 0);
      continue;
    }
    const auto children = data.parse_tree->children(module_decl);
    const auto module_name = children.size() == 1
                                 ? modulePathText(*data.parse_tree, children[0])
                                 : std::string{};
    if (module_name.empty()) {
      const auto &token = data.tokens->get(data.parse_tree->token(module_decl));
      data.diagnostics.emit(DiagnosticKind::InvalidParseTree, token.offset,
                            token.length);
      continue;
    }
    data.module_name = impl_->values->internIdentifier(module_name);
  }

  std::unordered_map<std::uint32_t, CheckIRId> modules;
  for (const auto &unit : impl_->units) {
    auto &data = *unit->impl_;
    if (!data.module_name.hasValue())
      continue;
    const auto [unused, inserted] =
        modules.emplace(data.module_name.index, data.id);
    (void)unused;
    if (!inserted)
      data.diagnostics.emit(DiagnosticKind::DuplicateModule, 0);
  }
  if (const auto diagnostics = format_diagnostics(); !diagnostics.empty()) {
    error = diagnostics;
    return false;
  }

  for (auto &unit : impl_->units) {
    auto &data = *unit->impl_;
    if (data.kind == CompilationUnitKind::ForeignBinding) {
      std::unordered_set<std::string> seen;
      for (std::size_t index = 0;
           data.cfdl_syntax && index < data.cfdl_syntax->imports.size();
           ++index) {
        const auto &module_name = data.cfdl_syntax->imports[index];
        const auto name = impl_->values->internIdentifier(module_name);
        UnitImport import;
        const auto found = modules.find(name.index);
        if (found != modules.end()) {
          const auto &provider = *impl_->units[found->second.index];
          if (provider.kind() != CompilationUnitKind::ForeignBinding) {
            data.diagnostics.emit(DiagnosticKind::CFDLInvalidImport,
                                  data.cfdl_syntax->import_offsets[index]);
            continue;
          }
          import.local_unit = found->second;
          import.provider = {impl_->package_name, module_name};
        } else {
          for (const auto *dependency : dependency_checks) {
            const auto *artifact = dependency->findModule(module_name);
            if (!artifact ||
                artifact->unit_kind != CompilationUnitKind::ForeignBinding)
              continue;
            if (import.public_interface.hasValue()) {
              data.diagnostics.emit(DiagnosticKind::AmbiguousImport,
                                    data.cfdl_syntax->import_offsets[index]);
              import = {};
              break;
            }
            import.public_interface = impl_->public_interfaces->findByModule(
                impl_->values->internIdentifier(dependency->packageName()),
                name);
            import.provider = {std::string(dependency->packageName()),
                               module_name};
          }
          if (!import.public_interface.hasValue()) {
            data.diagnostics.emit(DiagnosticKind::UnknownImport,
                                  data.cfdl_syntax->import_offsets[index]);
            continue;
          }
        }
        const auto key =
            import.provider.package_name + "\n" + import.provider.module_name;
        if (!seen.insert(key).second) {
          data.diagnostics.emit(DiagnosticKind::DuplicateImport,
                                data.cfdl_syntax->import_offsets[index]);
          continue;
        }
        data.imports.push_back(std::move(import));
      }
      continue;
    }
    if (!data.parse_tree)
      continue;
    std::unordered_set<std::string> seen;
    std::unordered_set<std::uint32_t> seen_lookup_names;
    const auto root =
        NodeId(static_cast<std::uint32_t>(data.parse_tree->size() - 1));
    for (const auto declaration : data.parse_tree->children(root)) {
      if (data.parse_tree->kind(declaration) != NodeKind::ImportDecl)
        continue;
      const auto children = data.parse_tree->children(declaration);
      if ((children.size() != 1 && children.size() != 2) ||
          data.parse_tree->kind(children[0]) != NodeKind::ModulePath)
        continue;
      const auto module_name = modulePathText(*data.parse_tree, children[0]);
      const auto name = impl_->values->internIdentifier(module_name);
      const auto token = data.tokens->get(data.parse_tree->token(children[0]));
      IdentifierId alias;
      if (children.size() == 2 &&
          data.parse_tree->kind(children[1]) == NodeKind::Name)
        alias = impl_->values->internIdentifier(
            data.tokens->text(data.parse_tree->token(children[1])));
      const auto lookup_name = alias.hasValue() ? alias : name;
      if (!lookup_name.hasValue() ||
          !seen_lookup_names.insert(lookup_name.index).second) {
        data.diagnostics.emit(DiagnosticKind::DuplicateImport, token.offset,
                              token.length);
        continue;
      }
      if (alias.hasValue() && modules.contains(alias.index)) {
        data.diagnostics.emit(DiagnosticKind::DuplicateImport, token.offset,
                              token.length);
        continue;
      }
      const auto found = modules.find(name.index);
      UnitImport import;
      if (found != modules.end()) {
        import.local_unit = found->second;
        import.provider = {impl_->package_name, module_name};
      } else {
        if (dependency_checks.empty()) {
          for (const auto *dependency : dependency_manifests) {
            const auto *artifact = dependency->findModule(module_name);
            if (!artifact)
              continue;
            if (import.public_interface.hasValue()) {
              data.diagnostics.emit(DiagnosticKind::AmbiguousImport,
                                    token.offset, token.length);
              import = {};
              break;
            }
            const auto package_id =
                impl_->values->internIdentifier(dependency->packageName());
            import.public_interface =
                impl_->public_interfaces->findByModule(package_id, name);
            import.provider = {std::string(dependency->packageName()),
                               module_name};
          }
        }
        for (const auto *dependency : dependency_checks) {
          const auto *artifact = dependency->findModule(module_name);
          if (!artifact)
            continue;
          if (import.public_interface.hasValue()) {
            data.diagnostics.emit(DiagnosticKind::AmbiguousImport, token.offset,
                                  token.length);
            import = {};
            break;
          }
          const auto package_id =
              impl_->values->internIdentifier(dependency->packageName());
          import.public_interface =
              impl_->public_interfaces->findByModule(package_id, name);
          import.provider = {std::string(dependency->packageName()),
                             module_name};
        }
        if (!import.public_interface.hasValue()) {
          if (!data.diagnostics.hasError())
            data.diagnostics.emit(DiagnosticKind::UnknownImport, token.offset,
                                  token.length);
          continue;
        }
      }
      const auto import_key =
          import.provider.package_name + "\n" + import.provider.module_name;
      if (!seen.insert(import_key).second) {
        data.diagnostics.emit(DiagnosticKind::DuplicateImport, token.offset,
                              token.length);
        continue;
      }
      import.is_export = hasNodeFlag(data.parse_tree->get(declaration).flags,
                                     NodeFlags::IsExport);
      import.alias = alias;
      data.imports.push_back(std::move(import));
    }
  }
