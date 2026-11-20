#include "FiFacilitiesServer.hpp"
#include "models/gen-fi/Json.hpp"
#include "models/gen-fi/Enums.hpp"
#include "models/tlc-fi/Objects.hpp"
#include "models/tlc-fi/Json.hpp"
#include "jsonrpc/RpcTypes.hpp"
#include "models/tlc-fi/Enums.hpp"

#include <iostream>
#include <sstream>
#include <iomanip>

namespace tlcfi::fi {

FiFacilitiesServer::FiFacilitiesServer(asio::io_context& io, unsigned short port, const tcp::TlsConfig& tlsConfig)
    : io_(io)
    , port_(port)
{
    server_ = std::make_shared<tcp::TcpServer>(io_, port_, tlsConfig);

    onLog = [](LogLevel level, const std::string& message) {
        switch (level) {
        case LogLevel::Trace:   std::cout << "[TRACE] " << message << "\n"; break;
        case LogLevel::Debug:   std::cout << "[DEBUG] " << message << "\n"; break;
        case LogLevel::Info:    std::cout << "[INFO] "  << message << "\n"; break;
        case LogLevel::Warning: std::cout << "[WARNING] " << message << "\n"; break;
        case LogLevel::Error:   std::cerr << "[ERROR] " << message << "\n"; break;
        case LogLevel::Fatal:   std::cerr << "[FATAL] " << message << "\n"; break;
        }
    };
}

void FiFacilitiesServer::log(LogLevel level, const std::string& message) {
    if (onLog) onLog(level, message);
}

FiFacilitiesServer::~FiFacilitiesServer() {
    log(LogLevel::Debug, "[SERVER] Destructor called");
    try {
        stop();
        log(LogLevel::Debug, "[SERVER] stop() completed in destructor");
    } catch (const std::exception& e) {
        log(LogLevel::Error, std::string("[SERVER] Exception in destructor: ") + e.what());
    }
    log(LogLevel::Debug, "[SERVER] Destructor exiting");
}

void FiFacilitiesServer::start() {
    if (running_.exchange(true)) {
        return;  // Already running
    }

    server_->onAccept = [this](std::shared_ptr<tcp::TcpServerTransport> conn) {
        handleNewConnection(conn);
    };

    // Start monitoring thread
    timeoutMonitorThread_ = std::thread([this]() {
        runTimeoutMonitor();
    });

    server_->start();
    log(LogLevel::Info, "[SERVER] TLC Facilities listening on port " + std::to_string(port_));
    log(LogLevel::Info, "[SERVER] Protocol: GEN-FI 1.1.0 / TLC-FI 1.1.0");
    // Note: TLS status is logged by TcpServerTransport via its name() method
}

void FiFacilitiesServer::stop() {
    if (!running_.exchange(false)) {
        log(LogLevel::Debug, "[SERVER] Stop called but already stopped");
        return;  // Already stopped
    }

    log(LogLevel::Info, "[SERVER] Stopping server...");

    // Stop accepting new connections
    if (server_) {
        server_->stop();
    }

    // Close all sessions (must not hold lock when calling stop!)
    std::vector<std::shared_ptr<tcp::TcpServerTransport>> transportsToStop;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        log(LogLevel::Info, "[SERVER] Closing " + std::to_string(sessions_.size()) + " sessions");

        // Collect transport pointers while holding lock
        for (auto& [id, session] : sessions_) {
            if (session->transport) {
                transportsToStop.push_back(session->transport);
            }
        }

        // Clear session maps while we still have the lock
        sessions_.clear();
        usernameSessions_.clear();
    }

    // Now call stop() WITHOUT holding the mutex
    // This is crucial because stop() triggers onDisconnected callback
    // which calls removeSession() which tries to lock the mutex
    for (auto& transport : transportsToStop) {
        transport->stop();
    }

    // Wait for monitoring thread
    if (timeoutMonitorThread_.joinable()) {
        log(LogLevel::Debug, "[SERVER] Waiting for timeout monitor to stop...");
        timeoutMonitorThread_.join();
    }

    log(LogLevel::Info, "[SERVER] Stopped");
}

