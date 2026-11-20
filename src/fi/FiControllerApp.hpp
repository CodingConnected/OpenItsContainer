#pragma once

#include "fi/FiConsumerApp.hpp"
#include "models/tlc-fi/Enums.hpp"
#include "models/tlc-fi/Objects.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace tlcfi::fi {

/**
 * @brief ITS Control Application (ITS-CLA) implementation
 * 
 * This class extends FiConsumerApp to implement a complete ITS Control Application
 * that manages the control state machine and provides control over intersections,
 * signal groups, and outputs.
 * 
 * The class handles control-specific functionality:
 * - Session subscription for monitoring control state (TLC-FI §4.8.2)
 * - Control state machine transitions (NotConfigured → Offline → ReadyToControl → InControl)
 * - Automatic meta read and subscription when entering NotConfigured state
 * - Automatic ReadyToControl request when entering Offline state
 * - Control request methods for signal groups and outputs
 * 
 * The implementation automatically handles control state transitions:
 * - When controlState = NotConfigured: triggers meta read and subscription
 * - When controlState = Offline: automatically requests ReadyToControl
 * - When controlState = StartControl: notifies via callback and acknowledges with InControl
 * 
 * @note This class is thread-safe for concurrent method calls. Callbacks are invoked
 *       from the I/O thread, so external handlers should be thread-safe.
 * 
 * @see FiConsumerApp (base class with connection and subscription management)
 * @see TLC-FI Spec §4.8 (Control Application), §4.8.2 (Control state logic)
 */
class FiControllerApp : public FiConsumerApp {
public:
	/**
	 * @brief Constructs a controller application for the given Facilities endpoint.
	 *
	 * @param host Facilities hostname or IP address.
	 * @param port Facilities TCP port.
	 * @param username Application username for registration (mandatory).
	 * @param password Application password for registration (mandatory).
	 * @param uri Application URI identifier for registration (mandatory).
	 * @param startHandoverCapability Handover capability when starting control (default: Cleared).
	 * @param endHandoverCapability Handover capability when ending control (default: Cleared).
	 */
	FiControllerApp(const std::string& host, const std::string& port,
	                const std::string& username, const std::string& password,
	                const std::string& uri,
	                ::tlcfi::tlcfi::HandoverCapability startHandoverCapability = ::tlcfi::tlcfi::HandoverCapability::Cleared,
	                ::tlcfi::tlcfi::HandoverCapability endHandoverCapability = ::tlcfi::tlcfi::HandoverCapability::Cleared);

	/**
	 * @brief Constructs a controller application with explicit TLS settings.
	 *
	 * @param host Facilities hostname or IP address.
	 * @param port Facilities TCP port.
	 * @param username Application username for registration (mandatory).
	 * @param password Application password for registration (mandatory).
	 * @param uri Application URI identifier for registration (mandatory).
	 * @param tlsConfig TLS client configuration.
	 * @param startHandoverCapability Handover capability when starting control.
	 * @param endHandoverCapability Handover capability when ending control.
	 */
	FiControllerApp(const std::string& host, const std::string& port,
	                const std::string& username, const std::string& password,
	                const std::string& uri, const tcp::TlsConfig& tlsConfig,
	                ::tlcfi::tlcfi::HandoverCapability startHandoverCapability = ::tlcfi::tlcfi::HandoverCapability::Cleared,
	                ::tlcfi::tlcfi::HandoverCapability endHandoverCapability = ::tlcfi::tlcfi::HandoverCapability::Cleared);
	
	/**
	 * @brief Destructor that stops the application and cleans up resources.
	 */
	~FiControllerApp() override;

	/**
	 * @brief Override cleanup to reset control-specific flags.
	 *
	 * Ensures that control state flags are reset when the app is cleaned up,
	 * allowing the app to be reused or reconnected.
	 */
	void cleanup() override;

	/**
	 * @brief Controller handles meta/subscribe via its own state machine.
	 *
	 * Returns true so FiAppHost skips readMeta()/subscribeAll() after registration.
	 * The controller triggers those when controlState=NotConfigured is received.
	 */
	bool handlesInitInternally() const override;

