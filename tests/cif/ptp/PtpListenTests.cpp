// Integration test for PTP listen-mode (Phase 3): a PtpLinkSet accepts on a
// port and demultiplexes by peer IP. A mock dialer connects; bytes flow both
// ways; a connection from an unconfigured peer is rejected; a dropped peer is
// re-adopted on reconnect.

#include "cif/ptp/PtpConfig.hpp"
#include "cif/ptp/PtpLink.hpp"
#include "cif/ptp/PtpLinkSet.hpp"

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using asio::ip::tcp;
using cif::ptp::Mode;
using cif::ptp::PtpLinkConfig;
using cif::ptp::PtpLinkSet;

namespace {

using Clock = std::chrono::steady_clock;

PtpLinkConfig listenCfg(int linkId, std::string peerIp, unsigned short tcpPort) {
    PtpLinkConfig c;
    c.linkId = linkId;
    c.mode = Mode::Listen;
    c.peerIp = std::move(peerIp);
    c.tcpPort = tcpPort;
    return c;
}

// Grab an ephemeral port by opening then closing a throwaway acceptor.
unsigned short freePort() {
    asio::io_context io;
    tcp::acceptor a(io, tcp::endpoint(tcp::v4(), 0));
    const unsigned short p = a.local_endpoint().port();
    a.close();
    return p;
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

// The listener binds asynchronously (start() posts), so retry the dial briefly.
void connectWithRetry(tcp::socket& s, unsigned short port, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    const tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), port);
    while (Clock::now() < deadline) {
        std::error_code ec;
        s.connect(ep, ec);
        if (!ec) {
            s.non_blocking(true);
            return;
        }
        s.close();
        std::this_thread::sleep_for(20ms);
    }
    throw std::runtime_error("connectWithRetry: timed out");
}

std::string readN(tcp::socket& s, std::size_t n, std::chrono::milliseconds timeout) {
    std::string out;
    char buf[256];
    const auto deadline = Clock::now() + timeout;
    while (out.size() < n && Clock::now() < deadline) {
        std::error_code ec;
        const std::size_t got =
            s.read_some(asio::buffer(buf, std::min(sizeof buf, n - out.size())), ec);
        if (!ec) out.append(buf, got);
        else if (ec == asio::error::would_block || ec == asio::error::try_again)
            std::this_thread::sleep_for(2ms);
        else break;
    }
    return out;
}

void writeAll(tcp::socket& s, const std::string& data) {
    std::size_t sent = 0;
    const auto deadline = Clock::now() + 2s;
    while (sent < data.size() && Clock::now() < deadline) {
        std::error_code ec;
        sent += s.write_some(asio::buffer(data.data() + sent, data.size() - sent), ec);
        if (ec == asio::error::would_block || ec == asio::error::try_again)
            std::this_thread::sleep_for(1ms);
        else if (ec) break;
    }
}

std::string drainIber(cif::ptp::PtpLink& link, std::size_t n, std::chrono::milliseconds timeout) {
    std::string out;
    const auto deadline = Clock::now() + timeout;
    char c;
    while (out.size() < n && Clock::now() < deadline) {
        if (link.iber().pop(c)) out.push_back(c);
        else std::this_thread::sleep_for(2ms);
    }
    return out;
}

// True if the socket observes the peer having closed within the timeout.
bool sawClose(tcp::socket& s, std::chrono::milliseconds timeout) {
    char buf[64];
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        std::error_code ec;
        const std::size_t n = s.read_some(asio::buffer(buf), ec);
        if (ec == asio::error::eof || ec == asio::error::connection_reset) return true;
        if (!ec && n == 0) return true;
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

} // namespace

TEST(PtpListen, AcceptsMatchingPeerAndShuttlesBytes) {
    const unsigned short port = freePort();

    asio::io_context linkIo;
    auto guard = asio::make_work_guard(linkIo);
    std::thread linkThread([&] { linkIo.run(); });

    PtpLinkSet set({listenCfg(1, "127.0.0.1", port)}, linkIo, 100ms);
    set.bringUpAll();

    asio::io_context cliIo;
    tcp::socket client(cliIo);
    connectWithRetry(client, port, 3s);
    ASSERT_TRUE(waitFor([&] { return set.link(1) && set.link(1)->isConnected(); }, 3s));

    // peer -> controller
    const std::string inbound = "koppel";
    writeAll(client, inbound);
    EXPECT_EQ(drainIber(*set.link(1), inbound.size(), 3s), inbound);

    // controller -> peer
    const std::string outbound = "PONG";
    for (char c : outbound) ASSERT_TRUE(set.writeChar(1, c));
    EXPECT_EQ(readN(client, outbound.size(), 3s), outbound);

    set.bringDownAll();
    guard.reset();
    linkThread.join();
}

TEST(PtpListen, RejectsUnknownPeer) {
    const unsigned short port = freePort();

    asio::io_context linkIo;
    auto guard = asio::make_work_guard(linkIo);
    std::thread linkThread([&] { linkIo.run(); });

    // The only listen link expects a peer that is not loopback, so a connection
    // from 127.0.0.1 must be rejected (accepted then immediately closed).
    PtpLinkSet set({listenCfg(1, "10.0.0.123", port)}, linkIo, 100ms);
    set.bringUpAll();

    asio::io_context cliIo;
    tcp::socket client(cliIo);
    connectWithRetry(client, port, 3s);

    EXPECT_TRUE(sawClose(client, 3s));
    EXPECT_FALSE(set.link(1)->isConnected());

    set.bringDownAll();
    guard.reset();
    linkThread.join();
}

TEST(PtpListen, ReAdoptsPeerAfterReconnect) {
    const unsigned short port = freePort();

    asio::io_context linkIo;
    auto guard = asio::make_work_guard(linkIo);
    std::thread linkThread([&] { linkIo.run(); });

    PtpLinkSet set({listenCfg(1, "127.0.0.1", port)}, linkIo, 100ms);
    set.bringUpAll();

    {
        asio::io_context cliIo;
        tcp::socket client(cliIo);
        connectWithRetry(client, port, 3s);
        ASSERT_TRUE(waitFor([&] { return set.link(1)->isConnected(); }, 3s));
        client.close();
        ASSERT_TRUE(waitFor([&] { return !set.link(1)->isConnected(); }, 3s));
    }

    // A fresh connection from the same peer must be adopted and flow resume.
    asio::io_context cliIo2;
    tcp::socket client2(cliIo2);
    connectWithRetry(client2, port, 3s);
    ASSERT_TRUE(waitFor([&] { return set.link(1)->isConnected(); }, 3s));

    const std::string again = "AGAIN";
    for (char c : again) ASSERT_TRUE(set.writeChar(1, c));
    EXPECT_EQ(readN(client2, again.size(), 3s), again);

    set.bringDownAll();
    guard.reset();
    linkThread.join();
}