void FiFacilitiesServer::handleNewConnection(
    std::shared_ptr<tcp::TcpServerTransport> conn)
{
    auto session = std::make_shared<ServerSession>();
    session->transport = conn;
    session->tempId = generateTempId();
    session->connectedAt = std::chrono::steady_clock::now();

    // Create JSON-RPC session with BalancedJsonFramer
    auto framer = std::make_shared<jsonrpc::BalancedJsonFramer>();
    auto rpc = std::make_shared<jsonrpc::JsonRpcSession>(conn, framer);
    session->rpcSession = rpc;

    const std::string tempId = session->tempId;

    // Connection lifecycle callbacks
    rpc->events.onConnected = [this, tempId]() {
        log(LogLevel::Info, "[SERVER] Client connected: " + tempId);
    };

    // Create a shared_ptr to a string that will hold the current session ID
    // This will be updated when registration completes
    auto sessionIdPtr = std::make_shared<std::string>(tempId);
    session->sessionIdPtr = sessionIdPtr;  // Store in session for updating later

    rpc->events.onDisconnected = [this, sessionIdPtr](const std::string& reason) {
        std::string identifier = *sessionIdPtr;  // Read the current ID
        std::string msg = "[SERVER] Client disconnected: " + identifier;
        if (!reason.empty()) msg += " (" + reason + ")";
        log(LogLevel::Info, msg);

        // Post to io_context to avoid calling removeSession synchronously
        // This prevents iterator invalidation issues in Debug mode
        asio::post(io_, [this, identifier]() {
            removeSession(identifier);
        });
    };

    rpc->events.onProtocolError = [this, tempId](const std::string& error) {
        log(LogLevel::Error, "[SERVER] Protocol error from " + tempId + ": " + error);
    };

    // Register RPC method handlers
    // Note: We capture session.get() since the shared_ptr is stored in sessions_ map
    // We wrap handlers in try-catch to convert FiProtocolError to JSON-RPC error responses
    rpc->registerHandler("Register", [this, session, rpc](const nlohmann::json& params) -> nlohmann::json {
        try {
            return handleRegister(session.get(), params);
        } catch (const FiProtocolError& e) {
            // Send JSON-RPC error response, then close connection
            log(LogLevel::Error, std::string("[SERVER] Protocol error in Register: ") + e.what());
            rpc->sendError(nullptr, e.code(), e.what());
            asio::post(io_, [session]() {
                if (session->transport) session->transport->stop();
            });
            return nlohmann::json();
        } catch (const std::exception& e) {
            // Catch any other exceptions to prevent std::terminate
            log(LogLevel::Error, std::string("[SERVER] Exception in Register handler: ") + e.what());
            rpc->sendError(nullptr, -32603, "Internal error");
            asio::post(io_, [session]() {
                if (session->transport) session->transport->stop();
            });
            return nlohmann::json();
        } catch (...) {
            // Catch absolutely everything
            log(LogLevel::Error, "[SERVER] Unknown exception in Register handler");
            asio::post(io_, [session]() {
                if (session->transport) session->transport->stop();
            });
            return nlohmann::json();
        }
    });

    rpc->registerHandler("Alive", [this, session, rpc](const nlohmann::json& params) -> nlohmann::json {
        try {
            return handleAlive(session.get(), params);
        } catch (const FiProtocolError& e) {
            log(LogLevel::Error, std::string("[SERVER] Protocol error in Alive: ") + e.what());
            rpc->sendError(nullptr, e.code(), e.what());
            asio::post(io_, [session]() {
                if (session->transport) session->transport->stop();
            });
            return nlohmann::json();
        } catch (const std::exception& e) {
            log(LogLevel::Error, std::string("[SERVER] Exception in Alive handler: ") + e.what());
            rpc->sendError(nullptr, -32603, "Internal error");
            return nlohmann::json();
        } catch (...) {
            log(LogLevel::Error, "[SERVER] Unknown exception in Alive handler");
            return nlohmann::json();
        }
    });

    rpc->registerHandler("Deregister", [this, session, rpc](const nlohmann::json& params) -> nlohmann::json {
        try {
            return handleDeregister(session.get(), params);
        } catch (const FiProtocolError& e) {
            log(LogLevel::Error, std::string("[SERVER] Protocol error in Deregister: ") + e.what());
            rpc->sendError(nullptr, e.code(), e.what());
            asio::post(io_, [session]() {
                if (session->transport) session->transport->stop();
            });
            return nlohmann::json();
        } catch (const std::exception& e) {
            log(LogLevel::Error, std::string("[SERVER] Exception in Deregister handler: ") + e.what());
            rpc->sendError(nullptr, -32603, "Internal error");
            asio::post(io_, [session]() {
                if (session->transport) session->transport->stop();
            });
            return nlohmann::json();
        } catch (...) {
            log(LogLevel::Error, "[SERVER] Unknown exception in Deregister handler");
            asio::post(io_, [session]() {
                if (session->transport) session->transport->stop();
            });
            return nlohmann::json();
        }
    });

    rpc->registerHandler("ReadMeta", [this, session, rpc](const nlohmann::json& params) -> nlohmann::json {
        try {
            return handleReadMeta(session.get(), params);
        } catch (const FiProtocolError& e) {
            log(LogLevel::Error, std::string("[SERVER] Protocol error in ReadMeta: ") + e.what());
            rpc->sendError(nullptr, e.code(), e.what());
            asio::post(io_, [session]() {
                if (session->transport) session->transport->stop();
            });
            return nlohmann::json();
        } catch (const std::exception& e) {
            log(LogLevel::Error, std::string("[SERVER] Exception in ReadMeta handler: ") + e.what());
            rpc->sendError(nullptr, -32603, "Internal error");
            return nlohmann::json();
        } catch (...) {
            log(LogLevel::Error, "[SERVER] Unknown exception in ReadMeta handler");
            return nlohmann::json();
        }
    });

    rpc->registerHandler("Subscribe", [this, session, rpc](const nlohmann::json& params) -> nlohmann::json {
        try {
            return handleSubscribe(session.get(), params);
        } catch (const FiProtocolError& e) {
            log(LogLevel::Error, std::string("[SERVER] Protocol error in Subscribe: ") + e.what());
            rpc->sendError(nullptr, e.code(), e.what());
            asio::post(io_, [session]() {
                if (session->transport) session->transport->stop();
            });
            return nlohmann::json();
        } catch (const std::exception& e) {
            log(LogLevel::Error, std::string("[SERVER] Exception in Subscribe handler: ") + e.what());
            rpc->sendError(nullptr, -32603, "Internal error");
            return nlohmann::json();
        } catch (...) {
            log(LogLevel::Error, "[SERVER] Unknown exception in Subscribe handler");
            return nlohmann::json();
        }
    });

    // Handle incoming notifications (e.g., UpdateState from client)
    rpc->events.onNotification = [this, session](const std::string& method, nlohmann::json params) {
        try {
            handleNotification(session.get(), method, params);
        } catch (const std::exception& e) {
            log(LogLevel::Error, std::string("[SERVER] Exception in notification handler: ") + e.what());
        }
    };

    // Store session with temporary ID
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions_[tempId] = session;
    }

    rpc->start();
}

