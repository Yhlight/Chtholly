// Included by IncrementalDependenciesCodecInternal.h; this fragment keeps
// canonical package-manifest codec helpers in the private namespace.
constexpr std::uint32_t StateFormatVersion = 77;
constexpr std::size_t MinimumFunctionEncodingSize =
    4 + 4 + 4 + 4 + 4 + 4 + 5 + 1 + 4 + 1 + 4 + 1 + 1 + 1 + 1 + 1 + 1 + 4 +
    StableFingerprint::ByteCount;
constexpr std::size_t MinimumInterfaceEncodingSize =
    4 + 4 + StableFingerprint::ByteCount + 4 + 4 + 4;
constexpr std::size_t MinimumObservationEncodingSize =
    2 + 4 + 4 + 4 + StableFingerprint::ByteCount * 3 + 4 + 4 + 4;
constexpr std::size_t MinimumModuleEncodingSize =
    StableFingerprint::ByteCount + 1 + MinimumInterfaceEncodingSize + 4 + 4 +
    StableFingerprint::ByteCount;

bool validModuleDependencies(const std::vector<ModuleIdentity> &values,
                             std::string_view owner, std::string &error) {
  for (std::size_t index = 0; index < values.size(); ++index) {
    const auto &value = values[index];
    if (value.package_name.empty() || value.module_name.empty() ||
        (index != 0 && values[index - 1] >= value)) {
      error = std::string(owner) + " has invalid module dependencies";
      return false;
    }
  }
  return true;
}

bool validForeignSymbols(const std::vector<ForeignSymbolRequirement> &values,
                         std::string_view owner, std::string &error) {
  if (!std::ranges::is_sorted(values) ||
      std::adjacent_find(values.begin(), values.end()) != values.end() ||
      std::ranges::any_of(values, [](const auto &value) {
        return value.logical_name.empty() || value.external_symbol.empty() ||
               value.calling_convention >= ForeignCallingConvention::Count ||
               !value.signature_fingerprint.hasValue();
      })) {
    error = std::string(owner) + " has invalid foreign symbol requirements";
    return false;
  }
  for (std::size_t index = 1; index < values.size(); ++index)
    if (values[index - 1].external_symbol == values[index].external_symbol &&
        (values[index - 1].calling_convention !=
             values[index].calling_convention ||
         values[index - 1].signature_fingerprint !=
             values[index].signature_fingerprint)) {
      error = std::string(owner) +
              " has conflicting foreign symbol ABI requirements";
      return false;
    }
  return true;
}

PackageProvenance canonicalProvenance(std::string_view package_name,
                                      PackageProvenance provenance) {
  if (!provenance.contract_fingerprint.hasValue()) {
    provenance.contract_fingerprint = StableFingerprint::fromCanonicalBytes(
        std::string("chtholly.next.workspace-package-contract.v1\n") +
        std::string(package_name));
  }
  return provenance;
}

std::string uniqueStateTemporaryPath(const std::string &path) {
  static std::atomic<std::uint64_t> sequence = 0;
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return path + ".tmp." + std::to_string(stamp) + "." +
         std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

void appendU32(std::string &out, std::uint32_t value) {
  for (std::uint32_t shift = 0; shift != 32; shift += 8)
    out.push_back(static_cast<char>((value >> shift) & 0xffU));
}

void appendField(std::string &out, std::string_view value) {
  assert(value.size() <= std::numeric_limits<std::uint32_t>::max());
  appendU32(out, static_cast<std::uint32_t>(value.size()));
  out.append(value);
}

void appendFingerprint(std::string &out, const StableFingerprint &fingerprint) {
  for (const auto byte : fingerprint.bytes())
    out.push_back(static_cast<char>(byte));
}

void appendCFFIIdentity(std::string &out, const CFFIReceiptIdentity &identity) {
  appendField(out, identity.target);
  appendField(out, identity.compiler_family);
  appendField(out, identity.clang_version);
  appendField(out, identity.libclang);
  appendField(out, identity.compiler);
  appendField(out, identity.compiler_version);
  appendField(out, identity.toolchain);
  appendField(out, identity.sdk);
  appendField(out, identity.config);
  appendField(out, identity.headers);
  appendField(out, identity.cfdl);
  appendField(out, identity.probe);
  appendField(out, identity.facts);
}

void appendForeignRequirement(std::string &out,
                              const ForeignSymbolRequirement &value) {
  appendField(out, value.logical_name);
  appendField(out, value.external_symbol);
  out.push_back(static_cast<char>(value.calling_convention));
  appendFingerprint(out, value.signature_fingerprint);
}

auto observationKey(const DependencyObservation &value) {
  return std::tuple(value.kind, std::string_view(value.provider.package_name),
                    std::string_view(value.provider.module_name),
                    std::string_view(value.binding_name),
                    std::string_view(value.canonical_provider.package_name),
                    std::string_view(value.canonical_provider.module_name),
                    std::string_view(value.canonical_name),
                    value.lifecycle_role, value.expected_fingerprint.hex());
}

auto invalidationKey(const UnitInvalidation &value) {
  return std::tuple(value.reason, std::string_view(value.provider.package_name),
                    std::string_view(value.provider.module_name),
                    std::string_view(value.binding_name));
}

void canonicalizeObservations(std::vector<DependencyObservation> &values) {
  std::ranges::sort(values, [](const auto &lhs, const auto &rhs) {
    return observationKey(lhs) < observationKey(rhs);
  });
}

void canonicalizeInvalidations(std::vector<UnitInvalidation> &values) {
  std::ranges::sort(values, [](const auto &lhs, const auto &rhs) {
    return invalidationKey(lhs) < invalidationKey(rhs);
  });
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

bool readCFFIIdentity(StateReader &reader, CFFIReceiptIdentity &identity) {
  return reader.readString(identity.target) &&
         reader.readString(identity.compiler_family) &&
         reader.readString(identity.clang_version) &&
         reader.readString(identity.libclang) &&
         reader.readString(identity.compiler) &&
         reader.readString(identity.compiler_version) &&
         reader.readString(identity.toolchain) &&
         reader.readString(identity.sdk) &&
         reader.readString(identity.config) &&
         reader.readString(identity.headers) &&
         reader.readString(identity.cfdl) &&
         reader.readString(identity.probe) && reader.readString(identity.facts);
}
