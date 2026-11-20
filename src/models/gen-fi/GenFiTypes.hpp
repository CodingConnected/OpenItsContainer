#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <variant>

#include <nlohmann/json.hpp>

namespace tlcfi::genfi {

/**
 * @brief A time represented as a number of ticks
 * 
 * A tick is the basic unit of relative time for an application and a Facilities Interface,
 * per session (values between sessions are not related). The value wraps around when the
 * maximum is reached.
 * 
 * Range: 0 to 4294967295
 * Unit: 1 millisecond
 * 
 * @note Every increment of 1 corresponds with 1ms incremented time of the sender.
 *       Always incrementing. On overflow, wraps properly back to a value making sure
 *       that it is always possible to deduct the previous interval.
 * 
 * @see GEN-FI Spec §5.2, §6.2
 */
using Ticks = std::uint64_t;

/**
 * @brief A time stamp
 * 
 * The number of milliseconds since 1-1-1970 00:00:00 UTC.
 * 
 * Range: 0 to 18446744073709551615
 * Unit: 1 millisecond
 * 
 * @note Calendar time expressed as UTC. There may be jumps in this time as it is
 *       being kept up-to-date by for instance NTP.
 * 
 * @see GEN-FI Spec §5.3, §6.2
 */
using Timestamp = std::uint64_t;

/**
 * @brief Speed value in meters per second
 * 
 * Speed value in meters per second.
 * When the information is not available, the value shall be set to null.
 * 
 * Range: 0.0 to 99.0
 * Unit: meter / second
 * 
 * @see GEN-FI Spec §6.2
 */
using Speed = double;

/**
 * @brief Length value in meters
 * 
 * Length value.
 * The value shall be set to null if the information is unavailable.
 * 
 * Range: 0 to 429496729.5
 * Unit: meter
 * 
 * @see GEN-FI Spec §6.2
 */
using Length = double;

/**
 * @brief Protocol version structure
 * 
 * Structure containing the protocol version.
 * The versioning scheme: major.minor.revision
 * 
 * - major: Used to indicate major technical or functional change to the protocol.
 *   Possibly breaking backwards compatibility.
 * - minor: Increasing values indicate minor changes to the protocol-definition.
 *   Compatibility is guaranteed.
 * - revision: Minor changes to specification, clarifications and typographical errors.
 *   Compatibility is guaranteed.
 * 
 * Range: major: 0-1000, minor: 0-1000, revision: 0-1000
 * 
 * @see GEN-FI Spec §5.8, §6.3
 */
struct ProtocolVersion {
	/// @brief Major version number
	int major {0};
	
	/// @brief Minor version number
	int minor {0};
	
	/// @brief Revision number
	int revision {0};
};

/**
 * @brief A geographical location
 * 
 * This object describes a WGS84 location.
 * 
 * @see GEN-FI Spec §6.2
 */
struct Location {
	/// @brief Latitude in degrees
	/// @note Range: -90.000000 to 90.000000
	double latitude {0.0};
	
	/// @brief Longitude in degrees
	/// @note Range: -180.000000 to 180.000000
	double longitude {0.0};
	
	/// @brief Elevation in meters (optional)
	/// @note Range: -100.000 to 8000.000
	std::optional<double> elevation;
};

/**
 * @brief Object Identifier
 * 
 * A unique identifier for an object instance per ObjectType.
 * Recommendation is to use functional names of the objects, for instance "D02"
 * for a detector, signal group "FC02".
 * 
 * Allowed characters: 'a-z' (ASCII 97-122), 'A-Z' (ASCII 65-90), '0-9',
 * '_' (underscore, ASCII 95) and '-' (hyphen, ASCII 45).
 * 
 * @see GEN-FI Spec §6.2
 */
using ObjectID = std::string;

/**
 * @brief Error code for protocol requests
 * 
 * Error code used for protocol requests, this is an extension of JSON-RPC errors.
 * Passed as the code attribute of an error object.
 * 
 * Error code ranges:
 * - 0-999: Generic Error codes
 * - 1000-1999: TLC-FI Error codes
 * - 2000-2999: RIS-FI Error codes
 * 
 * @see GEN-FI Spec §6.2, §11
 */
enum class ProtocolErrorCode : int {
	Error = 0,                  ///< Generic error
	NotAuthorised = 1,          ///< Not authorized
	NoRights = 2,               ///< No rights
	InvalidProtocol = 3,        ///< Invalid protocol
	AlreadyRegistered = 4,       ///< Already registered
	UnknownObjectType = 5,       ///< Unknown object type
	MissingAttribute = 6,        ///< Missing attribute
	InvalidAttributeType = 7,    ///< Invalid attribute type
	InvalidAttributeValue = 8,   ///< Invalid attribute value
	InvalidObjectReference = 9,  ///< Invalid object reference
};

/**
 * @brief Object reference
 * 
 * A reference to a number of objects of the same type.
 * 
 * @see GEN-FI Spec §6.2
 */
struct ObjectReference {
	/// @brief Object type
	/// @note Supports either integer enum (e.g., TLC-FI) or string type name
	std::variant<int, std::string> type;
	
