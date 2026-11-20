#pragma once

#include "../gen-fi/GenFiTypes.hpp"
#include "Types.hpp"
#include "Enums.hpp"

#include <optional>
#include <vector>

namespace tlcfi::tlcfi {

/**
 * @brief An ITS Control Application object (Session type)
 * 
 * This describes a session with an ITS Control Application.
 * The object is of type Session.
 * 
 * Access: Control applications have R/W access.
 * 
 * @see TLC-FI Spec §5.2
 */
struct ControlApplication {
	// META attributes (read-only)
	/// @brief Session identifier (read-only)
	std::string sessionid;
	
	/// @brief Application type (read-only). Value 2 = ControlApplication
	int type {2};

	// STATE attributes
	/// @brief Handover capability when starting control (write-only)
	/// @note Written by ITS-CLA to indicate supported handover methods
	std::optional<HandoverCapability> startCapability;
	
	/// @brief Handover capability when ending control (write-only)
	/// @note Written by ITS-CLA to indicate supported handover methods
	std::optional<HandoverCapability> endCapability;
	
	/// @brief Requested intersection identifier (write-only)
	/// @note Written by ITS-CLA to request control of a specific intersection
	std::optional<std::string> reqIntersection;
	
	/// @brief Requested control state (write-only)
	/// @note Written by ITS-CLA to request state transitions
	std::optional<ControlState> reqControlState;
	
	/// @brief Current control state (read-only)
	/// @note Managed by TLC Facilities, reflects actual state machine state
	ControlState controlState {ControlState::NotConfigured};
	
	/// @brief Requested handover method (read-only)
	/// @note Set by TLC Facilities when initiating handover
	std::optional<HandoverCapability> reqHandover;
};

/**
 * @brief A signal conflict definition
 * 
 * A conflict with a given SignalGroup, and the time to wait after that SignalGroup
 * turned to STOP to clear the intersection.
 * 
 * @see TLC-FI Spec §5.7
 */
struct SignalConflict {
	/// @brief Signal group identifier (ObjectID<signalgroup>)
	std::string signalgroup;
	
	/// @brief Intergreen time in 0.1s units
	/// @note Range: 0 to 65535 (0.1s units)
	int intergreentime {0};
};

/**
 * @brief Safety timing of a signal group
 * 
 * The minimum safety time a signal group must be in a state and the maximum time
 * a signal group may be in a state.
 * 
 * @see TLC-FI Spec §5.7
 */
struct SignalTiming {
	/// @brief Signal group state this timing applies to
	SignalGroupState state {SignalGroupState::Unavailable};
	
	/// @brief Minimum time in state (0.1s units)
	/// @note Range: 0 to 65535 or null (undefined)
	std::optional<int> min;
	
	/// @brief Maximum time in state (0.1s units)
	/// @note Range: 0 to 65535 or null (undefined)
	std::optional<int> max;
};

#if defined(TLCFI_VERSION_1_1_0)
/**
 * @brief A signal group prediction update
 *
 * Prediction of the end time of a specific signal group state. This structure is used by
 * an ITS-CLA when it updates the predictions in the TLC Facilities. And by the TLC
 * Facilities when it provides the predictions to consuming ITS-A's.
 *
 * The prediction is provided in ticks.
 *
 * @note Removed in TLC-FI 2.0.1
 * @see TLC-FI Spec v1.1.0 §5.7
 */
struct SignalGroupPrediction {
	/// @brief Signal group state this prediction applies to
	SignalGroupState state {SignalGroupState::Unavailable};

	/// @brief Tick at which this state is started, or expected to start
	std::optional<genfi::Ticks> startTime;

	/// @brief Minimum tick at which this state may end
	genfi::Ticks minEnd {0};

	/// @brief Maximum tick at which or before this state must end
	std::optional<genfi::Ticks> maxEnd;

