#pragma once

// Private concrete-specialization encoding, decoding, and body verification
// helpers. Included inside ConcreteSpecialization.cpp's anonymous namespace.

constexpr std::string_view ComponentMagic = "CHNXSCC51";
#include "ConcreteSpecializationCodecEncoding.inc"
#include "ConcreteSpecializationCodecReader.inc"
#include "ConcreteSpecializationCodecContracts.inc"
#include "ConcreteSpecializationBodyVerification.inc"
