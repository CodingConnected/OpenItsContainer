#pragma once

#include "../jsonrpc/JsonRpcSession.hpp"
#include "../models/gen-fi/GenFiTypes.hpp"
#include "../models/gen-fi/Enums.hpp"
#include "../models/gen-fi/Structures.hpp"
#include "../models/gen-fi/Json.hpp"
#include "../models/tlc-fi/Enums.hpp"
#include "../models/tlc-fi/Objects.hpp"
#include "../models/tlc-fi/Json.hpp"
#include "../models/tlc-fi/Objects.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <mutex>

namespace tlcfi::fi {

/**
 * @brief High-level Facilities Interface session wrapper
 * 
 * This class provides a convenient C++ interface for communicating with TLC-FI and RIS-FI
 * Facilities over JSON-RPC. It wraps the low-level JsonRpcSession and implements the
 * GEN-FI protocol methods (Register, Deregister, Alive) as well as TLC-FI-specific
 * control operations.
 * 
 * The class manages session state (session ID and facilities reference) obtained during
 * registration, and provides helper methods for common TLC-FI control operations such as
 * updating ControlApplication state, Intersection state, SignalGroup states, and Output states.
 * 
 * All methods that communicate with the Facilities return std::future objects to support
 * asynchronous operation. The underlying JSON-RPC communication is handled by JsonRpcSession.
 * 
 * @note This class is thread-safe for concurrent method calls, but individual futures
 *       returned by methods should be accessed from a single thread.
 * 
 * @see GEN-FI Spec §7 (Methods), §8 (Functional use-cases)
 * @see TLC-FI Spec §4.8 (Control Application), §4.11.3 (Atomic updates)
 */
class FiSession {
public:
	/**
	 * @brief Constructs a FiSession with the given JSON-RPC session.
	 * 
	 * The session must be started with start() before use. The JsonRpcSession handles
	 * the underlying transport (TCP/TLS) and JSON-RPC protocol details.
	 * 
	 * @param rpc Shared pointer to the JsonRpcSession instance to use for communication.
	 */
	explicit FiSession(std::shared_ptr<jsonrpc::JsonRpcSession> rpc);

	/**
	 * @brief Starts the session and registers the Alive handler.
	 * 
	 * Registers a handler for Facilities-initiated Alive calls (per GEN-FI §7.3).
	 * The handler echoes the received AliveObject back to the Facilities, as required
	 * by the protocol. After registration, starts the underlying RPC session.
	 * 
	 * @note Must be called before any other methods. The Alive handler ensures bidirectional
	 *       alive checking works correctly.
	 * 
	 * @see GEN-FI Spec §5.6 (Alive checking), §7.3 (Alive method)
	 */
	void start();

	/**
	 * @brief Stops the session and closes the connection.
	 * 
	 * Stops the underlying RPC session, which will close the transport connection.
	 * Any pending operations may be cancelled.
	 */
	void stop();

	/**
	 * @brief Registers an ITS Application with the Facilities.
	 * 
	 * Sends a Register request (GEN-FI §7.1) to authenticate and authorize the application.
	 * Upon successful registration, stores the session ID and facilities reference for
	 * use in subsequent operations.
	 * 
	 * The registration process transitions the application session state from Disconnected
	 * to Connected. The Facilities validates credentials, application type, and protocol
	 * version according to the decision tables in GEN-FI §5.5.
	 * 
	 * @param req Registration request containing username, password, application type,
	 *            protocol version, and URI.
	 * @param timeout Maximum time to wait for the registration response.
	 * @return Future that will contain the RegistrationReply with session ID and facilities
	 *         reference. The session state is automatically updated upon successful registration.
	 * 
	 * @throws std::runtime_error if registration fails (check the future for details).
	 * 
	 * @see GEN-FI Spec §7.1 (Register), §8.1 (Establish connection use-case)
	 */
	std::future<genfi::RegistrationReply> registerApp(const genfi::RegistrationRequest& req,
		std::chrono::milliseconds timeout);

	/**
	 * @brief Deregisters the application from the Facilities.
	 * 
	 * Sends a Deregister request (GEN-FI §7.2) to properly close the session.
	 * The Facilities will transition the session state to Disconnected and remove
	 * all subscriptions associated with this session.
	 * 
	 * @param timeout Maximum time to wait for the deregistration response.
	 * @return Future that completes when deregistration is acknowledged.
	 * 
	 * @see GEN-FI Spec §7.2 (Deregister), §8.2 (Break connection use-case)
	 */
	std::future<void> deregisterApp(std::chrono::milliseconds timeout);