	/// @brief List of object identifiers
	std::vector<ObjectID> ids;
};

/**
 * @brief An alive object
 * 
 * This describes an Alive object used for alive checking mechanism.
 * Part of the Protocol methods. An ITS Application which has been registered
 * must continuously send an Alive message and it assumes that it receives a similar
 * alive message from the Facilities with the same interval.
 * 
 * As part of the Alive checking, the sender provides:
 * - Actual UTC time (in milliseconds)
 * - Actual time-tick (in milliseconds)
 * 
 * @see GEN-FI Spec §5.6, §6.6
 */
struct AliveObject {
	/// @brief Actual time-tick (in milliseconds)
	Ticks ticks {0};
	
	/// @brief Actual UTC time (in milliseconds)
	Timestamp time {0};
};

/**
 * @brief Object Meta data
 * 
 * An object describing the Meta data of one or more objects. The ObjectMeta is the
 * contents of the Meta{} scope identifier of the Object.
 * The ticks attribute defines the tick at which the data update is sent.
 * 
 * @see GEN-FI Spec §6.2
 */
struct ObjectMeta {
	/// @brief Reference to the objects
	ObjectReference objects;
	
	/// @brief Meta data content (abstract ObjectMetaContent)
	std::vector<nlohmann::json> meta;
	
	/// @brief Tick at which the data update is sent
	Ticks ticks {0};
};

/**
 * @brief Object update
 * 
 * An object describing the data of one or more objects. The ObjectData is the
 * contents of the Object except the Meta{} scope.
 * The update of all objects mentioned in objects is atomic.
 * The ticks attribute defines the tick at which the data update is sent.
 * 
 * @see GEN-FI Spec §6.2
 */
struct ObjectData {
	/// @brief Reference to the objects
	ObjectReference objects;
	
	/// @brief Data content (abstract ObjectDataContent)
	std::vector<nlohmann::json> data;
	
	/// @brief Tick at which the data update is sent
	Ticks ticks {0};
};

/**
 * @brief Object state update
 * 
 * An object describing a state update of one or more objects. The ObjectStateUpdate
 * is the contents of the State{} scope of the objects.
 * The update of all objects mentioned in objects is atomic.
 * 
 * @see GEN-FI Spec §6.2
 */
struct ObjectStateUpdate {
	/// @brief Reference to the objects
	ObjectReference objects;
	
	/// @brief State content (abstract ObjectStateUpdateContent)
	std::vector<nlohmann::json> states;
};

/**
 * @brief Group Object state update
 *
 * This object is used to define a group of object state updates.
 * The different state updates are in the update attribute. The ticks attribute
 * defines the tick from which the states in the update are valid.
 *
 * @see GEN-FI Spec §6.2
 */
struct ObjectStateUpdateGroup {
	/// @brief List of object state updates
	std::vector<ObjectStateUpdate> update;

	/// @brief Tick from which the states in the update are valid
	Ticks ticks {0};
};

/**
 * @brief Object event notification
 *
 * An object describing an event notification for one or more objects.
 * This is the contents of the NotifyEvent notification from the Facilities.
 * The notification of all events mentioned in objects is atomic.
 *
 * @see TLC-FI Spec §5.4.4 (Event notifications)
 */
struct NotifyEventUpdate {
	/// @brief Reference to the objects
	ObjectReference objects;

	/// @brief Event content (abstract event data)
	std::vector<nlohmann::json> events;
};

/**
 * @brief Group event notification
 *
 * This object is used to define a group of event notifications.
 * The different event notifications are in the events attribute. The ticks attribute
 * defines the tick at which the events occurred.
 *
 * @see TLC-FI Spec §5.4.4 (Event notifications)
 */
struct NotifyEventGroup {
	/// @brief Object reference for the events
	ObjectReference objects;

	/// @brief List of event objects
	std::vector<nlohmann::json> events;

	/// @brief Tick at which the events occurred
	Ticks ticks {0};
};

/**
 * @brief A registration request
 * 
 * This object describes the contents of a registration request.
 * Used to register an Application with the Facilities.
 * 
 * @see GEN-FI Spec §6.3, §7.1
 */
struct RegistrationRequest {
	/// @brief Application username
	/// @note Allowed characters: 'a-z', 'A-Z', '0-9', '_', '-'.
	///       Username always starts with a letter. Not case-sensitive.
	std::string username;
	
	/// @brief Application password
	/// @note Values 32-126 from ASCII character set, except '"' and ','
	std::string password;
	
	/// @brief Application type
	/// @note 0=Consumer, 1=Provider, 2=Control (ApplicationType from GEN-FI)
	int type {0};
	
	/// @brief Protocol version
	ProtocolVersion version;
	
	/// @brief Application uniform resource identifier
	/// @note Values 32-126 from ASCII character set, except '"' and ','.
	///       Further limited by URI generic syntax.
	std::string uri;
};

/**
 * @brief A registration reply
 * 
 * This object describes the contents of a registration reply. The sessionid is a
 * unique identifier created by the Facilities to identify this session. All session
 * communication uses this identifier.
 * 
 * facilities: reference to the Facilities with which this session is active.
 * 
 * @see GEN-FI Spec §6.3, §7.1
 */
struct RegistrationReply {
	/// @brief Session identifier
	/// @note Unique identifier for a session with the Facilities.
	///       This is a specific type of ObjectID used only between two peers.
	ObjectID sessionid;
	
	/// @brief Reference to the Facilities
	ObjectReference facilities;
	
	/// @brief Protocol version
	ProtocolVersion version;
};

} // namespace tlcfi::genfi