	/**
	 * @brief Registers the application with the Facilities and subscribes to Session.
	 *
	 * Overrides FiConsumerApp::registerApp() to add Session subscription after
	 * successful registration. The Session subscription is needed to monitor control
	 * state transitions (e.g., Offline → ReadyToControl → StartControl → InControl).
	 *
	 * @param timeout Maximum time to wait for registration response.
	 * @return true if registration and Session subscription successful, false on failure.
	 *
	 * @see TLC-FI Spec §4.8.2 (Control state logic)
	 */
	bool registerApp(std::chrono::milliseconds timeout) override;

	/**
	 * @brief Subscribes to intersection and captures initial state.
	 *
	 * Overrides FiConsumerApp::subscribeIntersection() to capture the initial
	 * intersection control state from the subscription snapshot. This is needed
	 * to know the intersection state before StartControl is received.
	 *
	 * @param timeout Maximum time to wait for subscription response.
	 * @return true if subscription successful, false on failure.
	 */
	bool subscribeIntersection(std::chrono::milliseconds timeout) override;

	/**
	 * @brief Control interface methods
	 * 
	 * These methods allow external logic to request state changes for signal groups
	 * and outputs. The requests are sent atomically to the Facilities via UpdateState
	 * notifications. Local state is updated optimistically before sending.
	 */
	
	/**
	 * @brief Requests state changes for multiple signal groups atomically.
	 * 
	 * Updates SignalGroup.reqState for the specified signal groups. The update is
	 * sent as an atomic UpdateState notification (per TLC-FI §4.11.3). Local state
	 * is updated before sending to maintain consistency.
	 * 
	 * @param updates Vector of (signal group ID, requested state) pairs.
	 * 
	 * @see TLC-FI Spec §5.7 (Signal groups), §4.11.3 (Atomic updates)
	 */
	void requestSignalGroupStates(const std::vector<std::pair<std::string, ::tlcfi::tlcfi::SignalGroupState>>& updates);
	
	/**
	 * @brief Requests state changes for multiple outputs atomically.
	 * 
	 * Updates Output.reqState for the specified outputs. The update is sent as an
	 * atomic UpdateState notification. Local state is updated before sending.
	 * 
	 * @param updates Vector of (output ID, requested state) pairs.
	 * 
	 * @see TLC-FI Spec §5.6 (Outputs), §4.11.3 (Atomic updates)
	 */
	void requestOutputStates(const std::vector<std::pair<std::string, int>>& updates);
	
	/**
	 * @brief Control state transition hooks
	 * 
	 * These callbacks are invoked when the ControlApplication controlState transitions
	 * to specific states, allowing external logic to react to control state changes.
	 */
	
	/// @brief Invoked when controlState transitions to InControl.
	/// @param intersectionId The intersection identifier for which control was granted.
	std::function<void(const std::string& intersectionId)> onControlInControl;
	
	/// @brief Invoked when controlState transitions to EndControl.
	/// @param intersectionId The intersection identifier for which control is ending.
	std::function<void(const std::string& intersectionId)> onControlEndControl;

	/// @brief Invoked when the connection to the Facilities is lost.
	/// @param reason Description of why the connection was lost.
	/// @note This is called before cleanup, allowing external logic (e.g., CifHost)
	///       to react to disconnection (e.g., stop control loop, reset bridge state).
	std::function<void(const std::string& reason)> onConnectionLost;

	/**
	 * @brief Handover capability configuration
	 *
	 * These methods allow configuring the handover capabilities that the
	 * ITS-CLA reports to the TLC Facilities. The TLC uses these capabilities
	 * to determine the handover method when transferring control.
	 *
	 * @see TLC-FI Spec §4.8.4 (Application handover)
	 */

	/**
	 * @brief Sets the handover capability when starting control.
	 *
	 * This capability is sent to the TLC Facilities when requesting
	 * ReadyToControl or InControl states.
	 *
	 * @param capability The handover capability (Cleared, PreDefined, or Direct).
	 */
	void setStartHandoverCapability(::tlcfi::tlcfi::HandoverCapability capability);

	/**
	 * @brief Gets the current handover capability for starting control.
	 *
	 * @return The configured start handover capability.
	 */
	::tlcfi::tlcfi::HandoverCapability getStartHandoverCapability() const;

	/**
	 * @brief Sets the handover capability when ending control.
	 *
	 * This capability is sent to the TLC Facilities when acknowledging
	 * StartControl or InControl states.
	 *
	 * @param capability The handover capability (Cleared, PreDefined, or Direct).
	 */
	void setEndHandoverCapability(::tlcfi::tlcfi::HandoverCapability capability);