	/**
	 * @brief Sends an Alive message to the Facilities.
	 * 
	 * Sends an Alive request (GEN-FI §7.3) to maintain connection health. Both the
	 * ITS Application and Facilities must continuously send Alive messages with the
	 * required interval (2s for ITS-CLA, 10s for other applications per GEN-FI §5.7).
	 * 
	 * The Alive message contains the current time-tick and UTC timestamp. The Facilities
	 * echoes the AliveObject back as the response. If either party fails to receive an
	 * Alive message within 2.5 * interval, the connection is considered broken.
	 * 
	 * @param a AliveObject containing current ticks and timestamp.
	 * @param timeout Maximum time to wait for the Alive response.
	 * @return Future that will contain the echoed AliveObject from the Facilities.
	 * 
	 * @see GEN-FI Spec §5.6 (Alive checking), §5.7 (Timing), §7.3 (Alive method)
	 */
	std::future<genfi::AliveObject> alive(const genfi::AliveObject& a, std::chrono::milliseconds timeout);

	/**
	 * @brief Reads meta data for the specified objects.
	 * 
	 * Sends a ReadMeta request to retrieve the META attributes of one or more objects.
	 * Meta data contains constant information that doesn't change during the object's
	 * lifetime (e.g., object IDs, relationships, configuration).
	 * 
	 * The request parameters are flattened (type and ids) as required by the TLC-FI
	 * protocol, rather than using a nested ObjectReference structure.
	 * 
	 * @param objects ObjectReference specifying the object type and IDs to read.
	 * @param timeout Maximum time to wait for the response.
	 * @return Future that will contain ObjectMeta with the requested meta data.
	 * 
	 * @see GEN-FI Spec §6.2 (ObjectMeta), TLC-FI Spec §6.4 (ReadMeta)
	 */
	std::future<genfi::ObjectMeta> readMeta(const genfi::ObjectReference& objects,
	                                       std::chrono::milliseconds timeout);

	/**
	 * @brief Reads meta data using the facilities reference from registration.
	 * 
	 * Convenience method that uses the facilities reference stored during registration.
	 * This is useful for reading the Facilities object meta data without needing to
	 * construct the ObjectReference manually.
	 * 
	 * @param timeout Maximum time to wait for the response.
	 * @return Future that will contain ObjectMeta with the facilities meta data.
	 * @throws std::runtime_error if called before registration completes.
	 * 
	 * @see readMeta(const genfi::ObjectReference&, std::chrono::milliseconds)
	 */
	std::future<genfi::ObjectMeta> readMeta(std::chrono::milliseconds timeout);

	/**
	 * @brief Subscribes to state updates for the specified objects.
	 * 
	 * Sends a Subscribe request to receive notifications when the subscribed objects
	 * change state. The subscription returns an initial ObjectData snapshot containing
	 * the current state of all subscribed objects.
	 * 
	 * After subscription, the Facilities will send ObjectStateUpdate notifications
	 * whenever the subscribed objects change. These notifications are delivered through
	 * the events() callback mechanism.
	 * 
	 * Subscriptions are removed when the session is disconnected or an Alive error occurs.
	 * The application must re-subscribe after reconnecting.
	 * 
	 * @param objects ObjectReference specifying the object type and IDs to subscribe to.
	 * @param timeout Maximum time to wait for the initial snapshot.
	 * @return Future that will contain ObjectData with the initial state snapshot.
	 * 
	 * @see GEN-FI Spec §5.4.3 (Data subscriptions and notifications)
	 */
	std::future<genfi::ObjectData> subscribe(const genfi::ObjectReference& objects,
	                                        std::chrono::milliseconds timeout);

	/**
	 * @brief Sends an UpdateState notification to update object states.
	 * 
	 * Sends an UpdateState notification (not a request) containing state updates for
	 * one or more objects. As a client, the ITS Application sends notifications rather
	 * than requests per the GEN-FI specification.
	 * 
	 * The ObjectStateUpdateGroup can contain multiple ObjectStateUpdate entries, allowing
	 * atomic updates of multiple objects. All updates in a group are processed atomically
	 * by the Facilities (per TLC-FI §4.11.3).
	 * 
	 * @param group ObjectStateUpdateGroup containing the state updates to send.
	 * @return true when the notification was queued, false when the transport
	 *         disconnected before it could be queued.
	 * 
	 * @see GEN-FI Spec §6.2 (ObjectStateUpdate), TLC-FI Spec §4.11.3 (Atomic updates)
	 */
	bool updateState(const genfi::ObjectStateUpdateGroup& group) noexcept;

