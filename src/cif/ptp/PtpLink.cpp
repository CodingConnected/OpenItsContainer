#include "cif/ptp/PtpLink.hpp"

#include <asio/connect.hpp>
#include <asio/ip/address.hpp>
#include <asio/post.hpp>
#include <asio/write.hpp>

#include <iostream>
#include <memory>
#include <utility>

namespace cif::ptp {

namespace {
// How often the write pump drains UBER to the socket. Mirrors the CifHost
// UBER drain cadence (a few ms); low enough to feel immediate for koppelsignaal
// traffic, cheap enough to run per link continuously.
constexpr std::chrono::milliseconds DRAIN_INTERVAL{5};

// Max bytes shipped per async_write (bounds the transient write buffer; the
// pump re-fires every DRAIN_INTERVAL so throughput is unaffected).
constexpr std::size_t WRITE_CHUNK = 1024;
} // namespace

PtpLink::PtpLink(PtpLinkConfig cfg, std::size_t ringBytes, asio::io_context& io,
                 std::chrono::milliseconds reconnectInterval)
    : cfg_(std::move(cfg))
    , iber_(ringBytes)
    , uber_(ringBytes)
    , io_(io)
    , socket_(io)
    , reconnectTimer_(io)
    , drainTimer_(io)
    , reconnectInterval_(reconnectInterval) {}

void PtpLink::bringUp() {
    started_.store(true, std::memory_order_relaxed);
    asio::post(io_, [this] {
        if (stopping_) return;
        // The write pump runs for the life of the link in both modes; it only
        // writes while connected. Connect-mode also starts dialing here;
        // listen-mode waits for PtpListener to call adoptSocket().
        armDrainTimer();
        if (cfg_.mode == Mode::Connect) {
            startConnect();
        }
    });
}

void PtpLink::adoptSocket(asio::ip::tcp::socket sock) {
    auto shared = std::make_shared<asio::ip::tcp::socket>(std::move(sock));
    asio::post(io_, [this, shared] {
        if (stopping_) {
            std::error_code ec;
            shared->close(ec);
            return;
        }
        // A reconnecting peer supersedes its stale socket.
        closeSocket();
        socket_ = std::move(*shared);
        connected_.store(true, std::memory_order_relaxed);
        std::error_code opt;
        socket_.set_option(asio::ip::tcp::no_delay(true), opt);
        startRead();
    });
}

void PtpLink::bringDown() {
    asio::post(io_, [this] {
        stopping_ = true;
        // The non-throwing cancel(ec) overload is gone under ASIO_NO_DEPRECATED;
        // the no-arg form can throw, so guard it (a failed cancel is harmless —
        // the handler observes operation_aborted / a closed socket regardless).
        try { reconnectTimer_.cancel(); } catch (...) {}
        try { drainTimer_.cancel(); } catch (...) {}
        closeSocket();
        connected_.store(false, std::memory_order_relaxed);
    });
}

void PtpLink::startConnect() {
    if (stopping_) return;
    std::error_code ec;
    const auto addr = asio::ip::make_address(cfg_.peerIp, ec);
    if (ec) {
        // Config validated the IP format, so this is not expected; degrade to a
        // retry rather than throwing on the io thread.
        std::cout << "[ptp] link " << cfg_.linkId << " bad peerIp '" << cfg_.peerIp
                  << "': " << ec.message() << std::endl;
        scheduleReconnect();
        return;
    }
    const asio::ip::tcp::endpoint endpoint(addr, cfg_.tcpPort);
    socket_.async_connect(endpoint, [this](const std::error_code& e) { onConnect(e); });
}

void PtpLink::onConnect(const std::error_code& ec) {
    if (stopping_) return;
    if (ec) {
        closeSocket();
        scheduleReconnect();
        return;
    }
    connected_.store(true, std::memory_order_relaxed);
    std::error_code opt;
    socket_.set_option(asio::ip::tcp::no_delay(true), opt); // koppelsignalen are small + latency-sensitive
    startRead();
}

void PtpLink::startRead() {
    socket_.async_read_some(asio::buffer(readBuf_),
                            [this](const std::error_code& ec, std::size_t n) { onRead(ec, n); });
}

void PtpLink::onRead(const std::error_code& ec, std::size_t n) {
    if (stopping_) return;
    if (ec) {
        onDisconnect();
        return;
    }
    for (std::size_t i = 0; i < n; ++i) {
        // If the controller is not draining IBER the ring fills; drop the newest
        // byte rather than wedging the socket. The controller's own timers treat
        // the resulting gap as a link fault. (A rare event: IBER is >= 2048 and
        // the controller drains every ~100 ms.)
        (void)iber_.push(readBuf_[i]);
    }
    startRead();
}

void PtpLink::armDrainTimer() {
    if (stopping_) return;
    drainTimer_.expires_after(DRAIN_INTERVAL);
    drainTimer_.async_wait([this](const std::error_code& ec) {
        if (ec || stopping_) return;
        pumpWrites();
        armDrainTimer();
    });
}

void PtpLink::pumpWrites() {
    if (!connected_.load(std::memory_order_relaxed) || writing_) return;
    writeBuf_.clear();
    char c;
    while (writeBuf_.size() < WRITE_CHUNK && uber_.pop(c)) {
        writeBuf_.push_back(c);
    }
    if (writeBuf_.empty()) return;
    writing_ = true;
    asio::async_write(socket_, asio::buffer(writeBuf_),
                      [this](const std::error_code& ec, std::size_t n) { onWrite(ec, n); });
}

void PtpLink::onWrite(const std::error_code& ec, std::size_t /*n*/) {
    writing_ = false;
    if (stopping_) return;
    if (ec) {
        onDisconnect();
    }
}

void PtpLink::onDisconnect() {
    connected_.store(false, std::memory_order_relaxed);
    closeSocket();
    // Connect-mode re-dials; listen-mode simply waits for PtpListener to adopt
    // the peer's next connection (the acceptor keeps accepting).
    if (cfg_.mode == Mode::Connect) {
        scheduleReconnect();
    }
}

void PtpLink::scheduleReconnect() {
    if (stopping_) return;
    reconnectTimer_.expires_after(reconnectInterval_);
    reconnectTimer_.async_wait([this](const std::error_code& ec) {
        if (ec || stopping_) return;
        startConnect();
    });
}

void PtpLink::closeSocket() {
    std::error_code ec;
    socket_.close(ec); // a closed socket is reopened by the next async_connect
}

} // namespace cif::ptp