	/**
	 * @brief Gets the current handover capability for ending control.
	 *
	 * @return The configured end handover capability.
	 */
	::tlcfi::tlcfi::HandoverCapability getEndHandoverCapability() const;

protected:
	/**
	 * @brief Connection event override for disconnection handling.
	 *
	 * Invokes onConnectionLost callback before calling base class handler.
	 * This allows external logic (e.g., CifHost) to react to disconnection.
	 */
	void onDisconnected(const std::string& reason) override;

	/**
	 * @brief Overrides to handle Session control flow
	 *
	 * This override processes Session updates to handle control state machine
	 * transitions and trigger appropriate actions.
	 */
	
	/**
	 * @brief Processes Session object updates for control state machine.
	 * 
	 * Overrides FiConsumerApp::processSessionUpdate() to handle control state
	 * transitions. Calls maybeTriggerMetaOnSessionNotConfigured() and
	 * processSessionControlFlow() to implement control state logic.
	 * 
	 * @param g ObjectStateUpdateGroup containing Session updates.
	 * 
	 * @see TLC-FI Spec §4.8.2 (Control state logic)
	 */
	void processSessionUpdate(const genfi::ObjectStateUpdateGroup& g) override;

private:
	/**
	 * @brief Control state machine methods
	 * 
	 * These methods implement the control state machine logic from TLC-FI §4.8.2.
	 */
	
	/**
	 * @brief Processes Session object updates to handle control state transitions.
	 * 
	 * Monitors ControlApplication.controlState changes and automatically:
	 * - When Offline: requests ReadyToControl (if not already requested)
	 * - When StartControl: invokes onControlInControl callback and acknowledges with InControl
	 * 
	 * Implements the control state logic from TLC-FI §4.8.2.
	 * 
	 * @param g ObjectStateUpdateGroup containing Session updates.
	 * 
	 * @see TLC-FI Spec §4.8.2 (Control state logic), Table 3-7
	 */
	void processSessionControlFlow(const genfi::ObjectStateUpdateGroup& g);
	
	/**
	 * @brief Handles Session controlState from UpdateState notifications.
	 *
	 * Extracts controlState from Session updates and delegates to handleControlStateTransition().
	 * Always logs the received controlState for diagnostics.
	 *
	 * @param g ObjectStateUpdateGroup containing Session updates.
	 *
	 * @see TLC-FI Spec §4.8.1 (States), Table 2 (State transitions)
	 */
	void handleSessionControlState(const genfi::ObjectStateUpdateGroup& g);

	/**
	 * @brief Processes initial Session state from subscribe snapshot.
	 *
	 * Called after subscribing to Session to process the initial controlState.
	 * This is critical because the controlState may only be sent in the subscribe
	 * snapshot and not in subsequent UpdateState notifications.
	 *
	 * @param stateJson JSON object containing the Session state.
	 */
	void processInitialSessionState(const nlohmann::json& stateJson);

	/**
	 * @brief Handles a controlState transition and triggers appropriate actions.
	 *
	 * Central handler for all controlState values. Decides what action to take
	 * based on the current state:
	 * - NotConfigured: trigger meta/subscribe
	 * - Error: log warning, attempt meta/subscribe
	 * - Offline/ReadyToControl: ensure meta/subscribe is done
	 * - StartControl/InControl: unexpected at startup, handle gracefully
	 * - EndControl: notify callback
	 *
	 * @param cs The controlState value (from ControlState enum).
	 * @param stateJson JSON object containing the full Session state.
	 */
	void handleControlStateTransition(int cs, const nlohmann::json& stateJson);

	/**
	 * @brief Converts a controlState integer to a human-readable string.
	 *
	 * @param cs The controlState value.
	 * @return Human-readable state name (e.g., "NotConfigured", "Offline").
	 */
	static std::string controlStateToString(int cs);
	
	/**
	 * @brief Launches meta read and subscription in a background thread.
	 * 
	 * Reads meta data, then subscribes to all discovered objects. This is
	 * called automatically when entering NotConfigured state.
	 */
	void launchMetaAndSubscribe();
	
	/**
	 * @brief Sends a request to set ControlApplication.reqControlState = Offline.
	 * 
	 * Used to transition the control state to Offline, typically when ending control.
	 */
	void sendSessionReqControlStateOffline();
	
