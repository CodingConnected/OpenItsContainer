#pragma once

#include "../gen-fi/GenFiTypes.hpp"
#include <string>

namespace tlcfi {

/**
 * @brief Product name and version information for certified iTLC components
 *
 * @see TLC-FI Spec v2.0.1 §5.1.1
 */
struct ProductVersion {
	int major {0};      ///< Major version number (0-1000)
	int minor {0};      ///< Minor version number (0-1000)
	int revision {0};   ///< Revision number (0-1000)
};

/**
 * @brief Facilities information structure
 *
 * Contains identification and version information about the TLC Facilities.
 * Fields vary between protocol versions:
 * - v1.1.0: Uses fiVersion and facilitiesVersion
 * - v2.0.1: Uses certifiedName and certifiedVersion for iTLC certification
 *
 * @see TLC-FI Spec v1.1.0 §5.1.1
 * @see TLC-FI Spec v2.0.1 §5.1.1
 */
struct FacilitiesInformation {
	/// @brief Company name (present in both versions)
	/// @note In v2.0.1, must match iTLC certificate literally
	std::string companyname;

#if defined(TLCFI_VERSION_2_0_1)
	/// @brief Certified product name (iTLC Component)
	/// @note Added in TLC-FI 2.0.1
	std::string certifiedName;

	/// @brief Certified product version
	/// @note Added in TLC-FI 2.0.1
	ProductVersion certifiedVersion;

	/// @brief Current protocol version
	/// @note Added in TLC-FI 2.0.1
	genfi::ProtocolVersion version;

#elif defined(TLCFI_VERSION_1_1_0)
	/// @brief Facilities Interface version
	/// @note Removed in TLC-FI 2.0.1
	std::string fiVersion;

	/// @brief Facilities version identifier
	/// @note Removed in TLC-FI 2.0.1
	std::string facilitiesVersion;
#endif
};

} // namespace tlcfi
