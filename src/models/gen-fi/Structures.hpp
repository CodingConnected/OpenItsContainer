#pragma once

#include "GenFiTypes.hpp"

namespace tlcfi::genfi {

/**
 * @brief A session event information object
 * 
 * This object describes additional information related to a session event. Which
 * object and attribute caused the event.
 * Attribute contains the string representing the attribute.
 * 
 * @see GEN-FI Spec §6.5
 */
struct SessionEventInformation {
	/// @brief Object type that caused the event (integer; see e.g. TLCObjectType)
	int type {0};
	
	/// @brief Object identifier that caused the event
	ObjectID id;
	
	/// @brief Attribute name that caused the event
	std::string attribute;
};

/**
 * @brief A session event object
 * 
 * This object describes an event generated within a session.
 * 
 * @see GEN-FI Spec §6.5
 */
struct SessionEvent {
	/// @brief Session event code
	/// @note 0-999: Generic codes
	///       1000-1999: TLC-FI codes
	///       2000-2999: RIS-FI codes
	int code {0};
	
	/// @brief Optional additional information about the event
	std::optional<SessionEventInformation> info;
};

} // namespace tlcfi::genfi


