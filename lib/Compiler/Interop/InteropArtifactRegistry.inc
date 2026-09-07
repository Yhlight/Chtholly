// Included by InteropArtifact.cpp; this fragment retains the canonical
// codec/protocol/registry state and ownership boundaries.
std::string ArtifactBundle::encode(std::string &error) const {
  error.clear();
  ArtifactBundle canonical = *this;
  canonical.canonicalize();
  if (!canonical.verify(error))
    return {};
  Writer writer;
  writer.bytes.append(Magic);
  writer.u32(format_version);
  writer.u32(static_cast<std::uint32_t>(canonical.records.size()));
  for (const auto &record : canonical.records) {
    writeArtifactReference(writer, record.reference);
    writeArtifact(writer, record.artifact);
  }
  return writer.bytes;
}

std::optional<ArtifactBundle> ArtifactBundle::decode(std::string_view bytes,
                                                     std::string &error) {
  error.clear();
  Reader reader{bytes};
  std::string_view magic;
  if (!reader.take(Magic.size(), magic) || magic != Magic) {
    error = reader.error.empty() ? "interop bundle has an invalid header"
                                 : reader.error;
    return std::nullopt;
  }
  ArtifactBundle bundle;
  std::uint32_t count = 0;
  if (!reader.u32(bundle.format_version) || !reader.count(count)) {
    error = reader.error;
    return std::nullopt;
  }
  bundle.records.resize(count);
  for (auto &record : bundle.records)
    if (!readArtifactReference(reader, record.reference) ||
        !readArtifact(reader, record.artifact)) {
      error = reader.error;
      return std::nullopt;
    }
  if (reader.offset != bytes.size()) {
    error = "interop bundle has trailing bytes";
    return std::nullopt;
  }
  if (!bundle.verify(error))
    return std::nullopt;
  return bundle;
}

bool writeArtifactBundle(const ArtifactBundle &bundle, const std::string &path,
                         std::string &error) {
  error.clear();
  const auto bytes = bundle.encode(error);
  if (bytes.empty() && !bundle.records.empty())
    return false;
  std::ofstream output(chtholly::pathForFileSystem(path),
                       std::ios::binary | std::ios::trunc);
  if (!output) {
    error = "failed to open interop sidecar for writing: '" + path + "'";
    return false;
  }
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    error = "failed to write interop sidecar: '" + path + "'";
    return false;
  }
  return true;
}

bool readArtifactBundle(const std::string &path, ArtifactBundle &bundle,
                        std::string &error) {
  error.clear();
  std::ifstream input(chtholly::pathForFileSystem(path), std::ios::binary);
  if (!input) {
    error = "failed to open interop sidecar for reading: '" + path + "'";
    return false;
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > (64U << 20U)) {
    error = "interop sidecar exceeds the input limit";
    return false;
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  input.seekg(0, std::ios::beg);
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!input && !bytes.empty()) {
    error = "failed to read interop sidecar: '" + path + "'";
    return false;
  }
  auto decoded = ArtifactBundle::decode(bytes, error);
  if (!decoded)
    return false;
  bundle = std::move(*decoded);
  return true;
}

std::string ArtifactRegistry::key(const ArtifactReference &reference) {
  std::string result;
  result.append(reference.canonical_package);
  result.push_back('\0');
  result.append(reference.canonical_module);
  result.push_back('\0');
  result.append(reference.canonical_name);
  result.push_back('\0');
  result.append(reference.fingerprint.hex());
  return result;
}

ArtifactReference ArtifactRegistry::publish(std::string_view canonical_package,
                                            std::string_view canonical_module,
                                            std::string_view canonical_name,
                                            ForeignOperationArtifact artifact,
                                            std::string &error) {
  error.clear();
  artifact.canonicalize();
  if (!artifact.verify(error))
    return {};
  ArtifactReference reference;
  reference.canonical_package = std::string(canonical_package);
  reference.canonical_module = std::string(canonical_module);
  reference.canonical_name = std::string(canonical_name);
  reference.fingerprint = artifact.fingerprint;
  if (!reference.verify(error) ||
      !registerArtifact(reference, std::move(artifact), error))
    return {};
  return reference;
}

