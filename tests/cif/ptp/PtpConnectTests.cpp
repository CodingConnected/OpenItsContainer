// Integration test for PTP connect-mode (Phase 2): a PtpLink dials a mock TCP
// listener, shuttles bytes both ways transparently (including 0x16/0x01/0xFF, to
// prove we never interpret PTP framing), and reconnects after the peer drops.

#include "cif/ptp/PtpConfig.hpp"
#include "cif/ptp/PtpLink.hpp"

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using asio::ip::tcp;
using cif::ptp::Mode;
using cif::ptp::PtpLink;
using cif::ptp::PtpLinkConfig;

namespace {

using Clock = std::chrono::steady_clock;

PtpLinkConfig connectCfg(unsigned short port) {
    PtpLinkConfig c;
    c.linkId = 1;
    c.mode = Mode::Connect;
    c.peerIp = "127.0.0.1";
    c.tcpPort = port;
    return c;
}

template <class Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

// Non-blocking accept with a deadline (the io_context for `acc` is never run).
tcp::socket acceptWithDeadline(tcp::acceptor& acc, asio::io_context& io,
                               std::chrono::milliseconds timeout) {
    acc.non_blocking(true);
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        tcp::socket s(io);
        std::error_code ec;
        acc.accept(s, ec);
        if (!ec) {
            s.non_blocking(true);
            return s;
        }
        std::this_thread::sleep_for(2ms);
    }
    throw std::runtime_error("acceptWithDeadline: timed out");
}

std::string readN(tcp::socket& s, std::size_t n, std::chrono::milliseconds timeout) {
    std::string out;
    char buf[256];
    const auto deadline = Clock::now() + timeout;
    while (out.size() < n && Clock::now() < deadline) {
        std::error_code ec;
        const std::size_t got =
            s.read_some(asio::buffer(buf, std::min(sizeof buf, n - out.size())), ec);
        if (!ec) {
            out.append(buf, got);
        } else if (ec == asio::error::would_block || ec == asio::error::try_again) {
            std::this_thread::sleep_for(2ms);
        } else {
            break; // peer closed / error
        }
    }
    return out;
}

void writeAll(tcp::socket& s, const std::string& data) {
    std::size_t sent = 0;
    const auto deadline = Clock::now() + 2s;
    while (sent < data.size() && Clock::now() < deadline) {
        std::error_code ec;
        sent += s.write_some(asio::buffer(data.data() + sent, data.size() - sent), ec);
        if (ec == asio::error::would_block || ec == asio::error::try_again) {
            std::this_thread::sleep_for(1ms);
        } else if (ec) {
            break;
        }
    }
}

// Pop exactly n bytes out of the link's IBER ring (the control-thread side).
std::string drainIber(PtpLink& link, std::size_t n, std::chrono::milliseconds timeout) {
    std::string out;
    const auto deadline = Clock::now() + timeout;
    char c;
    while (out.size() < n && Clock::now() < deadline) {
        if (link.iber().pop(c)) {
            out.push_back(c);
        } else {
            std::this_thread::sleep_for(2ms);
        }
    }
    return out;
}

// A byte string containing PTP control bytes to prove transparency.
std::string transparentPayload() {
    std::string s;
    s.push_back(static_cast<char>(0x16)); // SYN
    s.push_back(static_cast<char>(0x01)); // SOH
    s.push_back(static_cast<char>(0xFF)); // EOB-lookalike
    s.push_back(static_cast<char>(0x00)); // NUL
    s += "world";
    return s;
}

} // namespace

TEST(PtpConnect, DialsListenerAndShuttlesBytesBothWays) {
    asio::io_context srvIo;
    tcp::acceptor acceptor(srvIo, tcp::endpoint(tcp::v4(), 0));
    const unsigned short port = acceptor.local_endpoint().port();

    asio::io_context linkIo;
    auto guard = asio::make_work_guard(linkIo);
    std::thread linkThread([&] { linkIo.run(); });

    PtpLink link(connectCfg(port), cif::ptp::MIN_RING_BYTES, linkIo, 100ms);
    link.bringUp();

    tcp::socket peer = acceptWithDeadline(acceptor, srvIo, 3s);
    ASSERT_TRUE(waitFor([&] { return link.isConnected(); }, 3s));

    // controller -> peer (UBER drained to the socket by the write pump).
    const std::string outbound = "HELLO";
    for (char c : outbound) ASSERT_TRUE(link.uber().push(c));
    EXPECT_EQ(readN(peer, outbound.size(), 3s), outbound);

    // peer -> controller (socket read pushed into IBER), byte-transparent.
    const std::string inbound = transparentPayload();
    writeAll(peer, inbound);
    EXPECT_EQ(drainIber(link, inbound.size(), 3s), inbound);

    link.bringDown();
    guard.reset();
    linkThread.join();
}