	/// @brief Likely tick at which this state will end
	std::optional<genfi::Ticks> likelyEnd;

	/// @brief Percentage confidence value of the likely prediction
	/// @note Range: 0 to 100, null = unknown
	std::optional<int> confidence;

	/// @brief Rough estimate of the tick at which this state will be activated next
	std::optional<genfi::Ticks> next;
};
#endif // TLCFI_VERSION_1_1_0

/**
 * @brief An intersection object
 * 
 * An object defining an intersection.
 * The stateticks attribute defines the tick of the TLC Facilities when the state
 * attribute within the STATE scope was last changed.
 * 
 * Access: Consumer (R), Provider (R), Control (R/W)
 * 
 * @note An ITS-CLA can only write reqState when ControlApplication.controlState =
 *       StartControl, InControl or EndControl. Writing reqState during any other
 *       ControlApplication.controlState will result in an error.
 * 
 * @see TLC-FI Spec §5.5
 */
struct Intersection {
	// META attributes (read-only)
	/// @brief Intersection identifier (read-only)
	std::string id;
	
	/// @brief List of output identifiers (read-only)
	std::vector<std::string> outputs;
	
	/// @brief List of input identifiers (read-only)
	std::vector<std::string> inputs;
	
	/// @brief List of signal group identifiers (read-only)
	std::vector<std::string> signalgroups;
	
	/// @brief List of detector identifiers (read-only)
	std::vector<std::string> detectors;
	
	/// @brief Special vehicle event generator identifier (read-only)
	std::optional<std::string> spvehgenerator;
	
	/// @brief Tick when state was last changed (read-only)
	genfi::Ticks stateticks {0};
	
	// STATE attributes
	/// @brief Current intersection control state (read-only)
	IntersectionControlState state {IntersectionControlState::Error};
	
	/// @brief Requested intersection control state (write-only)
	/// @note Can only be written when ControlApplication.controlState is
	///       StartControl, InControl, or EndControl
	std::optional<IntersectionControlState> reqState;
};

/**
 * @brief A detector object
 * 
 * This object describes a detector.
 * The stateticks attribute defines the tick of the TLC Facilities when the state
 * attribute within the STATE scope was last changed.
 * 
 * Access: Consumer (R), Provider (R), Control (R)
 * 
 * Events: DetectorEvent
 * 
 * @see TLC-FI Spec §5.3
 */
struct Detector {
	// META attributes (read-only)
	/// @brief Detector identifier (read-only)
	std::string id;
	
	/// @brief Whether this detector generates events (read-only)
	bool generatesEvents {false};
	
	/// @brief Tick when state was last changed (read-only)
	genfi::Ticks stateticks {0};
	
	// STATE attributes (read-only)
	/// @brief Current detector state (read-only)
	DetectorState state {DetectorState::Unoccupied};
	
	/// @brief Detector fault state (read-only)
	DetectorFaultState faultstate {DetectorFaultState::None};
	
	/// @brief Software input commando (SWICO) state (read-only)
	SwicoState swico {SwicoState::NoSwico};
};

/**
 * @brief An input object
 * 
 * This object describes an input. SwicoOff sets the state to 0, SwicoOn sets
 * the state to 1.
 * The stateticks attribute defines the tick of the TLC Facilities when the state
 * attribute within the STATE scope was last changed.
 * 
 * Access: Consumer (R), Provider (R), Control (R)
 * 
 * @see TLC-FI Spec §5.4
 */
struct Input {
	// META attributes (read-only)
	/// @brief Input identifier (read-only)
	std::string id;
	
	/// @brief Tick when state was last changed (read-only)
	genfi::Ticks stateticks {0};
	
	// STATE attributes (read-only)
	/// @brief Current input state (read-only)
	/// @note Range: -32768 to 32767
	int state {0};
	
	/// @brief Input fault state (read-only)
	InputFaultState faultstate {InputFaultState::None};
	