	/**
	 * @brief Update sending helpers
	 * 
	 * These methods send UpdateState notifications with only the changed fields,
	 * constructing minimal JSON objects per TLC-FI requirements.
	 */
	
	/// @brief Sends UpdateState notification for signal group state changes.
	void sendSignalGroupUpdates(const std::vector<std::pair<std::string, ::tlcfi::tlcfi::SignalGroupState>>& updates);
	
	/// @brief Sends UpdateState notification for output state changes.
	void sendOutputUpdates(const std::vector<std::pair<std::string, int>>& updates);

	/**
	 * @brief Requests the intersection to transition to Control state.
	 *
	 * Sends an UpdateState with Intersection.reqState = Control.
	 * Called when we receive StartControl but intersection is not yet in Control.
	 *
	 * @param intersectionId The intersection to request control for.
	 */
	void requestIntersectionControlState(const std::string& intersectionId);

	/**
	 * @brief Completes the InControl transition after intersection is ready.
	 *
	 * Called when intersection reaches Control state. Sends InControl to TLC
	 * and invokes onControlInControl callback.
	 */
	void completeInControlTransition();

	/**
	 * @brief Converts IntersectionControlState to human-readable string.
	 */
	static std::string intersectionStateToString(int state);

	/**
	 * @brief Checks if the intersection is in a stable state suitable for reading meta.
	 *
	 * The TLC may still be booting when the controlState transitions to NotConfigured.
	 * We should wait until the intersection is in a stable operational state
	 * (Control, AllRed, Standby, Dark, or AlternativeStandby) before reading meta.
	 *
	 * Transient/error states (SwitchOn, SwitchOff, Error) indicate the TLC is not
	 * fully configured yet, and reading meta may return incomplete data.
	 *
	 * @return true if the intersection is in a stable state for meta read.
	 */
	bool isIntersectionReadyForMeta() const;

protected:
	/**
	 * @brief Override processObjectUpdates to handle intersection state changes.
	 */
	void processObjectUpdates(const genfi::ObjectStateUpdateGroup& g) override;

	/**
	 * @brief Reacts to Session failure events (TLC-FI §7.7/§7.9 exceptions).
	 *
	 * On a failure code (>= 1000, e.g. UpdateStateFailedIncorrectControlState/
	 * IncorrectIntersection) the Facilities sets controlState=Error and closes the
	 * connection. Resets the control-flow guards so the control sequence re-arms
	 * cleanly after FiAppHost reconnects. Lifecycle codes are handled by the base.
	 */
	void handleSessionEvent(const genfi::SessionEvent& ev) override;

private:
	/**
	 * @brief Control-specific state flags
	 *
	 * Flags to prevent duplicate control state transitions.
	 */
	std::atomic<bool> readyToControlRequested_{false};                            ///< Flag to prevent duplicate ReadyToControl requests.
	std::atomic<bool> inControlRequested_{false};                                  ///< Flag to prevent duplicate InControl acknowledgments.
	std::atomic<bool> metaStarted_{false};                                         ///< Flag to prevent duplicate meta read/subscription.
	std::atomic<bool> sawValidControlState_{false};                              ///< Flag indicating we've seen a valid control state (not Unknown/-1).
	std::atomic<bool> waitingForIntersectionControl_{false};                      ///< Flag indicating we're waiting for intersection to reach Control.
	std::atomic<bool> intersectionSubscribed_{false};                              ///< Flag to prevent duplicate intersection subscriptions.
	std::atomic<int> intersectionControlState_{-1};                                ///< Current intersection control state (-1 = unknown).
	std::string pendingControlIntersectionId_;                                     ///< Intersection ID we're waiting to reach Control state.
	std::mutex controlMutex_;                                                       ///< Mutex for control state variables.
	std::thread metaSubscribeThread_;                                              ///< Worker running launchMetaAndSubscribe(); joined in cleanup()/destructor.

	/**
	 * @brief Handover capabilities
	 *
	 * These capabilities are reported to the TLC Facilities to indicate
	 * which handover methods this ITS-CLA supports.
	 *
	 * @see TLC-FI Spec §4.8.4 (Application handover)
	 */
	::tlcfi::tlcfi::HandoverCapability startHandoverCapability_;                    ///< Handover capability when starting control.
	::tlcfi::tlcfi::HandoverCapability endHandoverCapability_;                      ///< Handover capability when ending control.
};

} // namespace tlcfi::fi