void FiFacilitiesServer::removeSession(const std::string& identifier) {
    log(LogLevel::Debug, "[SERVER] removeSession called for: " + identifier);

    try {
        std::lock_guard<std::mutex> lock(sessionsMutex_);

        auto it = sessions_.find(identifier);
        if (it != sessions_.end()) {
            auto session = it->second;

            // Remove from username mapping
            if (!session->username.empty()) {
                usernameSessions_.erase(session->username);
            }

            // Remove from main map
            sessions_.erase(it);

            log(LogLevel::Debug, "[SERVER] Session removed: " + identifier);
        } else {
            log(LogLevel::Debug, "[SERVER] Session not found: " + identifier);
        }
    } catch (const std::exception& e) {
        log(LogLevel::Error, std::string("[SERVER] Exception in removeSession: ") + e.what());
        throw;
    }
}

FiFacilitiesServer::ServerSession* FiFacilitiesServer::findSessionByTempId(const std::string& tempId) {
    // Assumes sessionsMutex_ is already locked by caller
    auto it = sessions_.find(tempId);
    return (it != sessions_.end()) ? it->second.get() : nullptr;
}

FiFacilitiesServer::ServerSession* FiFacilitiesServer::findSessionBySessionId(const std::string& sessionId) {
    // Assumes sessionsMutex_ is already locked by caller
    auto it = sessions_.find(sessionId);
    return (it != sessions_.end()) ? it->second.get() : nullptr;
}

FiFacilitiesServer::ServerSession* FiFacilitiesServer::findSessionByUsername(const std::string& username) {
    // Assumes sessionsMutex_ is already locked by caller
    auto it = usernameSessions_.find(username);
    return (it != usernameSessions_.end()) ? it->second.get() : nullptr;
}

