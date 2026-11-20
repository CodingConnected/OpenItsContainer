#pragma once

#include "fi/LogLevel.hpp"
#include "tcp/TcpServer.hpp"
#include "tcp/TcpServerTransport.hpp"
#include "jsonrpc/JsonRpcSession.hpp"
#include "jsonrpc/BalancedJsonFramer.hpp"
#include "models/gen-fi/GenFiTypes.hpp"
#include "models/gen-fi/Structures.hpp"
#include "models/gen-fi/Enums.hpp"
#include "models/tlc-fi/Types.hpp"
#include "models/tlc-fi/Enums.hpp"

#include <asio/io_context.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

namespace tlcfi::fi {

/**
 * @brief Exception for TLC-FI protocol errors
 *
 * Thrown when application-level protocol violations occur (e.g., wrong version,
 * duplicate registration, unauthorized access). These are converted to JSON-RPC
 * error responses sent to the client before closing the connection.
 */
class FiProtocolError : public std::runtime_error {
public:
    FiProtocolError(int code, const std::string& message)
        : std::runtime_error(message)
        , errorCode_(code)
    {}

    int code() const { return errorCode_; }

private:
    int errorCode_;
};

/**
 * @brief Facilities server implementing TLC-FI protocol
 *
 * This class implements the server side of the TLC-FI (Traffic Light Controller
 * Facilities Interface) protocol as specified in the GEN-FI and TLC-FI specifications.
 *
 * @remark This class is NOT MEANT FOR PRODUCTION USE. It is intended for testing and development purposes only.
 *
 * Key features:
 * - Session management with timeout enforcement
 * - Registration with duplicate prevention
 * - Alive monitoring per application type
 * - Protocol version validation
 * - Connection throttling (42s between registration attempts)
 */
class FiFacilitiesServer {
public:
    /**
     * @brief Construct a new FiFacilitiesServer
     * @param io Asio io_context for network operations
     * @param port TCP port to listen on (default: 11501 for non-TLS)
     * @param tlsConfig Optional TLS configuration. If not provided or disabled, plain TCP is used.
     */
    FiFacilitiesServer(asio::io_context& io, unsigned short port = 11501, const tcp::TlsConfig& tlsConfig = tcp::TlsConfig{});

    /**
     * @brief Destructor - stops server and cleans up sessions
     */
    ~FiFacilitiesServer();

    /**
     * @brief Start the server (begin accepting connections)
     */
    void start();

    /**
     * @brief Stop the server and disconnect all clients
     */
    void stop();

    /**
     * @brief Check if server is running
     */
    bool isRunning() const { return running_.load(); }

    /**
     * @brief Returns the local TCP port the server is listening on.
     *
     * When constructed with port 0 the OS assigns a free ephemeral port;
     * this returns the actual bound port.
     */
    unsigned short port() const { return server_ ? server_->port() : port_; }

    /**
     * @brief Clear registration throttling for a username (test helper)
     * 
     * This method is intended for testing purposes to allow immediate
     * re-registration without waiting for the 42-second throttling period.
     * 
     * @param username Username to clear throttling for (empty string clears all)
     */
    void clearRegistrationThrottling(const std::string& username = "");

    /// @brief Logging callback — invoked for all log messages from this component.
    LogCallback onLog;

    /// Test hook invoked when an Alive request reaches the fixture.
    std::function<void()> onAliveReceived;

private:
    void log(LogLevel level, const std::string& message);
    /**
     * @brief Internal session state tracking
     *
     * Tracks per-connection state including:
     * - RPC session and transport
     * - Registration status and credentials
     * - Alive monitoring timestamps
     * - Control state (for control applications)
     */
    struct ServerSession {
        // Connection
        std::shared_ptr<tcp::TcpServerTransport> transport;
        std::shared_ptr<jsonrpc::JsonRpcSession> rpcSession;
        std::string tempId;  // Temporary ID before registration
        std::shared_ptr<std::string> sessionIdPtr;  // Pointer to current ID for callbacks

        // Registration
        std::string sessionId;  // Unique session ID (empty if not registered)
        std::string username;
        std::string password;  // Stored for validation
        genfi::ApplicationType appType = genfi::ApplicationType::Consumer;
        genfi::ProtocolVersion clientVersion {1, 1, 0};  // Client's negotiated protocol version
        bool registered = false;
        std::chrono::steady_clock::time_point connectedAt;
        std::chrono::steady_clock::time_point registeredAt;

        // Alive monitoring
        std::chrono::steady_clock::time_point lastAlive;

        // Control state (for control applications)
        tlcfi::ControlState controlState = tlcfi::ControlState::NotConfigured;
        std::chrono::steady_clock::time_point controlStateChangedAt;

        // Timeouts (from spec)
        static constexpr auto NO_REGISTRATION_TIMEOUT = std::chrono::seconds(10);  // GEN-FI: ITS-A Alive interval
        static constexpr auto ALIVE_TIMEOUT_CONTROL = std::chrono::seconds(5);     // TLC-FI: 2.5 * 2s = 5s
        static constexpr auto ALIVE_TIMEOUT_OTHER = std::chrono::seconds(25);      // GEN-FI: 2.5 * 10s = 25s
        static constexpr auto NOTCONFIGURED_TIMEOUT = std::chrono::seconds(60);    // TLC-FI: §4.8.1
        static constexpr auto STARTCONTROL_TIMEOUT = std::chrono::seconds(5);      // TLC-FI: §4.8.1
        static constexpr auto ENDCONTROL_TIMEOUT = std::chrono::seconds(180);      // TLC-FI: §4.8.1

