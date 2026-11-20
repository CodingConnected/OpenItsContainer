#pragma once

#include "tcp/TcpClientTransport.hpp"
#include "tcp/TlsConfig.hpp"
#include "jsonrpc/BalancedJsonFramer.hpp"
#include "jsonrpc/IMessageFramer.hpp"
#include "jsonrpc/JsonRpcSession.hpp"
#include "fi/FiSession.hpp"
#include "fi/LogLevel.hpp"
#include "models/tlc-fi/Enums.hpp"
#include "models/tlc-fi/Objects.hpp"
#include "models/tlc-fi/Json.hpp"

#include <asio/io_context.hpp>
#include <asio/executor_work_guard.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tlcfi::fi {

/**
 * @brief Connection state enumeration
 */
enum class ConnectionState {
	Disconnected,
	Connecting,
	Connected,
	Registered,
	Subscribed
};

/**
 * @brief Base ITS Consumer Application for TLC-FI Facilities
 * 
 * This class implements a base ITS Application that connects to TLC-FI Facilities,
 * manages registration, meta data retrieval, subscriptions, and provides object
 * state management. It is intended as a base class for specific application types
 * (e.g., Control, Monitor).
 * 
 * The class handles:
 * - Connection establishment and registration (GEN-FI §8.1)
 * - Meta data retrieval (TLC-FI §4.8.1)
 * - Flexible object subscriptions
 * - Object state synchronization via UpdateState notifications
 * - Alive checking (GEN-FI §5.6)
 * 
 * Derived classes can override virtual methods to add application-specific logic:
 * - processSessionUpdate() - handle Session object updates
 * - processObjectUpdates() - handle other object updates
 * - onConnected(), onDisconnected(), onProtocolError() - connection events
 * 
 * @note This class is thread-safe for concurrent method calls. Callbacks are invoked
 *       from the I/O thread, so external handlers should be thread-safe.
 * 
 * @see GEN-FI Spec §8.1 (Establish connection), §5.6 (Alive checking)
 * @see TLC-FI Spec §4.8 (Applications), §4.11 (Object exchange model)
 */
class FiConsumerApp {
public:
	/**
	 * @brief Constructs a consumer application for the given Facilities endpoint.
	 * 
	 * @param host Facilities hostname or IP address.
	 * @param port Facilities TCP port.
	 * @param username Application username for registration (mandatory).
	 * @param password Application password for registration (mandatory).
	 * @param uri Application URI identifier for registration (mandatory).
	 * @param tlsConfig Optional TLS configuration. If not provided or disabled, plain TCP is used.
	 */
	FiConsumerApp(const std::string& host, const std::string& port,
	              const std::string& username, const std::string& password,
	              const std::string& uri, const tcp::TlsConfig& tlsConfig = tcp::TlsConfig{});
	
	/**
	 * @brief Virtual destructor that stops the application and cleans up resources.
	 */
	virtual ~FiConsumerApp();

	/**
	 * @brief Connection and session lifecycle methods
	 * 
	 * These methods establish the connection, register with the Facilities, retrieve
	 * meta data, and set up subscriptions. They should be called in sequence:
	 * connect() -> registerApp() -> readMeta() -> subscribe methods
	 */
	
	/**
	 * @brief Establishes TCP connection to the Facilities.
	 * 
	 * Creates the transport, framer, and RPC session, then starts the I/O thread.
	 * Waits up to timeout for the connection to be established.
	 * 
	 * @param timeout Maximum time to wait for connection.
	 * @return true if connection established, false on timeout.
	 */
	bool connect(std::chrono::milliseconds timeout);
	
	/**
	 * @brief Registers the application with the Facilities.
	 * 
	 * Sends a Register request (GEN-FI §7.1) with credentials for the given
	 * application. Upon success, starts the alive checking loop.
	 * 
	 * Derived classes can override this to customize registration (e.g., application type)
	 * and add additional subscriptions.
	 * 
	 * @param timeout Maximum time to wait for registration response.
	 * @return true if registration successful, false on failure.
	 * 
	 * @see GEN-FI Spec §7.1 (Register), §8.1 (Establish connection use-case)
	 */
	virtual bool registerApp(std::chrono::milliseconds timeout);
	
