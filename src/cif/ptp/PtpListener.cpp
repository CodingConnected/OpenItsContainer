#include "cif/ptp/PtpListener.hpp"

#include "cif/ptp/PtpLink.hpp"

#include <asio/post.hpp>

#include <iostream>
#include <utility>

namespace cif::ptp {

PtpListener::PtpListener(asio::io_context& io, std::uint16_t port, std::vector<Peer> peers)
    : io_(io)
    , port_(port)
    , peers_(std::move(peers))
    , acceptor_(io) {}

void PtpListener::start() {
    asio::post(io_, [this] {
        if (stopping_) return;
        const asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), port_);
        std::error_code ec;
        acceptor_.open(endpoint.protocol(), ec);
        if (!ec) acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
        if (!ec) acceptor_.bind(endpoint, ec);
        if (!ec) acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            std::cout << "[ptp] listen on port " << port_ << " failed: " << ec.message()
                      << " (links on this port stay down)" << std::endl;
            return;
        }
        doAccept();
    });
}

void PtpListener::stop() {
    asio::post(io_, [this] {
        stopping_ = true;
        std::error_code ec;
        acceptor_.close(ec);
    });
}

void PtpListener::doAccept() {
    if (stopping_) return;
    acceptor_.async_accept(
        [this](const std::error_code& ec, asio::ip::tcp::socket sock) { onAccept(ec, std::move(sock)); });
}

void PtpListener::onAccept(const std::error_code& ec, asio::ip::tcp::socket sock) {
    if (stopping_) return;
    if (ec) {
        // operation_aborted means we are closing; anything else, keep listening.
        if (ec != asio::error::operation_aborted) {
            doAccept();
        }
        return;
    }

    std::error_code rec;
    const auto remote = sock.remote_endpoint(rec);
    if (!rec) {
        if (PtpLink* link = linkForAddress(remote.address())) {
            link->adoptSocket(std::move(sock));
        } else {
            std::cout << "[ptp] listen port " << port_ << ": rejecting connection from "
                      << remote.address().to_string() << " (no configured link)" << std::endl;
            std::error_code cec;
            sock.close(cec);
        }
    }
    doAccept();
}

PtpLink* PtpListener::linkForAddress(const asio::ip::address& addr) const {
    for (const auto& [peerAddr, link] : peers_) {
        if (peerAddr == addr) {
            return link;
        }
    }
    return nullptr;
}

} // namespace cif::ptp