nlohmann::json FiFacilitiesServer::handleRegister(
    ServerSession* session,
    const nlohmann::json& params)
{
    genfi::RegistrationRequest req;
    try {
        req = params.get<genfi::RegistrationRequest>();
    } catch (const std::exception& e) {
        log(LogLevel::Error, std::string("[SERVER] Invalid RegistrationRequest: ") + e.what());
        throw FiProtocolError(PROTOCOL_ERROR_INVALID_PROTOCOL,
            "Invalid RegistrationRequest format");
    }

    log(LogLevel::Info, "[SERVER] Registration request: username=" + req.username
        + ", type=" + std::to_string(req.type)
        + ", version=" + std::to_string(req.version.major) + "." + std::to_string(req.version.minor)
        + "." + std::to_string(req.version.revision));

    // Validate protocol version (TLC-FI 1.1.0 or 2.0.1 / GEN-FI 1.1.0 or 2.0.1)
    // We support major=1 (minor=0 or 1) OR major=2 (minor=0, revision<=1)
    bool validVersion = ((req.version.major == 1 && req.version.minor <= 1) ||
                        (req.version.major == 2 && req.version.minor == 0 && req.version.revision <= 1));
    if (!validVersion) {
        log(LogLevel::Warning, "[SERVER] Unsupported protocol version: "
            + std::to_string(req.version.major) + "." + std::to_string(req.version.minor)
            + "." + std::to_string(req.version.revision));
        throw FiProtocolError(PROTOCOL_ERROR_INVALID_PROTOCOL,
            "Unsupported protocol version. Server supports 1.0.0, 1.1.0, and 2.0.1");
    }

    // Check registration throttling (GEN-FI §9.2.1: 42s interval)
    if (!canRegister(req.username)) {
        log(LogLevel::Warning, "[SERVER] Registration throttled: " + req.username
            + " (must wait 42s between attempts)");
        // Don't record this as a failed attempt - the timer is already running
        throw FiProtocolError(PROTOCOL_ERROR_NOT_AUTHORISED,
            "Registration too soon. Wait 42 seconds between registration attempts.");
    }

    // Check for existing session with same username (GEN-FI §9.2.1)
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        auto existingSession = findSessionByUsername(req.username);
        if (existingSession && existingSession != session) {
            log(LogLevel::Warning, "[SERVER] Duplicate registration attempt: " + req.username
                + " (already registered)");
            recordRegistrationAttempt(req.username, false);
            throw FiProtocolError(PROTOCOL_ERROR_NOT_AUTHORISED,
                "Application username already registered");
        }
    }

    // Authentication (dummy: accept all for test purposes)
    // Real implementation would validate req.username + req.password here
    // For now, we just store the credentials
    session->username = req.username;
    session->password = req.password;
    session->appType = static_cast<genfi::ApplicationType>(req.type);
    session->clientVersion = req.version;  // Store negotiated protocol version

    // Generate unique session ID
    std::string sessionId = generateSessionId(req.username);
    session->sessionId = sessionId;
    session->registered = true;
    session->registeredAt = std::chrono::steady_clock::now();
    session->lastAlive = session->registeredAt;

    // Update the sessionIdPtr so the disconnect callback uses the new ID
    if (session->sessionIdPtr) {
        *session->sessionIdPtr = sessionId;
    }

    // Set initial control state for control applications
    if (session->appType == genfi::ApplicationType::Control) {
        session->controlState = tlcfi::ControlState::NotConfigured;
        session->controlStateChangedAt = session->registeredAt;
    }

    // Update session maps: move the shared_ptr from tempId to sessionId
    std::shared_ptr<ServerSession> sessionPtr;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);

        // Get the shared_ptr BEFORE erasing
        auto it = sessions_.find(session->tempId);
        if (it != sessions_.end()) {
            sessionPtr = it->second;

            // Remove from tempId map
            sessions_.erase(it);

            // Add to sessionId map (reuse same shared_ptr)
            sessions_[sessionId] = sessionPtr;

            // Add to username map
            usernameSessions_[session->username] = sessionPtr;
        }
    }

    // Record successful registration
    recordRegistrationAttempt(req.username, true);

    // Create registration reply
    genfi::RegistrationReply reply;
    reply.sessionid = sessionId;
    reply.facilities.type = "TLCFacilities";
    reply.facilities.ids.push_back("FAC-001");
    // Echo back the client's protocol version
    reply.version = session->clientVersion;

    log(LogLevel::Info, "[SERVER] Registration successful: " + sessionId
        + " (username=" + session->username + ")");

    // Send Session state notification after registration reply
    // This must be done asynchronously after the reply is sent
    // Capture the shared_ptr to keep the session alive
    asio::post(io_, [this, sessionPtr]() {
        if (sessionPtr && sessionPtr->registered) {
            sendSessionStateUpdate(sessionPtr.get());
        }
    });

    return nlohmann::json(reply);
}