	/**
	 * @brief Reads Facilities meta data and intersection configuration.
	 * 
	 * Reads meta data for the Facilities object, then reads intersection meta data
	 * to discover signal groups, outputs, detectors, and inputs. Stores the discovered
	 * IDs for use in subscriptions. Invokes onMetaReceived callback when complete.
	 * 
	 * @param timeout Maximum time to wait for meta data responses.
	 * @return true if meta data retrieved successfully, false on failure.
	 * 
	 * @see TLC-FI Spec §4.8.1 (States), §5.5 (Intersections)
	 */
	bool readMeta(std::chrono::milliseconds timeout);
	
	/**
	 * @brief Subscription methods
	 * 
	 * These methods subscribe to specific object types for state updates. After
	 * subscription, ObjectStateUpdate notifications will be received automatically.
	 * Each method initializes local object state from the subscription snapshot.
	 */
	
	/**
	 * @brief Subscribes to signal groups for state updates.
	 * 
	 * @param timeout Maximum time to wait for subscription response.
	 * @return true if subscription successful, false on failure.
	 * 
	 * @see TLC-FI Spec §5.7 (Signal groups)
	 */
	bool subscribeSignalGroups(std::chrono::milliseconds timeout);
	
	/**
	 * @brief Subscribes to outputs for state updates.
	 * 
	 * @param timeout Maximum time to wait for subscription response.
	 * @return true if subscription successful, false on failure.
	 * 
	 * @see TLC-FI Spec §5.6 (Outputs)
	 */
	bool subscribeOutputs(std::chrono::milliseconds timeout);
	
	/**
	 * @brief Subscribes to detectors for state updates.
	 * 
	 * @param timeout Maximum time to wait for subscription response.
	 * @return true if subscription successful, false on failure.
	 * 
	 * @see TLC-FI Spec §5.8 (Detectors)
	 */
	bool subscribeDetectors(std::chrono::milliseconds timeout);
	
	/**
	 * @brief Subscribes to inputs for state updates.
	 * 
	 * @param timeout Maximum time to wait for subscription response.
	 * @return true if subscription successful, false on failure.
	 * 
	 * @see TLC-FI Spec §5.9 (Inputs)
	 */
	bool subscribeInputs(std::chrono::milliseconds timeout);
	
	/**
	 * @brief Subscribes to intersection for state updates.
	 *
	 * @param timeout Maximum time to wait for subscription response.
	 * @return true if subscription successful, false on failure.
	 *
	 * @see TLC-FI Spec §5.5 (Intersections)
	 */
	virtual bool subscribeIntersection(std::chrono::milliseconds timeout);

	/**
	 * @brief Subscribes to the special vehicle event generator for event notifications.
	 *
	 * @param timeout Maximum time to wait for subscription response.
	 * @return true if subscription successful, false on failure.
	 *
	 * @see TLC-FI Spec §5.8 (Special vehicle events)
	 */
	bool subscribeSpecialVehicleEventGenerator(std::chrono::milliseconds timeout);
	
	/**
	 * @brief Subscribes to all discovered objects for state updates.
	 * 
	 * Convenience method that calls individual subscribe methods for SignalGroups,
	 * Outputs, Detectors, Inputs, and Intersection objects discovered during readMeta().
	 * 
	 * @param timeout Maximum time to wait for each subscription response.
	 * @return true if all subscriptions successful, false on failure.
	 * 
	 * @see GEN-FI Spec §5.4.3 (Data subscriptions and notifications)
	 */
	bool subscribeAll(std::chrono::milliseconds timeout);
	
	/**
	 * @brief Blocks until a key is pressed (for demo/testing).
	 * 
	 * Platform-specific implementation that waits for user input before returning.
	 */
	void monitorUntilKeyPress();
	
	/**
	 * @brief Stops the application and cleans up resources.
	 * 
	 * Stops alive checking, deregisters from Facilities, stops the RPC session,
	 * and shuts down the I/O thread.
	 */
	virtual void stop();

	/**
	 * @brief Performs complete cleanup of connection and state.
	 * 
	 * This is an idempotent method that performs full cleanup:
	 * - Stops alive loop
	 * - Stops and joins I/O thread
	 * - Cleans up transport, RPC, and FiSession
	 * - Clears all state (session ID, meta data, object caches)
	 * - Resets all connection flags
	 * 
	 * Called automatically by connect() at the start, and by stop() during shutdown.
	 * Safe to call multiple times.
	 */
	virtual void cleanup();

	/**
	 * @brief Checks if the application is fully initialized and connected.
	 *
	 * @return true if fully initialized (connected, registered, and subscribed), false otherwise.
	 */
	bool isFullyInitialized() const { return fullyInitialized_.load(); }