	/**
	 * @brief Sets ControlApplication state for this session.
	 * 
	 * Updates the ControlApplication object (Session type) for this session with new
	 * requested state values. Only writable STATE attributes are included (META attributes
	 * like sessionid and type are read-only and excluded).
	 * 
	 * This method is used to:
	 * - Request control of an intersection (reqIntersection, reqControlState)
	 * - Set handover capabilities (startCapability, endCapability)
	 * - Transition control states (reqControlState)
	 * 
	 * The update is sent as an UpdateState notification targeting the Session object
	 * type with this session's ID.
	 * 
	 * @param state Requested control state (e.g., ReadyToControl, InControl, EndControl).
	 * @param intersectionId Intersection ID to request control of (empty string or null
	 *                      to clear the request).
	 * @param startCap Optional handover capability when starting control.
	 * @param endCap Optional handover capability when ending control.
	 * 
	 * @throws std::runtime_error if called before registration completes.
	 * 
	 * @see TLC-FI Spec §5.2 (ControlApplication), §4.8 (Control Application)
	 */
	void control_setReqState(::tlcfi::tlcfi::ControlState state,
	                         const std::string& intersectionId,
	                        std::optional<::tlcfi::tlcfi::HandoverCapability> startCap = std::nullopt,
	                        std::optional<::tlcfi::tlcfi::HandoverCapability> endCap = std::nullopt);

	/**
	 * @brief Requests a change to an Intersection control state.
	 * 
	 * Updates the Intersection.reqState attribute to request a new intersection control
	 * state (e.g., Control, Standby, AllRed). The ITS-CLA can only write reqState when
	 * ControlApplication.controlState is StartControl, InControl, or EndControl.
	 * 
	 * @param intersectionId The intersection identifier.
	 * @param state The requested intersection control state.
	 * 
	 * @see TLC-FI Spec §5.5 (Intersections), §4.8.2 (Control state logic)
	 */
	void intersection_setReqState(const std::string& intersectionId,
	                             ::tlcfi::tlcfi::IntersectionControlState state);

	/**
	 * @brief Sets requested states for multiple signal groups atomically.
	 * 
	 * Updates SignalGroup.reqState for multiple signal groups in a single atomic update.
	 * This ensures that all signal group state changes are processed together, which is
	 * important for maintaining functional consistency (per TLC-FI §4.11.3).
	 * 
	 * The ITS-CLA can only write reqState when ControlApplication.controlState is
	 * StartControl, InControl, or EndControl.
	 * 
	 * @param updates Vector of (signal group ID, requested state) pairs.
	 * 
	 * @see TLC-FI Spec §5.7 (Signal groups), §4.11.3 (Atomic updates)
	 */
	void signalgroups_setReqStates(const std::vector<std::pair<std::string, ::tlcfi::tlcfi::SignalGroupState>>& updates);

	/**
	 * @brief Sets requested states for multiple outputs atomically.
	 * 
	 * Updates Output.reqState for multiple outputs in a single atomic update. The state
	 * value is an implementation-specific integer (-32768 to 32767, or null for default).
	 * 
	 * For exclusive outputs, the ITS-CLA can only write reqState when
	 * ControlApplication.controlState is StartControl, InControl, or EndControl.
	 * Non-exclusive outputs can be controlled by any Provider Application.
	 * 
	 * @param updates Vector of (output ID, requested state) pairs.
	 * 
	 * @see TLC-FI Spec §5.6 (Outputs), §4.4 (Outputs), §4.11.3 (Atomic updates)
	 */
	void outputs_setReqStates(const std::vector<std::pair<std::string, int>>& updates);

	/**
	 * @brief Atomically acknowledges StartControl and requests Intersection Control state.
	 * 
	 * This is a convenience method that performs two related operations atomically:
	 * 1. Sets ControlApplication.reqControlState = InControl (acknowledging StartControl)
	 * 2. Sets Intersection.reqState = Control (requesting normal operation)
	 * 
	 * This pattern is commonly used when the Facilities requests the ITS-CLA to take
	 * control (StartControl state), and the ITS-CLA responds by acknowledging and
	 * requesting the intersection to enter Control state.
	 * 
	 * @param intersectionId The intersection identifier.
	 * 
	 * @see TLC-FI Spec §4.8.2 (Control state logic), Table 5 (StartControl logic)
	 */
	void control_ackStartAndSetIntersectionControl(const std::string& intersectionId);

	/**
	 * @brief Returns the events interface for receiving notifications.
	 * 
	 * Provides access to the JsonRpcEvents callbacks for handling incoming notifications
	 * from the Facilities, such as ObjectStateUpdate notifications for subscribed objects
	 * and SessionEvent notifications.
	 * 
	 * @return Reference to the JsonRpcEvents object with callbacks for notifications.
	 */
	jsonrpc::JsonRpcEvents& events();

private:
	std::shared_ptr<jsonrpc::JsonRpcSession> rpc_;  ///< Underlying JSON-RPC session for communication.
	std::mutex stateMutex_;                          ///< Mutex protecting session state (sessionId_, facilitiesRef_).
	std::optional<genfi::ObjectReference> facilitiesRef_;  ///< Facilities reference from RegistrationReply (stored for convenience).
	std::optional<std::string> sessionId_;           ///< Session identifier from RegistrationReply (used for Session object updates).
};

} // namespace tlcfi::fi