TEST(PtpConnect, ReconnectsAfterPeerDrops) {
    asio::io_context srvIo;
    tcp::acceptor acceptor(srvIo, tcp::endpoint(tcp::v4(), 0));
    const unsigned short port = acceptor.local_endpoint().port();

    asio::io_context linkIo;
    auto guard = asio::make_work_guard(linkIo);
    std::thread linkThread([&] { linkIo.run(); });

    PtpLink link(connectCfg(port), cif::ptp::MIN_RING_BYTES, linkIo, /*reconnect*/ 100ms);
    link.bringUp();

    {
        tcp::socket peer = acceptWithDeadline(acceptor, srvIo, 3s);
        ASSERT_TRUE(waitFor([&] { return link.isConnected(); }, 3s));
        // Drop the peer; the link must notice and go not-connected.
        peer.close();
        ASSERT_TRUE(waitFor([&] { return !link.isConnected(); }, 3s));
    }

    // The link retries every 100 ms; a fresh accept must succeed and flow resume.
    tcp::socket peer2 = acceptWithDeadline(acceptor, srvIo, 3s);
    ASSERT_TRUE(waitFor([&] { return link.isConnected(); }, 3s));

    const std::string again = "AGAIN";
    for (char c : again) ASSERT_TRUE(link.uber().push(c));
    EXPECT_EQ(readN(peer2, again.size(), 3s), again);

    link.bringDown();
    guard.reset();
    linkThread.join();
}

// Like ReconnectsAfterPeerDrops, but the peer drops *after* a successful
// bidirectional exchange — the reconnect path must recover a link that was
// already up and passing traffic, and resume a full exchange afterwards.
TEST(PtpConnect, ReconnectsAndResumesAfterPriorExchange) {
    asio::io_context srvIo;
    tcp::acceptor acceptor(srvIo, tcp::endpoint(tcp::v4(), 0));
    const unsigned short port = acceptor.local_endpoint().port();

    asio::io_context linkIo;
    auto guard = asio::make_work_guard(linkIo);
    std::thread linkThread([&] { linkIo.run(); });

    PtpLink link(connectCfg(port), cif::ptp::MIN_RING_BYTES, linkIo, /*reconnect*/ 100ms);
    link.bringUp();

    // First connection: exchange bytes both ways, THEN drop the peer.
    {
        tcp::socket peer = acceptWithDeadline(acceptor, srvIo, 3s);
        ASSERT_TRUE(waitFor([&] { return link.isConnected(); }, 3s));

        const std::string out1 = "FIRST";
        for (char c : out1) ASSERT_TRUE(link.uber().push(c));
        EXPECT_EQ(readN(peer, out1.size(), 3s), out1);

        const std::string in1 = transparentPayload();
        writeAll(peer, in1);
        EXPECT_EQ(drainIber(link, in1.size(), 3s), in1);

        peer.close();
        ASSERT_TRUE(waitFor([&] { return !link.isConnected(); }, 3s));
    }

    // The retry timer re-dials; a fresh accept succeeds and a full exchange
    // resumes on the new connection.
    tcp::socket peer2 = acceptWithDeadline(acceptor, srvIo, 3s);
    ASSERT_TRUE(waitFor([&] { return link.isConnected(); }, 3s));

    const std::string out2 = "SECOND";
    for (char c : out2) ASSERT_TRUE(link.uber().push(c));
    EXPECT_EQ(readN(peer2, out2.size(), 3s), out2);

    const std::string in2 = "resumed";
    writeAll(peer2, in2);
    EXPECT_EQ(drainIber(link, in2.size(), 3s), in2);

    link.bringDown();
    guard.reset();
    linkThread.join();
}