nlohmann::json FiFacilitiesServer::handleAlive(
    ServerSession* session,
    const nlohmann::json& params)
{
    if (!session->registered) {
        log(LogLevel::Warning, "[SERVER] Alive request from unregistered session");
        throw FiProtocolError(PROTOCOL_ERROR_NOT_AUTHORISED,
            "Not registered");
    }

    genfi::AliveObject alive;
    try {
        alive = params.get<genfi::AliveObject>();
    } catch (const std::exception& e) {
        log(LogLevel::Error, std::string("[SERVER] Invalid AliveObject: ") + e.what());
        throw FiProtocolError(-32602, "Invalid AliveObject format");
    }

    log(LogLevel::Trace, "[SERVER] << Received Alive from " + session->sessionId
        + " (client time=" + std::to_string(alive.time) + ", ticks=" + std::to_string(alive.ticks) + ")");

    if (onAliveReceived) {
        onAliveReceived();
    }

    // Update last alive timestamp
    session->lastAlive = std::chrono::steady_clock::now();

    // Echo back with server's timestamp and tick
    // TODO: Implement proper tick counter
    alive.time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();
    alive.ticks = 0;  // TODO: Implement server tick counter

    log(LogLevel::Trace, "[SERVER] >> Sending Alive response to " + session->sessionId
        + " (server time=" + std::to_string(alive.time) + ", ticks=" + std::to_string(alive.ticks) + ")");

    return nlohmann::json(alive);
}

nlohmann::json FiFacilitiesServer::handleDeregister(
    ServerSession* session,
    const nlohmann::json& params)
{
    if (!session->registered) {
        // GEN-FI §9.2.7: Reply with ProtocolErrorCode(0) if not registered
        throw FiProtocolError(PROTOCOL_ERROR_INVALID_PROTOCOL,
            "Not registered");
    }

    log(LogLevel::Info, "[SERVER] Deregister: " + session->sessionId
        + " (username=" + session->username + ")");

    // Mark as not registered (will be cleaned up on disconnect)
    session->registered = false;

    // Close the connection
    asio::post(io_, [session]() {
        if (session->transport) {
            session->transport->stop();
        }
    });

    // Return empty object (as per GEN-FI spec)
    return nlohmann::json::object();
}

nlohmann::json FiFacilitiesServer::handleReadMeta(ServerSession* session, const nlohmann::json& params) {
    (void)session;
    if (!params.contains("type") || !params.contains("ids") || !params["ids"].is_array()) {
        throw FiProtocolError(PROTOCOL_ERROR_INVALID_PROTOCOL, "Invalid ReadMeta params");
    }

    auto typeValue = params["type"];
    genfi::ObjectMeta meta;

    if (typeValue.is_string()) {
        meta = buildFacilitiesMeta();
    } else if (typeValue.is_number_integer()) {
        int typeInt = typeValue.get<int>();
        if (typeInt == static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Intersection)) {
            if (params["ids"].empty() || !params["ids"][0].is_string()) {
                throw FiProtocolError(PROTOCOL_ERROR_INVALID_PROTOCOL, "Intersection id missing");
            }
            meta = buildIntersectionMeta(params["ids"][0].get<std::string>());
        } else {
            throw FiProtocolError(PROTOCOL_ERROR_UNKNOWN_OBJECT_TYPE, "Unsupported object type for ReadMeta");
        }
    } else {
        throw FiProtocolError(PROTOCOL_ERROR_INVALID_PROTOCOL, "Unknown type field");
    }

    return nlohmann::json(meta);
}

nlohmann::json FiFacilitiesServer::handleSubscribe(ServerSession* session, const nlohmann::json& params) {
    (void)session;
    if (!params.contains("type") || !params.contains("ids") || !params["ids"].is_array()) {
        throw FiProtocolError(PROTOCOL_ERROR_INVALID_PROTOCOL, "Invalid Subscribe params");
    }

    if (!params["type"].is_number_integer() && !params["type"].is_string()) {
        throw FiProtocolError(PROTOCOL_ERROR_INVALID_PROTOCOL, "Subscribe type must be int or string");
    }

    int typeInt = -1;
    if (params["type"].is_number_integer()) {
        typeInt = params["type"].get<int>();
    } else {
        throw FiProtocolError(PROTOCOL_ERROR_UNKNOWN_OBJECT_TYPE, "String type not supported for Subscribe");
    }

    std::vector<std::string> ids;
    for (const auto& id : params["ids"]) {
        if (id.is_string()) ids.push_back(id.get<std::string>());
    }

    auto data = buildObjectData(typeInt, ids);
    return nlohmann::json(data);
}