	/**
	 * @brief Checks if the TCP connection is currently established.
	 *
	 * @return true if connected, false otherwise.
	 */
	bool isConnected() const { return connected_.load(); }

	/**
	 * @brief Indicates whether the application handles initialization (meta + subscribe) internally.
	 *
	 * When true, FiAppHost will skip calling readMeta() and subscribeAll() after registration,
	 * because the application's own state machine handles those steps (e.g., FiControllerApp
	 * triggers meta/subscribe when controlState=NotConfigured).
	 *
	 * @return false by default (FiConsumerApp). Override to return true in derived classes.
	 */
	virtual bool handlesInitInternally() const;

	/**
	 * @brief State access methods
	 * 
	 * These methods provide read-only access to application state and cached
	 * object state.
	 */
	
	/**
	 * @brief Gets the current state of a signal group from local cache.
	 * 
	 * @param id Signal group identifier.
	 * @return Current SignalGroupState, or Unavailable if not found.
	 */
	::tlcfi::tlcfi::SignalGroupState getSignalGroupState(const std::string& id);
	
	/**
	 * @brief Gets the session ID from registration.
	 * 
	 * @return Session ID if registered, empty optional otherwise.
	 */
	std::optional<std::string> getSessionId();
	
	/**
	 * @brief Gets the intersection ID discovered from meta.
	 * 
	 * @return Intersection ID if known, empty optional otherwise.
	 */
	std::optional<std::string> getIntersectionId();
	
	/**
	 * @brief Gets the signal group IDs discovered from meta.
	 * 
	 * @return Vector of signal group identifiers.
	 */
	std::vector<std::string> getSignalGroupIds();
	
	/**
	 * @brief Gets the output IDs discovered from meta.
	 * 
	 * @return Vector of output identifiers.
	 */
	std::vector<std::string> getOutputIds();
	
	/**
	 * @brief Gets the detector IDs discovered from meta.
	 *
	 * @return Vector of detector identifiers.
	 */
	std::vector<std::string> getDetectorIds();

	/**
	 * @brief Gets the input IDs discovered from meta.
	 *
	 * @return Vector of input identifiers.
	 */
	std::vector<std::string> getInputIds();

	/**
	 * @brief Gets the current state of a detector from local cache.
	 *
	 * @param id Detector identifier.
	 * @return Current Detector state, or empty Detector if not found.
	 */
	::tlcfi::tlcfi::Detector getDetector(const std::string& id);

	/**
	 * @brief Gets the current state of an input from local cache.
	 *
	 * @param id Input identifier.
	 * @return Current Input state, or empty Input if not found.
	 */
	::tlcfi::tlcfi::Input getInput(const std::string& id);

	/**
	 * @brief Gets the current state of an output from local cache.
	 *
	 * @param id Output identifier.
	 * @return Current Output state, or empty Output if not found.
	 */
	::tlcfi::tlcfi::Output getOutput(const std::string& id);

	/**
	 * @brief Event hooks for external logic
	 * 
	 * These callbacks are invoked when object state updates are received from the
	 * Facilities. They allow external logic to react to state changes without
	 * polling. Callbacks run without holding @ref stateMutex_, so they may safely
	 * call getters on this object.
	 */
	
	/// @brief Invoked when a SignalGroup state update is received.
	std::function<void(const ::tlcfi::tlcfi::SignalGroup&)> onSignalGroupUpdate;
	
	/// @brief Invoked when an Output state update is received.
	std::function<void(const ::tlcfi::tlcfi::Output&)> onOutputUpdate;
	
	/// @brief Invoked when a Detector state update is received.
	std::function<void(const ::tlcfi::tlcfi::Detector&)> onDetectorUpdate;
	
	/// @brief Invoked when an Input state update is received.
	std::function<void(const ::tlcfi::tlcfi::Input&)> onInputUpdate;

	/// @brief Invoked when a SpecialVehicleEvent is received.
	std::function<void(const ::tlcfi::tlcfi::SpecialVehicleEvent&)> onSpecialVehicleEvent;

	/// @brief Invoked for every Session event received via NotifyEvent (lifecycle and error).
	/// @note Delivered for all SessionEventCodes, including non-error lifecycle codes
	///       (Deregistered, FacilitiesStopping). For failure codes a decoded reason is also
	///       delivered via onSessionError.
	/// @see GEN-FI §6.5 (SessionEvent), TLC-FI §5.2.6 (SessionEventCode)
	std::function<void(const genfi::SessionEvent&)> onSessionEvent;