bool ArtifactRegistry::registerArtifact(const ArtifactReference &reference,
                                        ForeignOperationArtifact artifact,
                                        std::string &error) {
  error.clear();
  if (!reference.verify(error))
    return false;
  artifact.canonicalize();
  if (!artifact.verify(error) || artifact.fingerprint != reference.fingerprint)
    return false;
  const auto artifact_key = key(reference);
  if (const auto found = keys_.find(artifact_key); found != keys_.end()) {
    if (artifacts_.get(found->second) != artifact) {
      error = "interop artifact reference is already bound to different data";
      return false;
    }
    return true;
  }
  const auto id = artifacts_.add(std::move(artifact));
  keys_.emplace(artifact_key, id);
  return true;
}

bool ArtifactRegistry::registerBundle(const ArtifactBundle &bundle,
                                      std::string &error) {
  error.clear();
  ArtifactBundle candidate = bundle;
  if (!candidate.verify(error))
    return false;
  ArtifactRegistry staged = *this;
  for (const auto &record : candidate.records)
    if (!staged.registerArtifact(record.reference, record.artifact, error))
      return false;
  *this = std::move(staged);
  return true;
}

ArtifactBundle ArtifactRegistry::exportBundle(std::string &error) const {
  error.clear();
  ArtifactBundle bundle;
  for (const auto &[artifact_key, id] : keys_) {
    if (!artifacts_.contains(id)) {
      error = "interop artifact registry contains a stale identity";
      return {};
    }
    const auto &artifact = artifacts_.get(id);
    const auto first = artifact_key.find('\0'),
               second = artifact_key.find('\0', first + 1),
               third = artifact_key.find('\0', second + 1);
    ArtifactBundleRecord record;
    record.reference.canonical_package = artifact_key.substr(0, first);
    record.reference.canonical_module =
        artifact_key.substr(first + 1, second - first - 1);
    record.reference.canonical_name =
        artifact_key.substr(second + 1, third - second - 1);
    record.reference.fingerprint = artifact.fingerprint;
    record.artifact = artifact;
    bundle.records.push_back(std::move(record));
  }
  bundle.canonicalize();
  if (!bundle.verify(error))
    return {};
  return bundle;
}

const ForeignOperationArtifact *
ArtifactRegistry::resolve(const ArtifactReference &reference) const {
  std::string error;
  if (!reference.verify(error))
    return nullptr;
  const auto found = keys_.find(key(reference));
  return found == keys_.end() ? nullptr : artifacts_.tryGet(found->second);
}

const ForeignProtocolIdentity *ArtifactRegistry::findProtocolIdentity(
    ForeignProtocolIdentityKind kind, std::string_view canonical_package,
    std::string_view canonical_module, std::string_view canonical_resource,
    std::string_view canonical_name, std::string_view owner_callable,
    std::string &error) const {
  error.clear();
  const ForeignProtocolIdentity *result = nullptr;
  for (const auto &[artifact_key, id] : keys_) {
    (void)artifact_key;
    if (!artifacts_.contains(id)) {
      error = "interop artifact registry contains a stale identity";
      return nullptr;
    }
    for (const auto &identity : artifacts_.get(id).protocol_identities) {
      if (identity.kind != kind ||
          identity.canonical_package != canonical_package ||
          identity.canonical_module != canonical_module ||
          (!canonical_resource.empty() &&
           identity.canonical_resource != canonical_resource) ||
          identity.canonical_name != canonical_name ||
          identity.owner_callable != owner_callable)
        continue;
      if (result && *result != identity) {
        error = "interop protocol reference is ambiguous";
        return nullptr;
      }
      result = &identity;
    }
  }
  if (!result)
    error = "interop protocol reference is unresolved";
  return result;
}