	/// @brief Software input commando (SWICO) state (read-only)
	SwicoState swico {SwicoState::NoSwico};
};

/**
 * @brief An output object
 * 
 * This object describes a non-signal group output signal.
 * The stateticks attribute defines the tick of the TLC Facilities when the state
 * attribute within the STATE scope was last changed.
 * This object is refreshed by the ITS-A writing reqState (non-exclusive outputs).
 * 
 * Access: Consumer (R), Provider (R/W), Control (R/W)
 * 
 * @note An ITS-CLA can only write reqState for exclusive outputs when
 *       ControlApplication.controlState = StartControl, InControl or EndControl.
 *       Writing reqState (for an exclusive output) during any other
 *       ControlApplication.controlState will result in an error.
 * 
 * @see TLC-FI Spec §5.6
 */
struct Output {
	// META attributes (read-only)
	/// @brief Output identifier (read-only)
	std::string id;
	
	/// @brief Intersection identifier this output belongs to (read-only)
	std::string intersection;
	
	/// @brief Tick when state was last changed (read-only)
	genfi::Ticks stateticks {0};
	
	// STATE attributes
	/// @brief Requested output state (write-only)
	/// @note Range: -32768 to 32767, or null (uses default TLC configured value)
	/// @note For exclusive outputs: can only be written when ControlApplication.controlState
	///       is StartControl, InControl, or EndControl
	std::optional<int> reqState;
	
	/// @brief Current output state (read-only)
	/// @note Range: -32768 to 32767, or null
	std::optional<int> state;
	
	/// @brief Output fault state (read-only)
	OutputFaultState faultstate {OutputFaultState::None};
};

/**
 * @brief A signal group object
 * 
 * This object describes a signal group.
 * The stateticks attribute defines the tick of the TLC Facilities when the state
 * attribute within the STATE scope was last changed.
 * reqPrediction, predictions: ordered ascending in time.
 * 
 * Access: Consumer (R), Provider (R), Control (R/W)
 * 
 * @note An ITS-CLA can only write reqState and reqPredictions when
 *       ControlApplication.controlState = StartControl, InControl or EndControl.
 *       Writing reqState or reqPredictions during any other ControlApplication.controlState
 *       will result in an error.
 * 
 * @note Maximum 16 entries for reqPredictions and predictions arrays.
 * 
 * @see TLC-FI Spec §5.7
 */
struct SignalGroup {
	// META attributes (read-only)
	/// @brief Signal group identifier (read-only)
	std::string id;
	
	/// @brief Intersection identifier this signal group belongs to (read-only)
	std::string intersection;
	
	/// @brief Intergreen conflicts with other signal groups (read-only)
	std::vector<SignalConflict> intergreen;
	
	/// @brief Safety timing constraints for each state (read-only)
	std::vector<SignalTiming> timing;
	
	/// @brief Tick when state was last changed (read-only)
	genfi::Ticks stateticks {0};
	
	// STATE attributes
	/// @brief Current signal group state (read-only)
	SignalGroupState state {SignalGroupState::Unavailable};
	
	/// @brief Requested signal group state (write-only)
	/// @note Can only be written when ControlApplication.controlState is
	///       StartControl, InControl, or EndControl
	std::optional<SignalGroupState> reqState;

#if defined(TLCFI_VERSION_1_1_0)
	/// @brief Requested signal group predictions (write-only)
	/// @note Maximum 16 entries, ordered ascending in time
	/// @note Can only be written when ControlApplication.controlState is
	///       StartControl, InControl, or EndControl
	/// @note Removed in TLC-FI 2.0.1
	std::vector<SignalGroupPrediction> reqPredictions;