	/// @brief Invoked when the Facilities reports a failure SessionEvent (code >= 1000).
	/// @param code The SessionEventCode (e.g. UpdateStateFailedIncorrectControlState = 1000).
	/// @param reason Human-readable decoding of the code (and any SessionEventInformation).
	/// @note Lifecycle codes (Deregistered, FacilitiesStopping) are NOT errors and do not
	///       invoke this callback; observe them via onSessionEvent instead.
	std::function<void(int code, const std::string& reason)> onSessionError;

	/// @brief Converts a SessionEventCode to a human-readable string (generic + TLC-FI codes).
	static std::string sessionEventCodeToString(int code);

	/**
	 * @brief Meta data received hook
	 * 
	 * Invoked when meta data has been read and object IDs have been discovered.
	 * Provides the discovered IDs for signal groups, outputs, detectors, and inputs.
	 * 
	 * @param signalGroupIds Discovered signal group identifiers.
	 * @param outputIds Discovered output identifiers.
	 * @param detectorIds Discovered detector identifiers.
	 * @param inputIds Discovered input identifiers.
	 */
	std::function<void(const std::vector<std::string>&, const std::vector<std::string>&, 
	                   const std::vector<std::string>&, const std::vector<std::string>&)> onMetaReceived;

	/**
	 * @brief Logging callback function type.
	 * 
	 * Applications can set this callback to customize logging behavior.
	 * The callback receives the log level and message, and can decide
	 * what to do with it (print, filter, route to file, forward to host, etc.).
	 * 
	 * @param level Log level (Trace, Debug, Info, Error, Fatal)
	 * @param message Log message
	 */
	using LogCallback = std::function<void(LogLevel level, const std::string& message)>;

	/// @brief Logging callback - invoked for all log messages
	LogCallback onLog;

	/// @brief Access to the JsonRpcSession for tracing/monitoring JSON-RPC communication.
	std::shared_ptr<jsonrpc::JsonRpcSession> getJsonRpcSession() { return rpc_; }

	/// @brief Connection established event callback - invoked when TCP connection is established.
	/// This can be used to set up trace logging or other post-connection initialization.
	std::function<void()> onConnectedEvent;

protected:
	/**
	 * @brief Logs a message using the configured logging callback.
	 * 
	 * If a logging callback is set (e.g., by FiAppHost), it will be used.
	 * Otherwise, defaults to printing to stdout/stderr based on log level.
	 * 
	 * @param level Log level
	 * @param message Log message
	 */
	void log(LogLevel level, const std::string& message);

	/**
	 * @brief Logs a message using the configured logging callback (convenience overload).
	 * 
	 * @param level Log level
	 * @param message Log message (const char*)
	 */
	void log(LogLevel level, const char* message);

protected:
	/**
	 * @brief Protected virtual methods for derived classes to override
	 * 
	 * These methods allow derived classes to customize behavior and add
	 * application-specific logic.
	 */
	
	/**
	 * @brief Processes Session object updates.
	 * 
	 * Called by onNotification() when UpdateState notifications are received.
	 * Base implementation is empty. Derived classes (e.g., FiControllerApp) can
	 * override this to handle Session control flow.
	 * 
	 * @param g ObjectStateUpdateGroup containing updates.
	 */
	virtual void processSessionUpdate(const genfi::ObjectStateUpdateGroup& g);
	
	/**
	 * @brief Processes non-Session object updates.
	 *
	 * Called by onNotification() after processSessionUpdate(). Handles
	 * SignalGroup, Output, Detector, and Input updates by updating local
	 * state and invoking callbacks.
	 *
	 * Derived classes can override to add custom processing.
	 *
	 * @param g ObjectStateUpdateGroup containing updates.
	 */
	virtual void processObjectUpdates(const genfi::ObjectStateUpdateGroup& g);

	/**
	 * @brief Processes event notifications.
	 *
	 * Called by onNotification() when NotifyEvent notifications are received.
	 * Handles special vehicle events and other event-based notifications.
	 *
	 * Derived classes can override to add custom processing.
	 *
	 * @param g NotifyEventGroup containing event notifications.
	 */
	virtual void processEventUpdates(const genfi::NotifyEventGroup& g);