void FiFacilitiesServer::sendSessionStateUpdate(ServerSession* session) {
    if (!session->registered || !session->rpcSession) {
        log(LogLevel::Warning, "[SERVER] Cannot send session state: session not registered or no RPC");
        return;
    }

    try {
        // Create ControlApplication (Session) object
        tlcfi::ControlApplication sessionObj;
        sessionObj.sessionid = session->sessionId;
        sessionObj.type = static_cast<int>(session->appType);
        sessionObj.controlState = session->controlState;
        // Other optional fields are left empty for now

        // Create ObjectStateUpdate
        genfi::ObjectStateUpdate update;
        update.objects.type = static_cast<int>(tlcfi::TLCObjectType::Session);
        update.objects.ids.push_back(session->sessionId);
        update.states.push_back(nlohmann::json(sessionObj));

        // Wrap in ObjectStateUpdateGroup
        genfi::ObjectStateUpdateGroup group;
        group.update.push_back(std::move(update));  // Move to avoid copies
        group.ticks = 0;  // TODO: Implement proper tick counter

        // Send as notification
        log(LogLevel::Debug, "[SERVER] >> Sending UpdateState notification to " + session->sessionId
            + " (controlState=" + std::to_string(static_cast<int>(session->controlState)) + ")");

        session->rpcSession->sendNotification("UpdateState", nlohmann::json(group));

        log(LogLevel::Debug, "[SERVER] UpdateState notification sent successfully");
    } catch (const std::exception& e) {
        log(LogLevel::Error, std::string("[SERVER] Error sending session state update: ") + e.what());
    }
}

std::string FiFacilitiesServer::generateSessionId(const std::string& username) {
    // Format: username-<counter>-<timestamp>
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    std::ostringstream oss;
    oss << username << "-" << sessionCounter_++ << "-" << ms;
    return oss.str();
}

std::string FiFacilitiesServer::generateTempId() {
    std::ostringstream oss;
    oss << "temp-" << std::setfill('0') << std::setw(6) << tempIdCounter_++;
    return oss.str();
}

bool FiFacilitiesServer::canRegister(const std::string& username) {
    std::lock_guard<std::mutex> lock(registrationMutex_);

    auto it = registrationAttempts_.find(username);
    if (it == registrationAttempts_.end()) {
        return true;  // First registration attempt
    }

    auto& attempt = it->second;

    // GEN-FI §5.7: 42s interval only applies to successful registrations
    // Failed attempts can retry immediately
    if (!attempt.successful) {
        return true;  // Last attempt failed, can retry immediately
    }

    // Last registration was successful, check if enough time has passed
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - attempt.lastAttempt;

    return elapsed >= REGISTRATION_INTERVAL;
}

void FiFacilitiesServer::recordRegistrationAttempt(const std::string& username, bool successful) {
    std::lock_guard<std::mutex> lock(registrationMutex_);

    RegistrationAttempt attempt;
    attempt.username = username;
    attempt.lastAttempt = std::chrono::steady_clock::now();
    attempt.successful = successful;

    registrationAttempts_[username] = attempt;
}

void FiFacilitiesServer::clearRegistrationThrottling(const std::string& username) {
    std::lock_guard<std::mutex> lock(registrationMutex_);
    
    if (username.empty()) {
        // Clear all registration attempts
        registrationAttempts_.clear();
    } else {
        // Clear specific username
        registrationAttempts_.erase(username);
    }
}

void FiFacilitiesServer::runTimeoutMonitor() {
    log(LogLevel::Debug, "[SERVER] Timeout monitor started");

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        std::vector<std::string> timedOutSessions;

        // Make a complete copy of the sessions map to avoid iterator invalidation
        // In Debug mode, the STL tracks iterator validity and detects when we modify
        // the map (erase + insert in handleRegister), which invalidates all iterators.
        // The solution is to make a copy of the entire map using the copy constructor.
        std::unordered_map<std::string, std::shared_ptr<ServerSession>> sessionsCopy;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            sessionsCopy = sessions_;  // Deep copy of the map
        }

        // Now iterate over the copy without holding the lock
        for (const auto& [id, session] : sessionsCopy) {
            // Check for no registration timeout (GEN-FI §9.2.5)
            if (session->isRegistrationTimedOut()) {
                auto waited = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - session->connectedAt).count();
                log(LogLevel::Warning, "[SERVER] No registration timeout: " + id
                    + " (waited " + std::to_string(waited) + "s)");
                timedOutSessions.push_back(id);
                continue;
            }

            // Check for alive timeout (GEN-FI §5.6)
            if (session->isAliveTimedOut()) {
                log(LogLevel::Warning, "[SERVER] Alive timeout: " + id
                    + " (username=" + session->username
                    + ", timeout=" + std::to_string(session->getAliveTimeout().count()) + "s)");
                timedOutSessions.push_back(id);
                continue;
            }

            // Check for control state timeout (TLC-FI §4.8.1)
            if (session->isControlStateTimedOut()) {
                log(LogLevel::Warning, "[SERVER] Control state timeout: " + id
                    + " (state=" + std::to_string(static_cast<int>(session->controlState)) + ")");
                // Transition to Error state
                session->controlState = tlcfi::ControlState::Error;
                // TODO Phase 6: Send session state update notification
                timedOutSessions.push_back(id);
                continue;
            }
        }

        // Close timed-out sessions (must not hold lock when calling stop!)
        // Get transport pointers while holding lock, then release lock before calling stop()
        std::vector<std::shared_ptr<tcp::TcpServerTransport>> transportsToStop;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            for (const auto& id : timedOutSessions) {
                auto it = sessions_.find(id);
                if (it != sessions_.end() && it->second->transport) {
                    transportsToStop.push_back(it->second->transport);
                }
            }
        }

        // Now call stop() WITHOUT holding the mutex
        // This is crucial because stop() triggers onDisconnected callback
        // which calls removeSession() which needs to lock the mutex
        for (auto& transport : transportsToStop) {
            transport->stop();
        }
    }

    log(LogLevel::Debug, "[SERVER] Timeout monitor stopped");
}

