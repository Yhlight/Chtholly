#pragma once

// This header is included inside the IncrementalDependencies implementation's
// private namespace. It contains the bounded package-manifest codec helpers;
// the public manifest methods remain the only entry points.

constexpr std::string_view StateMagic = "CHNXTPK79";
#include "IncrementalDependenciesCodecPrimitives.inc"
#include "IncrementalDependenciesCodecPublic.inc"
#include "IncrementalDependenciesCodecOwnership.inc"
#include "IncrementalDependenciesCodecForeign.inc"
#include "IncrementalDependenciesCodecArtifacts.inc"
