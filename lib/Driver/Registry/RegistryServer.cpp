#include "chtholly/Driver/RegistryServer.h"

#include "ManifestToml.h"
#include "RegistryCrypto.h"
#include "RegistryDatabaseInternal.h"
#include "RegistryServerSupportInternal.h"
#include "chtholly/Driver/ProcessRunner.h"
#include "chtholly/Driver/RegistryBackup.h"
#include "chtholly/Driver/SemVer.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <sodium.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_set>

namespace chtholly {
using registry_internal::Statement;
using registry_internal::Transaction;
using registry_internal::execute;
using registry_internal::effectiveNow;
using registry_internal::isHexDigest;
using registry_internal::isSafeRegistryPackageName;
using registry_internal::isValidHttpsBaseUrl;
using registry_internal::mutationDigest;
using registry_internal::pathIsWithin;
using registry_internal::rfc3339;
using registry_internal::runGit;
using registry_internal::rootRequest;
using registry_internal::copyIntoCas;
using registry_internal::resolveConfigPath;

namespace {

bool sameVariantFacts(const RegistryArtifactVariant &lhs,
                      const RegistryArtifactVariant &rhs) {
  return lhs.name == rhs.name && lhs.target.triple == rhs.target.triple &&
         lhs.target.pointer_width_bits == rhs.target.pointer_width_bits &&
         lhs.abi_version == rhs.abi_version &&
         lhs.runtime_abi == rhs.runtime_abi &&
         lhs.requested_features == rhs.requested_features &&
         lhs.default_features == rhs.default_features && lhs.url == rhs.url &&
         lhs.archive_size == rhs.archive_size &&
         lhs.archive_sha256 == rhs.archive_sha256 &&
         lhs.artifact_identity == rhs.artifact_identity &&
         lhs.closure_digest == rhs.closure_digest;
}

} // namespace

struct RegistryPublicationStore::Impl {
  RegistryServerConfig config;
  sqlite3 *database = nullptr;
  mutable std::mutex mutex;
  std::unique_ptr<RegistrySigningProvider> signer;

  ~Impl() { sqlite3_close(database); }

#include "RegistryServerImplInitialization.inc"

#include "RegistryServerImplSecurity.inc"
#include "RegistryServerImplPublication.inc"
#include "RegistryServerImplRecovery.inc"
};

#include "RegistryServerPublicOpen.inc"
#include "RegistryServerPublicPublisher.inc"
#include "RegistryServerPublicOperator.inc"
#include "RegistryServerPublicPublish.inc"
#include "RegistryServerPublicLifecycle.inc"
#include "RegistryServerPublicRecovery.inc"
#include "RegistryServerPublicGc.inc"
#include "RegistryServerPublicQueries.inc"
} // namespace chtholly