        /**
         * @brief Get alive timeout for this session based on app type
         */
        std::chrono::seconds getAliveTimeout() const {
            return (appType == genfi::ApplicationType::Control)
                ? ALIVE_TIMEOUT_CONTROL
                : ALIVE_TIMEOUT_OTHER;
        }

        /**
         * @brief Check if registration timeout expired (no registration after connect)
         */
        bool isRegistrationTimedOut() const {
            if (registered) return false;
            auto now = std::chrono::steady_clock::now();
            return (now - connectedAt) > NO_REGISTRATION_TIMEOUT;
        }

        /**
         * @brief Check if alive monitoring timed out
         */
        bool isAliveTimedOut() const {
            if (!registered) return false;
            auto now = std::chrono::steady_clock::now();
            return (now - lastAlive) > getAliveTimeout();
        }

        /**
         * @brief Check if control state timeout expired (for control applications)
         */
        bool isControlStateTimedOut() const {
            if (!registered || appType != genfi::ApplicationType::Control) {
                return false;
            }

            auto now = std::chrono::steady_clock::now();
            auto elapsed = now - controlStateChangedAt;

            switch (controlState) {
                case tlcfi::ControlState::NotConfigured:
                    return elapsed > NOTCONFIGURED_TIMEOUT;
                case tlcfi::ControlState::StartControl:
                    return elapsed > STARTCONTROL_TIMEOUT;
                case tlcfi::ControlState::EndControl:
                    return elapsed > ENDCONTROL_TIMEOUT;
                default:
                    return false;
            }
        }
    };

    /**
     * @brief Tracking for preventing duplicate registrations
     */
    struct RegistrationAttempt {
        std::string username;
        std::chrono::steady_clock::time_point lastAttempt;
        bool successful;
    };

    // Connection handling
    void handleNewConnection(std::shared_ptr<tcp::TcpServerTransport> conn);
    void removeSession(const std::string& identifier);  // Can be tempId or sessionId

    // Session tracking
    ServerSession* findSessionByTempId(const std::string& tempId);
    ServerSession* findSessionBySessionId(const std::string& sessionId);
    ServerSession* findSessionByUsername(const std::string& username);

    // RPC method handlers
    nlohmann::json handleRegister(ServerSession* session, const nlohmann::json& params);
    nlohmann::json handleAlive(ServerSession* session, const nlohmann::json& params);
    nlohmann::json handleDeregister(ServerSession* session, const nlohmann::json& params);
    nlohmann::json handleReadMeta(ServerSession* session, const nlohmann::json& params);
    nlohmann::json handleSubscribe(ServerSession* session, const nlohmann::json& params);

    // Notification handlers
    void handleNotification(ServerSession* session, const std::string& method, const nlohmann::json& params);
    void handleUpdateState(ServerSession* session, const nlohmann::json& params);

    /**
     * @brief Processes a control state transition request from the client.
     *
     * Implements the TLC-FI control state machine (§4.8.2):
     * - NotConfigured + reqControlState=Offline → Offline
     * - Offline + reqControlState=ReadyToControl → StartControl
     * - StartControl + reqControlState=InControl → InControl
     *
     * Invalid transitions are logged and ignored.
     *
     * @param session The server session to update.
     * @param reqControlState The requested control state from the client.
     */
    void processControlStateTransition(ServerSession* session, tlcfi::ControlState reqControlState);

    // Session state notifications
    void sendSessionStateUpdate(ServerSession* session);

    // Session management
    std::string generateSessionId(const std::string& username);
    std::string generateTempId();
    bool canRegister(const std::string& username);
    void recordRegistrationAttempt(const std::string& username, bool successful);

    // Monitoring threads
    void runTimeoutMonitor();

    // Minimal facilities catalog for testing
    struct FacilitiesCatalog {
        std::string facilitiesId {"FAC-001"};
        std::string intersectionId {"INT-1"};
        std::vector<std::string> signalGroups {"SG1", "SG2"};
        std::vector<std::string> outputs {"OUT1", "OUT2"};
        std::vector<std::string> detectors {"DET1"};
        std::vector<std::string> inputs {"IN1"};
    };

    genfi::ObjectMeta buildFacilitiesMeta();
    genfi::ObjectMeta buildIntersectionMeta(const std::string& intersectionId);
    genfi::ObjectData buildObjectData(int typeValue, const std::vector<std::string>& ids);

    // Protocol error codes
    static constexpr int PROTOCOL_ERROR_INVALID_PROTOCOL = -32000;
    static constexpr int PROTOCOL_ERROR_NOT_AUTHORISED = -32001;
    static constexpr int PROTOCOL_ERROR_UNKNOWN_OBJECT_TYPE = -32002;

    // Member variables
    asio::io_context& io_;
    unsigned short port_;
    std::shared_ptr<tcp::TcpServer> server_;  ///< Shared so TcpServer can use enable_shared_from_this.

    // Session tracking
    std::mutex sessionsMutex_;
    std::unordered_map<std::string, std::shared_ptr<ServerSession>> sessions_;  // Key: tempId or sessionId
    std::unordered_map<std::string, std::shared_ptr<ServerSession>> usernameSessions_;  // Key: username

    // Registration throttling
    std::mutex registrationMutex_;
    std::unordered_map<std::string, RegistrationAttempt> registrationAttempts_;
    static constexpr auto REGISTRATION_INTERVAL = std::chrono::seconds(42);  // GEN-FI §5.7

    // Counters
    std::atomic<uint64_t> sessionCounter_{0};
    std::atomic<uint64_t> tempIdCounter_{0};

    // Threading
    std::atomic<bool> running_{false};
    std::thread timeoutMonitorThread_;

    FacilitiesCatalog catalog_;
    std::mutex catalogMutex_;
};

} // namespace tlcfi::fi
