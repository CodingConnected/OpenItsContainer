#pragma once

// --- Project Includes ---
#include "cif/ptp/PtpConfig.hpp"
#include "cif/ptp/PtpRing.hpp"

// --- Third-Party Includes ---
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

// --- Standard Library Includes ---
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <string>
#include <system_error>

// --- Namespace Declaration ---
namespace cif::ptp {

/**
 * @brief One PTP link: its configuration, the two byte rings that bridge the
 *        hosted controller and the socket, and (connect-mode) the socket state
 *        machine that keeps a full-duplex TCP connection to the peer alive.
 *
 * Ownership of the rings by thread:
 *   - IBER: produced by the io thread (socket read handler), consumed by the
 *     control thread (lees_char_COM_IBER).
 *   - UBER: produced by the control thread (schrijf_char_COM_UBER), consumed by
 *     the io thread (the periodic write pump).
 * Each ring therefore has exactly one producer and one consumer — no mutex.
 *
 * All socket/timer work runs on the shared io_context passed at construction;
 * @ref bringUp / @ref bringDown just post onto it and are safe to call from the
 * control thread. Phase 2 implements @ref Mode::Connect (dial + 60 s reconnect +
 * full-duplex byte shuttle). @ref Mode::Listen is a logged no-op until Phase 3.
 */
class PtpLink {
public:
    /**
     * @brief Construct a link.
     * @param cfg               This link's configuration.
     * @param ringBytes         Usable capacity of each ring (>= MIN_RING_BYTES).
     * @param io                Shared io_context all socket/timer ops run on.
     * @param reconnectInterval Delay between reconnect attempts (spec: 60 s).
     */
    PtpLink(PtpLinkConfig cfg, std::size_t ringBytes, asio::io_context& io,
            std::chrono::milliseconds reconnectInterval);

    const PtpLinkConfig& config() const noexcept { return cfg_; }

    PtpRing& iber() noexcept { return iber_; }
    PtpRing& uber() noexcept { return uber_; }

    /**
     * @brief Start the link (posts onto the io_context).
     *
     * Connect-mode: begin dialing @c peerIp:tcpPort and start the write pump; on
     * failure/disconnect, retry every @c reconnectInterval. Listen-mode: arm the
     * write pump and wait for @ref adoptSocket to hand over an accepted
     * connection (the accepting is done by @ref PtpListener, which owns the
     * shared listen port and routes by peer IP).
     */
    void bringUp();

    /**
     * @brief Adopt an accepted connection (listen-mode only; posts onto the
     *        io_context). Called by @ref PtpListener when a peer whose address
     *        matches this link's @c peerIp connects. Replaces any existing
     *        connection (a peer that reconnects supersedes its stale socket).
     * @param sock A connected socket bound to the same io_context.
     */
    void adoptSocket(asio::ip::tcp::socket sock);

    /**
     * @brief Stop the link: cancel timers and close the socket (posts onto the
     *        io_context). Safe to call from any thread.
     */
    void bringDown();

    /// @brief True once @ref bringUp has been called.
    bool isStarted() const noexcept { return started_.load(std::memory_order_relaxed); }
    /// @brief True while a peer connection is established.
    bool isConnected() const noexcept { return connected_.load(std::memory_order_relaxed); }

private:
    // All of the following run on the io thread.
    void startConnect();
    void onConnect(const std::error_code& ec);
    void startRead();
    void onRead(const std::error_code& ec, std::size_t n);
    void armDrainTimer();
    void pumpWrites();
    void onWrite(const std::error_code& ec, std::size_t n);
    void onDisconnect();
    void scheduleReconnect();
    void closeSocket();

    PtpLinkConfig cfg_;
    PtpRing iber_;
    PtpRing uber_;

    asio::io_context& io_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer reconnectTimer_;
    asio::steady_timer drainTimer_;
    std::chrono::milliseconds reconnectInterval_;

    std::array<char, 512> readBuf_{};
    std::string writeBuf_;

    std::atomic<bool> started_{false};
    std::atomic<bool> connected_{false};
    bool writing_ = false;   ///< io thread only: an async_write is in flight.
    bool stopping_ = false;  ///< io thread only: bringDown requested.
};

} // namespace cif::ptp