bool ArtifactRegistry::verify(std::string &error) const {
  error.clear();
  if (keys_.size() != artifacts_.size()) {
    error = "interop artifact registry has inconsistent identity count";
    return false;
  }
  struct ProtocolState {
    ForeignProtocolIdentity identity;
    std::uint32_t obligations = 0;
    std::uint32_t discharges = 0;
    bool quiescent = false;
  };
  std::vector<ProtocolState> protocols;
  for (const auto &[artifact_key, id] : keys_) {
    if (!artifacts_.contains(id)) {
      error = "interop artifact registry contains a stale identity";
      return false;
    }
    if (artifact_key.empty() || !artifacts_.get(id).verify(error))
      return false;
    const auto &artifact = artifacts_.get(id);
    for (const auto &identity : artifact.protocol_identities) {
      auto found = std::ranges::find_if(protocols, [&](const auto &state) {
        return state.identity.fingerprint == identity.fingerprint;
      });
      if (found == protocols.end()) {
        protocols.push_back({identity});
      } else if (found->identity != identity) {
        error = "interop registry has conflicting protocol identities";
        return false;
      }
    }
  }
  std::unordered_set<std::string> invoked_events;
  for (const auto &[artifact_key, id] : keys_) {
    (void)artifact_key;
    const auto &artifact = artifacts_.get(id);
    for (const auto &edge : artifact.protocol_edges) {
      auto found = std::ranges::find_if(protocols, [&](const auto &state) {
        return state.identity.fingerprint == edge.source;
      });
      if (found == protocols.end()) {
        error = "interop registry protocol edge has an unresolved source";
        return false;
      }
      const auto target =
          std::ranges::find_if(protocols, [&](const auto &state) {
            return state.identity.fingerprint == edge.target;
          });
      if (target == protocols.end()) {
        error = "interop registry protocol edge has an unresolved target";
        return false;
      }
      if (edge.owner_callable.empty()) {
        error = "interop registry protocol edge has no owner callable";
        return false;
      }
      if (edge.kind == ForeignProtocolEdgeKind::Invokes) {
        if (found->identity.kind != ForeignProtocolIdentityKind::Event) {
          error =
              "interop registry invoke edge references a non-event identity";
          return false;
        }
        if (found->identity.owner_callable == edge.owner_callable)
          invoked_events.insert(edge.source.hex());
      } else if ((edge.kind == ForeignProtocolEdgeKind::Obliges ||
                  edge.kind == ForeignProtocolEdgeKind::Discharges ||
                  edge.kind == ForeignProtocolEdgeKind::Requires) &&
                 found->identity.kind != ForeignProtocolIdentityKind::Action) {
        error = "interop registry action edge references a non-action identity";
        return false;
      } else if (edge.kind == ForeignProtocolEdgeKind::Escapes &&
                 found->identity.kind != ForeignProtocolIdentityKind::Event) {
        error = "interop registry escape edge references a non-event identity";
        return false;
      }
      if (edge.kind == ForeignProtocolEdgeKind::Obliges)
        ++found->obligations;
      else if (edge.kind == ForeignProtocolEdgeKind::Discharges)
        ++found->discharges;
      else if (edge.kind == ForeignProtocolEdgeKind::Requires &&
               edge.predicate == "predicate=quiescent")
        found->quiescent = true;
    }
  }
  for (const auto &state : protocols) {
    if (state.identity.kind == ForeignProtocolIdentityKind::Event &&
        !invoked_events.contains(state.identity.fingerprint.hex())) {
      error = "interop registry contains an unreachable event identity";
      return false;
    }
  }
  for (const auto &state : protocols) {
    if ((state.obligations != 0 && state.discharges != 1) ||
        (state.quiescent && state.discharges == 0)) {
      error = "interop registry has an incomplete protocol closure";
      return false;
    }
  }
  return true;
}
