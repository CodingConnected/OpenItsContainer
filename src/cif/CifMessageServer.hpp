#pragma once

// --- Standard Library Includes ---
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// --- Third-Party Includes ---
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

// --- Project Includes ---
#include "fi/LogLevel.hpp"
#include "CifMessageServerConfig.hpp"

namespace cif {

/**
 * @brief TCP server for CIF IBER/UBER message ring buffers
 *
 * Exposes the CIF serial message interface over TCP. External tools can connect
 * and send text commands which are written into CIF_IBER[]. Responses from
 * CIF_UBER[] are broadcast to all connected clients.
 *
 * The server owns its own asio::io_context and worker thread, independent from
 * the TLC-FI protocol handling.
 *
 * By default, connections are accepted at any time. Inbound data received
 * while the control loop is inactive is discarded (or answered with a
 * configured inactiveMessage). Existing sessions survive disableClients().
 * Set cifClientsOnlyDuringControl in the config to restore the legacy
 * behaviour where connections are refused outside the active control window.
 *
 * Ring buffer protocol:
 * - Incoming (TCP -> IBER): lines terminated by \n or \r\n are stripped and
 *   written into CIF_IBER[] with \r terminator
 * - Outgoing (UBER -> TCP): messages delimited by \r\n in CIF_UBER[] are
 *   sent to all connected clients as lines terminated by \r\n
 *
 * @warning Incoming data reaches the controller command channel. The server
 *          binds to loopback by default; a non-loopback bind address must be
 *          an explicit deployment decision with compensating network controls.
 */
class CifMessageServer {
public:
    /**
     * @brief Construct a message server with the supplied bind and ring configuration.
     * @param config Server configuration; defaults to a loopback-only bind.
     */
    CifMessageServer(const CifMessageServerConfig& config);

    ~CifMessageServer();

    /**
     * @brief Start the TCP server and its worker thread.
     *
     * Starts listening but does not accept clients until enableClients() is called.
     */
    void start();

    /**
     * @brief Stop the TCP server and join the worker thread.
     */
    void stop();

    /**
     * @brief Allow client connections and start accepting.
     *
     * Called when the control loop starts. New connections will be accepted
     * and existing accept operations will proceed.
     */
    void enableClients();

    /**
     * @brief Disconnect all clients and refuse new connections.
     *
     * Called when the control loop stops. All existing client sockets are
     * closed and new connections are immediately closed until enableClients()
     * is called again.
     */
    void disableClients();

    /**
     * @brief Poll the CIF_UBER[] ring buffer and send complete messages to clients.
     *
     * Must be called synchronously from the control loop thread.
     * Reads bytes from CIF_UBER_LEES toward CIF_UBER_SCHRIJF, detects \r\n
     * boundaries, and sends complete messages to all connected TCP clients.
     */
    void pollDataOut();

    /// @brief Logging callback — invoked for all log messages from this component.
    ::tlcfi::fi::LogCallback onLog;

private:
    void log(::tlcfi::fi::LogLevel level, const std::string& message);
    struct Session;

    void doAccept();
    void doRead(std::shared_ptr<Session> session);
    void enqueueWrite(const std::shared_ptr<Session>& session,
                      std::shared_ptr<std::string> message);
    void doWrite(const std::shared_ptr<Session>& session);
    void disconnectSession(const std::shared_ptr<Session>& session, const char* reason);
    void writeToDataIn(const std::string& message);
    void broadcastToClients(const std::string& message);
    void removeSession(const std::shared_ptr<Session>& session);
    void closeAllSessions();

    struct Session {
        Session(asio::ip::tcp::socket sock, std::size_t maxInboundLineSize)
            : socket(std::move(sock))
            , maxLineSize(maxInboundLineSize) {}
        asio::ip::tcp::socket socket;
        std::array<char, 256> readBuf{};
        std::string lineBuffer;
        std::size_t maxLineSize;
        std::deque<std::shared_ptr<std::string>> writeQueue;
        std::size_t pendingWriteBytes{0};
        bool writeInProgress{false};
        bool writeQueueOverflowed{false};

        enum class IacState : std::uint8_t { None, Iac, IacOption };
        IacState iacState{IacState::None};
        bool lastWasCR{false};
    };

    CifMessageServerConfig config_;
    asio::io_context ioContext_;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::thread workerThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> clientsEnabled_{false};

    std::mutex sessionsMutex_;
    std::vector<std::shared_ptr<Session>> sessions_;

    // Partial dataOut message accumulator (for messages spanning multiple pollDataOut calls)
    std::string dataOutPartial_;
};

} // namespace cif