	/**
	 * @brief Reacts to a Session event after the public callbacks have fired.
	 *
	 * Called by processEventUpdates() for each Session SessionEvent (after
	 * onSessionEvent/onSessionError). Base implementation is a no-op. Derived
	 * classes (e.g. FiControllerApp) override this to apply state side-effects
	 * such as resetting control-flow guards on a failure code.
	 *
	 * @param ev The parsed Session event.
	 */
	virtual void handleSessionEvent(const genfi::SessionEvent& ev);

	/**
	 * @brief Connection event handlers
	 * 
	 * These virtual methods are called when connection events occur.
	 * Derived classes can override to add custom handling.
	 */
	
	/// @brief Called when TCP connection is established.
	virtual void onConnected();
	
	/// @brief Called when connection is closed or lost.
	/// @param reason Description of why the connection was closed.
	virtual void onDisconnected(const std::string& reason);
	
	/// @brief Called when a protocol-level error occurs.
	/// @param reason Description of the protocol error.
	virtual void onProtocolError(const std::string& reason);
	
	/**
	 * @brief Protected member access for derived classes
	 */
	
	/// @brief Access to the FiSession for derived classes to send requests.
	std::unique_ptr<FiSession>& getFiSession() { return fi_; }

	/// @brief Access to the state mutex for derived classes.
	std::mutex& getStateMutex() { return stateMutex_; }

	/// @brief Set the intersection ID (for derived classes that discover it independently).
	/// @note Caller must hold getStateMutex().
	void setIntersectionId(const std::string& id) { intersectionId_ = id; }

	/**
	 * @brief Alive checking management
	 * 
	 * These methods manage the alive checking loop that sends Alive messages
	 * to the Facilities at the required interval (2s for ITS-CLA per GEN-FI §5.7).
	 */
	
	/// @brief Starts the alive checking loop in a background thread.
	/// @see GEN-FI Spec §5.6 (Alive checking), §5.7 (Timing)
	void startAliveLoop();

	/**
	 * @brief Common registration helper
	 * 
	 * Performs the actual registration with the Facilities using the specified
	 * application type. Stores the session ID on success.
	 * 
	 * @param type Application type (Consumer, Provider, or Control).
	 * @param timeout Maximum time to wait for registration response.
	 * @return true if registration successful, false on failure.
	 */
	bool doRegistration(genfi::ApplicationType type, std::chrono::milliseconds timeout);

private:
	/**
	 * @brief RPC event handlers
	 * 
	 * These methods handle events from the underlying JsonRpcSession.
	 */
	
	/**
	 * @brief Processes incoming notifications from the Facilities.
	 *
	 * Handles UpdateState notifications by parsing the update group and
	 * dispatching to processSessionUpdate() and processObjectUpdates().
	 * Handles NotifyEvent notifications by parsing the event group and
	 * dispatching to processEventUpdates().
	 *
	 * @param method Notification method name ("UpdateState" or "NotifyEvent").
	 * @param params Notification parameters (ObjectStateUpdateGroup or NotifyEventGroup).
	 *
	 * @see GEN-FI Spec §5.4.3 (Data subscriptions and notifications)
	 * @see TLC-FI Spec §5.4.4 (Event notifications)
	 */
	void onNotification(const std::string& method, nlohmann::json params);
	
	
	/// @brief Stops the alive checking loop and waits for thread completion.
	void stopAliveLoop();

	/**
	 * @brief Object state management helpers
	 * 
	 * These methods update local object state from JSON state updates received
	 * via ObjectStateUpdate notifications. They apply partial updates to existing
	 * object instances, maintaining local state consistency.
	 */
	
	/// @brief Updates a SignalGroup instance from a JSON state update.
	/// @param id SignalGroup identifier.
	/// @param stateUpdate JSON state update (may be partial).
	/// @param groupTicks Group-level ticks from ObjectStateUpdateGroup (used as fallback for stateticks).
	void updateSignalGroupState(const std::string& id, const nlohmann::json& stateUpdate, genfi::Ticks groupTicks);

	/// @brief Updates an Output instance from a JSON state update.
	/// @param id Output identifier.
	/// @param stateUpdate JSON state update (may be partial).
	/// @param groupTicks Group-level ticks from ObjectStateUpdateGroup (used as fallback for stateticks).
	void updateOutputState(const std::string& id, const nlohmann::json& stateUpdate, genfi::Ticks groupTicks);

	/// @brief Updates a Detector instance from a JSON state update.
	/// @param id Detector identifier.
	/// @param stateUpdate JSON state update (may be partial).
	/// @param groupTicks Group-level ticks from ObjectStateUpdateGroup (used as fallback for stateticks).
	void updateDetectorState(const std::string& id, const nlohmann::json& stateUpdate, genfi::Ticks groupTicks);