	/// @brief Current signal group predictions (read-only)
	/// @note Maximum 16 entries, ordered ascending in time
	/// @note Removed in TLC-FI 2.0.1
	std::vector<SignalGroupPrediction> predictions;
#endif // TLCFI_VERSION_1_1_0
};

/**
 * @brief A detector event object
 * 
 * This object describes an event generated by a detector. When it occurs the id is the
 * same as the origination Detector.
 * This object implements the abstract object ObjectEventContent.
 * 
 * @see TLC-FI Spec §5.3
 */
struct DetectorEvent {
	/// @brief Speed of the detected object
	genfi::Speed objectspeed {0};
	
	/// @brief Length of the detected object
	genfi::Length objectlength {0};
	
	/// @brief Height of the detected object (optional)
	std::optional<genfi::Length> objectheight;
	
	/// @brief Width of the detected object (optional)
	std::optional<genfi::Length> objectwidth;
	
	/// @brief Vehicle classification
	DetectorClassification classification {DetectorClassification::Unknown};
	
	/// @brief Driving direction
	DetectorDirection direction {DetectorDirection::Normal};
};

/**
 * @brief A special vehicle event data type
 * 
 * This object describes the contents of a special vehicle event. The contents are
 * based on the contents of a KAR message.
 * This object implements the abstract object ObjectEventContent.
 * 
 * @see TLC-FI Spec §5.8
 */
struct SpecialVehicleEvent {
	/// @brief Virtual loop identifier (optional)
	std::optional<int> virtualLoop;
	
	/// @brief Vehicle type (optional)
	/// @note VehicleType - not enumerated here, kept as int
	std::optional<int> vehType;
	
	/// @brief Public transport line number (optional)
	std::optional<LineNumber> lineNr;
	
	/// @brief Public transport service number (optional)
	std::optional<ServiceNumber> serviceNr;
	
	/// @brief Public transport company number (optional)
	std::optional<CompanyNumber> companyNr;
	
	/// @brief Vehicle identifier (optional)
	std::optional<VehicleId> vehId;
	
	/// @brief Signal group direction (optional)
	/// @note Range: 0 to 255
	std::optional<DirectionSG> directionSG;
	
	/// @brief Vehicle status (optional)
	/// @note VehicleStatus - kept as int
	std::optional<int> status;
	
	/// @brief Priority class (optional)
	std::optional<PriorityClass> priorityClass;
	
	/// @brief Punctuality class (optional)
	std::optional<PunctualityClass> punctuality;
	
	/// @brief Punctuality time in seconds (optional)
	/// @note Range: -3600 to 3600 seconds
	std::optional<PunctualityTime> punctualityTime;
	
	/// @brief Vehicle length (optional)
	std::optional<genfi::Length> length;
	
	/// @brief Vehicle speed (optional)
	std::optional<genfi::Speed> speed;
	
	/// @brief Distance to stopline in meters (optional)
	/// @note Range: -99 to 9999 meters (negative = passed stopline)
	std::optional<DistanceToStopline> distToStopLine;
	
	/// @brief Time to stopline in seconds (optional)
	/// @note Range: 0 to 255 seconds
	std::optional<int> timeToStopLine;
	
	/// @brief Public transport journey number (optional)
	std::optional<JourneyNumber> journeyNr;
	
	/// @brief Public transport journey category (optional)
	std::optional<JourneyCategory> journeyCat;
	
	/// @brief Public transport route (optional)
	std::optional<RoutePublicTransport> routePT;
	
	/// @brief Announcement type (optional)
	std::optional<AnnouncementType> type;
	
	/// @brief Activation point number (optional)
	/// @note Range: 0 to 32767
	std::optional<ActivationPointNr> activationPointNr;
	
	/// @brief Location information (optional)
	std::optional<genfi::Location> location;
	
	/// @brief Date and time (optional)
	std::optional<DateTime> dateTime;
	
	/// @brief Reserved field 23 (optional)
	std::optional<int> reserve23;
	
	/// @brief Reserved field 24 (optional)
	std::optional<int> reserve24;
};

} // namespace tlcfi::tlcfi
