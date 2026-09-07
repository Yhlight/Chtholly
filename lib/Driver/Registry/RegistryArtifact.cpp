#include "chtholly/Driver/RegistryArtifact.h"

#include "ManifestToml.h"
#include "RegistryCrypto.h"
#include "chtholly/Driver/RegistryServer.h"
#include "chtholly/Driver/RegistryWitness.h"
#include "chtholly/Driver/SemVer.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <curl/curl.h>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#include <aclapi.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace chtholly {
namespace {
#include "RegistryArtifactSupportInternal.h"

} // namespace

#include "RegistryArtifactParsing.inc"
#include "RegistryArtifactSigning.inc"
#include "RegistryArtifactPublication.inc"
#include "RegistryArtifactLifecycle.inc"
#include "RegistryArtifactGossip.inc"
#include "RegistryArtifactFetch.inc"

} // namespace chtholly
