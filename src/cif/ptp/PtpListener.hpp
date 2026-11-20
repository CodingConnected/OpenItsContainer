#pragma once

// --- Third-Party Includes ---
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

// --- Standard Library Includes ---
#include <cstdint>
#include <system_error>
#include <utility>
#include <vector>

// --- Namespace Declaration ---
namespace cif::ptp {

class PtpLink;

/**
 * @brief TCP acceptor for one listen port, demultiplexing incoming connections
 *        to listen-mode links by peer IP.
 *
 * Several listen-mode links can share a single TCP port (the spec fixes 7100),
 * so one acceptor serves them all: on each accepted connection it matches the
 * remote address against the configured @c peerIp of each link on this port and
 * hands the socket to that link via @ref PtpLink::adoptSocket. A connection from
 * an address that matches no configured link is closed (logged once).
 *
 * All work runs on the shared io_context. @ref start / @ref stop post onto it
 * and are safe to call from the control thread.
 *
 * @note Binds to the IPv4 wildcard (0.0.0.0:port). IPv6 peers are not matched.
 */
class PtpListener {
public:
    /// A configured (peer address, target link) pair on this listener's port.
    using Peer = std::pair<asio::ip::address, PtpLink*>;

    PtpListener(asio::io_context& io, std::uint16_t port, std::vector<Peer> peers);

    /// @brief Open + bind + listen, then begin accepting (posts onto io_context).
    void start();
    /// @brief Stop accepting and close the acceptor (posts onto io_context).
    void stop();

    std::uint16_t port() const noexcept { return port_; }

private:
    void doAccept();
    void onAccept(const std::error_code& ec, asio::ip::tcp::socket sock);
    PtpLink* linkForAddress(const asio::ip::address& addr) const;

    asio::io_context& io_;
    std::uint16_t port_;
    std::vector<Peer> peers_;
    asio::ip::tcp::acceptor acceptor_;
    bool stopping_ = false;
};

} // namespace cif::ptp