genfi::ObjectMeta FiFacilitiesServer::buildFacilitiesMeta() {
    std::lock_guard<std::mutex> lock(catalogMutex_);
    genfi::ObjectMeta meta;
    meta.objects.type = "TLCFacilities";
    meta.objects.ids = { catalog_.facilitiesId };

    nlohmann::json obj;
    obj["id"] = catalog_.facilitiesId;
    obj["intersections"] = { catalog_.intersectionId };
    meta.meta.push_back(obj);
    meta.ticks = 0;
    return meta;
}

genfi::ObjectMeta FiFacilitiesServer::buildIntersectionMeta(const std::string& intersectionId) {
    std::lock_guard<std::mutex> lock(catalogMutex_);
    genfi::ObjectMeta meta;
    meta.objects.type = static_cast<int>(::tlcfi::tlcfi::TLCObjectType::Intersection);
    meta.objects.ids = { intersectionId };

    nlohmann::json obj;
    obj["id"] = intersectionId;
    obj["signalgroups"] = catalog_.signalGroups;
    obj["outputs"] = catalog_.outputs;
    obj["detectors"] = catalog_.detectors;
    obj["inputs"] = catalog_.inputs;
    meta.meta.push_back(obj);
    meta.ticks = 0;
    return meta;
}

genfi::ObjectData FiFacilitiesServer::buildObjectData(int typeValue, const std::vector<std::string>& ids) {
    std::lock_guard<std::mutex> lock(catalogMutex_);
    genfi::ObjectData data;
    data.objects.type = typeValue;
    data.objects.ids = ids;

    switch (static_cast<::tlcfi::tlcfi::TLCObjectType>(typeValue)) {
        case ::tlcfi::tlcfi::TLCObjectType::SignalGroup: {
            for (const auto& id : ids) {
                ::tlcfi::tlcfi::SignalGroup sg;
                sg.id = id;
                sg.state = ::tlcfi::tlcfi::SignalGroupState::Dark;
                sg.stateticks = 1;
                data.data.push_back(nlohmann::json(sg));
            }
            break;
        }
        case ::tlcfi::tlcfi::TLCObjectType::Output: {
            for (const auto& id : ids) {
                ::tlcfi::tlcfi::Output out;
                out.id = id;
                out.state = 1;
                out.stateticks = 1;
                data.data.push_back(nlohmann::json(out));
            }
            break;
        }
        case ::tlcfi::tlcfi::TLCObjectType::Detector: {
            for (const auto& id : ids) {
                ::tlcfi::tlcfi::Detector det;
                det.id = id;
                det.state = ::tlcfi::tlcfi::DetectorState::Unoccupied;
                det.stateticks = 1;
                data.data.push_back(nlohmann::json(det));
            }
            break;
        }
        case ::tlcfi::tlcfi::TLCObjectType::Input: {
            for (const auto& id : ids) {
                ::tlcfi::tlcfi::Input inp;
                inp.id = id;
                inp.state = 1;
                inp.stateticks = 1;
                data.data.push_back(nlohmann::json(inp));
            }
            break;
        }
        case ::tlcfi::tlcfi::TLCObjectType::Intersection: {
            for (const auto& id : ids) {
                nlohmann::json isect;
                isect["id"] = id;
                // Include intersection control state so controller can determine if it's ready
                isect["state"] = static_cast<int>(::tlcfi::tlcfi::IntersectionControlState::Control);
                isect["stateticks"] = 0;
                data.data.push_back(isect);
            }
            break;
        }
        default:
            // Leave data empty for unsupported types
            break;
    }

    data.ticks = 0;
    return data;
}

