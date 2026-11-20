#pragma once

#include "../gen-fi/GenFiTypes.hpp"

namespace tlcfi::version {

// TLC-FI Protocol Version Selection
// Build with: cmake -DTLCFI_PROTOCOL_VERSION=1.1.0 or 2.0.1
// Default: 2.0.1

#if defined(TLCFI_VERSION_2_0_1)
    // TLC-FI Version 2.0.1 (December 2025)
    constexpr genfi::ProtocolVersion PROTOCOL_VERSION{2, 0, 1};
    constexpr bool SUPPORTS_PREDICTIONS = false;
    constexpr bool SUPPORTS_VARIABLES = false;
    constexpr bool USES_CERTIFIED_FIELDS = true;  // certifiedName, certifiedVersion

#elif defined(TLCFI_VERSION_1_1_0)
    // TLC-FI Version 1.1.0 (February 2017)
    constexpr genfi::ProtocolVersion PROTOCOL_VERSION{1, 1, 0};
    constexpr bool SUPPORTS_PREDICTIONS = true;
    constexpr bool SUPPORTS_VARIABLES = true;
    constexpr bool USES_CERTIFIED_FIELDS = false;  // Uses fiVersion, facilitiesVersion

#else
    // Default to v2.0.1 if no version specified
    constexpr genfi::ProtocolVersion PROTOCOL_VERSION{2, 0, 1};
    constexpr bool SUPPORTS_PREDICTIONS = false;
    constexpr bool SUPPORTS_VARIABLES = false;
    constexpr bool USES_CERTIFIED_FIELDS = true;
#endif

} // namespace tlcfi::version
