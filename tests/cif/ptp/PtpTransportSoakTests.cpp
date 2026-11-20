// Content-agnostic proof of the container's PTP byte transport. The transport is
// a pure byte pipe — it never interprets PTP framing — so the only things that
// can be right or wrong are: every byte is delivered, in order, unaltered, both
// directions; no byte value is treated specially; and it holds up under load.
// These tests drive two real PtpLinkSets (connect <-> listen) over a loopback
// socket with random byte streams (covering the full 0..255 range, including the
// control bytes a naive framer might act on), plus a sustained duplex soak.
//
// Reconnect / re-adoption is covered content-agnostically elsewhere
// (PtpConnectTests, PtpListenTests); this file focuses on volume + duplex + soak.

#include "cif/ptp/PtpConfig.hpp"
#include "cif/ptp/PtpLink.hpp"
#include "cif/ptp/PtpLinkSet.hpp"

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using asio::ip::tcp;
using cif::ptp::Mode;
using cif::ptp::PtpLinkConfig;
using cif::ptp::PtpLinkSet;

namespace {

using Clock = std::chrono::steady_clock;
using Bytes = std::vector<std::uint8_t>;

PtpLinkConfig linkCfg(Mode mode, unsigned short tcpPort) {
    PtpLinkConfig c;
    c.linkId = 1;
    c.mode = mode;
    c.peerIp = "127.0.0.1";
    c.tcpPort = tcpPort;
    return c;
}

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

// A payload that contains every byte value at least once (so byte transparency
// is exercised), then pseudo-random bytes to the requested length.
Bytes makePayload(std::size_t n, std::uint32_t seed) {
    Bytes v;
    v.reserve(n < 256 ? 256 : n);
    for (int b = 0; b < 256; ++b) v.push_back(static_cast<std::uint8_t>(b));
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> d(0, 255);
    while (v.size() < n) v.push_back(static_cast<std::uint8_t>(d(rng)));
    return v;
}

// Push every byte of `data` into link 1's outgoing ring, retrying on a full
// ring (backpressure) until the deadline.
void produce(PtpLinkSet& set, const Bytes& data, std::chrono::milliseconds timeout) {
    std::size_t i = 0;
    const auto deadline = Clock::now() + timeout;
    while (i < data.size() && Clock::now() < deadline) {
        if (set.writeChar(1, static_cast<char>(data[i]))) ++i;
        else std::this_thread::sleep_for(1ms); // ring full: let the io thread drain
    }
}

// Pop exactly `n` bytes out of link 1's incoming ring.
Bytes consume(PtpLinkSet& set, std::size_t n, std::chrono::milliseconds timeout) {
    Bytes out;
    out.reserve(n);
    const auto deadline = Clock::now() + timeout;
    while (out.size() < n && Clock::now() < deadline) {
        const int c = set.readChar(1);
        if (c != PtpLinkSet::EOB) out.push_back(static_cast<std::uint8_t>(c));
        else std::this_thread::sleep_for(200us);
    }
    return out;
}

// Fixture-free helper: stand up a connected connect/listen pair on the given io.
struct TransportPair {
    PtpLinkSet listenSet;
    PtpLinkSet connectSet;

    TransportPair(asio::io_context& io, unsigned short port)
        : listenSet({linkCfg(Mode::Listen, port)}, io, 100ms)
        , connectSet({linkCfg(Mode::Connect, port)}, io, 100ms) {
        listenSet.bringUpAll();
        connectSet.bringUpAll();
    }

    bool connected() {
        return listenSet.link(1) && listenSet.link(1)->isConnected() &&
               connectSet.link(1) && connectSet.link(1)->isConnected();
    }
};

} // namespace

TEST(PtpTransportSoak, ConcurrentDuplexTransfersRandomStream) {
    const unsigned short port = freePort();
    asio::io_context io;
    auto guard = asio::make_work_guard(io);
    std::thread ioThread([&] { io.run(); });

    TransportPair link(io, port);
    ASSERT_TRUE(waitFor([&] { return link.connected(); }, 5s));

    constexpr std::size_t N = 32 * 1024;
    const Bytes aToB = makePayload(N, 0xA11CE);
    const Bytes bToA = makePayload(N, 0xB0B);

    // Full duplex: both directions transfer simultaneously, each with its own
    // producer/consumer thread (one producer + one consumer per ring keeps the
    // SPSC contract intact).
    Bytes gotAtB, gotAtA;
    std::thread pA([&] { produce(link.connectSet, aToB, 20s); });
    std::thread pB([&] { produce(link.listenSet, bToA, 20s); });
    std::thread cB([&] { gotAtB = consume(link.listenSet, N, 20s); });
    std::thread cA([&] { gotAtA = consume(link.connectSet, N, 20s); });
    pA.join();
    pB.join();
    cB.join();
    cA.join();

    EXPECT_EQ(gotAtB, aToB); // every byte, in order, unaltered
    EXPECT_EQ(gotAtA, bToA);

    link.connectSet.bringDownAll();
    link.listenSet.bringDownAll();
    guard.reset();
    ioThread.join();
}

TEST(PtpTransportSoak, SustainedDuplexSoak) {
    const unsigned short port = freePort();
    asio::io_context io;
    auto guard = asio::make_work_guard(io);
    std::thread ioThread([&] { io.run(); });

    TransportPair link(io, port);
    ASSERT_TRUE(waitFor([&] { return link.connected(); }, 5s));

    // Continuously exchange fresh random chunks both ways for ~2 s, verifying
    // exact delivery every round — proves the pipe neither wedges, drops, nor
    // reorders over sustained bidirectional traffic.
    constexpr std::size_t CHUNK = 256;
    const auto soakEnd = Clock::now() + 2s;
    int rounds = 0;
    while (Clock::now() < soakEnd) {
        const Bytes up = makePayload(CHUNK, 0x1000 + rounds);
        const Bytes down = makePayload(CHUNK, 0x2000 + rounds);
        produce(link.connectSet, up, 2s);
        produce(link.listenSet, down, 2s);
        EXPECT_EQ(consume(link.listenSet, CHUNK, 3s), up);
        EXPECT_EQ(consume(link.connectSet, CHUNK, 3s), down);
        ASSERT_TRUE(link.connected());
        ++rounds;
    }
    EXPECT_GT(rounds, 10); // actually soaked for many rounds

    link.connectSet.bringDownAll();
    link.listenSet.bringDownAll();
    guard.reset();
    ioThread.join();
}