	/// @brief Updates an Input instance from a JSON state update.
	/// @param id Input identifier.
	/// @param stateUpdate JSON state update (may be partial).
	/// @param groupTicks Group-level ticks from ObjectStateUpdateGroup (used as fallback for stateticks).
	void updateInputState(const std::string& id, const nlohmann::json& stateUpdate, genfi::Ticks groupTicks);

protected:
	/**
	 * @brief Connection infrastructure
	 * 
	 * Manages the TCP transport, JSON-RPC session, and I/O thread for asynchronous
	 * communication with the Facilities.
	 */
	std::string host_;                                                              ///< Facilities hostname/IP.
	std::string port_;                                                              ///< Facilities TCP port.
	std::string username_;                                                          ///< Application username for registration.
	std::string password_;                                                          ///< Application password for registration.
	std::string uri_;                                                               ///< Application URI identifier for registration.
	tcp::TlsConfig tlsConfig_;                                                      ///< TLS configuration for encrypted connections.

private:
	asio::io_context io_;                                                          ///< ASIO I/O context for async operations.
	std::optional<asio::executor_work_guard<asio::io_context::executor_type>> workGuard_; ///< Work guard keeping the I/O context running; reset/re-armed across reconnects.
	std::thread ioThread_;                                                          ///< Thread running the I/O context.
	std::shared_ptr<tcp::TcpClientTransport> transport_;                       ///< TCP transport for communication.
	std::shared_ptr<jsonrpc::IMessageFramer> framer_;                              ///< JSON framer for message boundaries.
	std::shared_ptr<jsonrpc::JsonRpcSession> rpc_;                                 ///< JSON-RPC session.
	std::unique_ptr<FiSession> fi_;                                     ///< Facilities Interface session wrapper.
	std::atomic<bool> connected_{false};                                           ///< Connection state flag.
	std::atomic<ConnectionState> connectionState_{ConnectionState::Disconnected};   ///< Detailed connection state.
	std::atomic<bool> fullyInitialized_{false};                                     ///< Flag indicating if full initialization completed.

	/**
	 * @brief Application state
	 * 
	 * Tracks session state, discovered object IDs, and meta read state.
	 * Protected by stateMutex_ for thread-safe access.
	 */
	std::mutex stateMutex_;                                                        ///< Mutex protecting application state.
	std::optional<std::string> sessionId_;                                         ///< Session ID from registration.
	std::optional<std::string> intersectionId_;                                   ///< Intersection ID discovered from meta.
	std::vector<std::string> signalGroupIds_;                                     ///< Signal group IDs discovered from meta.
	std::vector<std::string> outputIds_;                                           ///< Output IDs discovered from meta.
	std::vector<std::string> detectorIds_;                                         ///< Detector IDs discovered from meta.
	std::vector<std::string> inputIds_;                                            ///< Input IDs discovered from meta.
	std::optional<std::string> spvehGeneratorId_;                                  ///< SpecialVehicleEventGenerator ID from meta.
	int64_t lastStateTicks_{0};                                                    ///< Last received state update ticks.
	std::atomic<bool> metaStarted_{false};                                         ///< Flag to prevent duplicate meta reads.

	/**
	 * @brief Local object state cache
	 * 
	 * Maintains local copies of object state, updated via ObjectStateUpdate notifications.
	 * This provides fast read access without querying the Facilities.
	 */
	std::unordered_map<std::string, ::tlcfi::tlcfi::SignalGroup> signalGroups_;     ///< Local SignalGroup state cache.
	std::unordered_map<std::string, ::tlcfi::tlcfi::Output> outputs_;               ///< Local Output state cache.
	std::unordered_map<std::string, ::tlcfi::tlcfi::Detector> detectors_;         ///< Local Detector state cache.
	std::unordered_map<std::string, ::tlcfi::tlcfi::Input> inputs_;                ///< Local Input state cache.

	/**
	 * @brief Alive checking thread
	 * 
	 * Manages the background thread that sends Alive messages at the required interval.
	 */
	std::atomic<bool> aliveKeepRunning_{false};                                    ///< Flag to control alive loop.
	std::thread aliveThread_;                                                      ///< Thread running the alive loop.
	std::atomic<bool> stopped_{false};                                             ///< Flag to ensure stop() is idempotent.
};

} // namespace tlcfi::fi