void FiFacilitiesServer::handleNotification(ServerSession* session, const std::string& method, const nlohmann::json& params) {
    if (method == "UpdateState") {
        handleUpdateState(session, params);
    } else {
        log(LogLevel::Debug, "[SERVER] Ignoring unknown notification: " + method);
    }
}

void FiFacilitiesServer::handleUpdateState(ServerSession* session, const nlohmann::json& params) {
    if (!session->registered) {
        log(LogLevel::Warning, "[SERVER] UpdateState from unregistered session, ignoring");
        return;
    }

    // Parse the ObjectStateUpdateGroup
    genfi::ObjectStateUpdateGroup group;
    try {
        group = params.get<genfi::ObjectStateUpdateGroup>();
    } catch (const std::exception& e) {
        log(LogLevel::Error, std::string("[SERVER] Invalid UpdateState params: ") + e.what());
        return;
    }

    for (const auto& upd : group.update) {
        int typeValue = -1;
        if (std::holds_alternative<int>(upd.objects.type)) {
            typeValue = std::get<int>(upd.objects.type);
        } else if (std::holds_alternative<std::string>(upd.objects.type)) {
            // String type not handled for now
            continue;
        }

        // Process Session/ControlApplication updates for control state transitions
        if (typeValue == static_cast<int>(tlcfi::TLCObjectType::Session)) {
            for (const auto& s : upd.states) {
                if (s.contains("reqControlState") && s["reqControlState"].is_number_integer()) {
                    auto reqState = static_cast<tlcfi::ControlState>(s["reqControlState"].get<int>());
                    log(LogLevel::Debug, "[SERVER] << Received reqControlState="
                        + std::to_string(static_cast<int>(reqState)) + " from " + session->sessionId);
                    processControlStateTransition(session, reqState);
                }
            }
        }
        // Other object type updates (SignalGroup, Output, etc.) are accepted silently
    }
}

void FiFacilitiesServer::processControlStateTransition(ServerSession* session, tlcfi::ControlState reqControlState) {
    if (session->appType != genfi::ApplicationType::Control) {
        log(LogLevel::Warning, "[SERVER] Control state transition from non-control app, ignoring");
        return;
    }

    auto current = session->controlState;
    auto now = std::chrono::steady_clock::now();

    switch (reqControlState) {
        case tlcfi::ControlState::Offline: {
            if (current == tlcfi::ControlState::NotConfigured) {
                session->controlState = tlcfi::ControlState::Offline;
                session->controlStateChangedAt = now;
                log(LogLevel::Info, "[SERVER] Control state: NotConfigured -> Offline (" + session->sessionId + ")");
                sendSessionStateUpdate(session);
            } else {
                log(LogLevel::Warning, "[SERVER] Invalid transition: reqControlState=Offline from state="
                    + std::to_string(static_cast<int>(current)));
            }
            break;
        }
        case tlcfi::ControlState::ReadyToControl: {
            if (current == tlcfi::ControlState::Offline) {
                // Per TLC-FI §4.8.2: Offline + reqControlState=ReadyToControl → StartControl
                // The server transitions through ReadyToControl directly to StartControl
                session->controlState = tlcfi::ControlState::StartControl;
                session->controlStateChangedAt = now;
                log(LogLevel::Info, "[SERVER] Control state: Offline -> StartControl (" + session->sessionId + ")");
                sendSessionStateUpdate(session);
            } else {
                log(LogLevel::Warning, "[SERVER] Invalid transition: reqControlState=ReadyToControl from state="
                    + std::to_string(static_cast<int>(current)));
            }
            break;
        }
        case tlcfi::ControlState::InControl: {
            if (current == tlcfi::ControlState::StartControl) {
                session->controlState = tlcfi::ControlState::InControl;
                session->controlStateChangedAt = now;
                log(LogLevel::Info, "[SERVER] Control state: StartControl -> InControl (" + session->sessionId + ")");
                sendSessionStateUpdate(session);
            } else {
                log(LogLevel::Warning, "[SERVER] Invalid transition: reqControlState=InControl from state="
                    + std::to_string(static_cast<int>(current)));
            }
            break;
        }
        default: {
            log(LogLevel::Warning, "[SERVER] Unhandled reqControlState="
                + std::to_string(static_cast<int>(reqControlState)));
            break;
        }
    }
}

} // namespace tlcfi::fi
