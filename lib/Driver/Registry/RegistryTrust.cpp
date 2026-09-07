#include "chtholly/Driver/RegistryTrust.h"

#include "chtholly/Driver/RegistryTransparency.h"

#include "ManifestToml.h"
#include "RegistryCrypto.h"
#include "chtholly/Driver/RegistrySigner.h"
#include "chtholly/Support/Digest.h"
#include "chtholly/Support/FileSystem.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace chtholly {
namespace {

#include "RegistryTrustHelpers.inc"
} // namespace

#include "RegistryTrustMetadata.inc"
#include "RegistryTrustVerification.inc"
#include "RegistryTrustSigning.inc"

} // namespace chtholly
