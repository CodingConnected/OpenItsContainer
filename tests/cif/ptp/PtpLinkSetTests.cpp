#include "cif/ptp/PtpLinkSet.hpp"
#include "cif/ptp/PtpConfig.hpp"

#include <asio/io_context.hpp>
#include <gtest/gtest.h>

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using cif::ptp::Mode;
using cif::ptp::PtpLink;
using cif::ptp::PtpLinkConfig;
using cif::ptp::PtpLinkSet;

namespace {

PtpLinkConfig makeLink(int linkId, Mode mode, std::string peerIp = "10.0.0.1") {
    PtpLinkConfig c;
    c.linkId = linkId;
    c.mode = mode;
    c.peerIp = std::move(peerIp);
    return c;
}

// These tests exercise byte routing only; the io_context is never run, so no
// sockets are opened by bringUp() and the rings are driven directly.
} // namespace

TEST(PtpLinkSet, ConstructsConfiguredLinksOnly) {
    asio::io_context io;
    PtpLinkSet set({makeLink(1, Mode::Connect), makeLink(3, Mode::Listen)}, io);
    EXPECT_EQ(set.linkCount(), 2U);
    EXPECT_NE(set.link(1), nullptr);
    EXPECT_EQ(set.link(2), nullptr); // not configured
    EXPECT_NE(set.link(3), nullptr);
    EXPECT_EQ(set.link(1)->config().mode, Mode::Connect);
    EXPECT_EQ(set.link(3)->config().mode, Mode::Listen);
}

TEST(PtpLinkSet, RingsMeetMinimumSize) {
    asio::io_context io;
    PtpLinkSet set({makeLink(1, Mode::Connect)}, io,
                   PtpLinkSet::DEFAULT_RECONNECT_INTERVAL, /*ringBytes*/ 10);
    // Requested 10 is raised to the spec minimum (2048).
    EXPECT_GE(set.link(1)->iber().capacity(), cif::ptp::MIN_RING_BYTES);
    EXPECT_GE(set.link(1)->uber().capacity(), cif::ptp::MIN_RING_BYTES);
}

TEST(PtpLinkSet, UnknownLinkIdIsGraceful) {
    asio::io_context io;
    PtpLinkSet set({makeLink(1, Mode::Connect)}, io);
    // Unconfigured linkIds must never crash — they read empty and drop writes.
    EXPECT_EQ(set.readChar(0), PtpLinkSet::EOB);
    EXPECT_EQ(set.readChar(2), PtpLinkSet::EOB);
    EXPECT_EQ(set.readChar(99), PtpLinkSet::EOB);
    EXPECT_EQ(set.busy(2), 0);
    EXPECT_FALSE(set.writeChar(2, 'x'));
    set.initIber(2); // no-op, no crash
    set.initUber(2);
}

TEST(PtpLinkSet, ArbitraryLinkIdRoutes) {
    asio::io_context io;
    // linkId is engineer-chosen, not a 1..4 slot: a large id must route normally.
    PtpLinkSet set({makeLink(7033, Mode::Connect), makeLink(0, Mode::Listen)}, io);
    EXPECT_EQ(set.linkCount(), 2U);
    ASSERT_NE(set.link(7033), nullptr);
    ASSERT_NE(set.link(0), nullptr);
    ASSERT_TRUE(set.writeChar(7033, 'Z'));
    char c;
    ASSERT_TRUE(set.link(7033)->uber().pop(c));
    EXPECT_EQ(c, 'Z');
    // Byte on 7033 must not leak to link 0.
    EXPECT_TRUE(set.link(0)->uber().empty());
}

TEST(PtpLinkSet, UnknownLinkIdWarnsOnce) {
    asio::io_context io;
    PtpLinkSet set({makeLink(1, Mode::Connect)}, io);
    std::ostringstream captured;
    std::streambuf* old = std::cerr.rdbuf(captured.rdbuf());
    set.readChar(42); // unknown -> warns
    set.readChar(42); // unknown again -> silent
    set.writeChar(42, 'x'); // still silent for the same id
    std::cerr.rdbuf(old);
    const std::string out = captured.str();
    // Exactly one warning line for id 42.
    EXPECT_NE(out.find("42"), std::string::npos);
    EXPECT_EQ(out.find("42"), out.rfind("42"));
}

TEST(PtpLinkSet, WriteCharGoesToUberRing) {
    asio::io_context io;
    PtpLinkSet set({makeLink(1, Mode::Connect)}, io);
    ASSERT_TRUE(set.writeChar(1, 'H'));
    ASSERT_TRUE(set.writeChar(1, 'i'));
    char c;
    ASSERT_TRUE(set.link(1)->uber().pop(c)); EXPECT_EQ(c, 'H');
    ASSERT_TRUE(set.link(1)->uber().pop(c)); EXPECT_EQ(c, 'i');
    EXPECT_TRUE(set.link(1)->uber().empty());
}

TEST(PtpLinkSet, ReadCharComesFromIberRing) {
    asio::io_context io;
    PtpLinkSet set({makeLink(1, Mode::Connect)}, io);
    EXPECT_EQ(set.busy(1), 0);
    EXPECT_EQ(set.readChar(1), PtpLinkSet::EOB); // empty

    // Simulate the io thread filling IBER.
    ASSERT_TRUE(set.link(1)->iber().push('A'));
    EXPECT_EQ(set.busy(1), 1);
    EXPECT_EQ(set.readChar(1), static_cast<int>('A'));
    EXPECT_EQ(set.busy(1), 0);
    EXPECT_EQ(set.readChar(1), PtpLinkSet::EOB);
}

TEST(PtpLinkSet, DataByte0xFFIsNotConfusedWithEob) {
    asio::io_context io;
    PtpLinkSet set({makeLink(1, Mode::Connect)}, io);
    ASSERT_TRUE(set.link(1)->iber().push(static_cast<char>(0xFF)));
    EXPECT_EQ(set.busy(1), 1);
    // A real 0xFF byte reads back as 255, distinct from EOB (-1).
    EXPECT_EQ(set.readChar(1), 255);
    EXPECT_EQ(set.readChar(1), PtpLinkSet::EOB);
}

TEST(PtpLinkSet, InitResetsRings) {
    asio::io_context io;
    PtpLinkSet set({makeLink(1, Mode::Connect)}, io);
    set.link(1)->iber().push('x');
    set.writeChar(1, 'y');
    set.initIber(1);
    set.initUber(1);
    EXPECT_EQ(set.busy(1), 0);
    EXPECT_TRUE(set.link(1)->uber().empty());
}

TEST(PtpLinkSet, RoutingIsPerPortnr) {
    asio::io_context io;
    PtpLinkSet set({makeLink(1, Mode::Connect), makeLink(2, Mode::Listen)}, io);
    // A byte written to link 1 must not appear on link 2, and vice versa.
    ASSERT_TRUE(set.writeChar(1, '1'));
    ASSERT_TRUE(set.writeChar(2, '2'));
    char c;
    ASSERT_TRUE(set.link(1)->uber().pop(c)); EXPECT_EQ(c, '1');
    EXPECT_TRUE(set.link(1)->uber().empty());
    ASSERT_TRUE(set.link(2)->uber().pop(c)); EXPECT_EQ(c, '2');

    set.link(1)->iber().push('a');
    EXPECT_EQ(set.busy(2), 0); // link 2's incoming is independent
    EXPECT_EQ(set.readChar(1), static_cast<int>('a'));
}

TEST(PtpLinkSet, BringUpMarksLinksStarted) {
    asio::io_context io;
    PtpLinkSet set({makeLink(1, Mode::Connect), makeLink(2, Mode::Connect)}, io);
    EXPECT_FALSE(set.link(1)->isStarted());
    set.bringUpAll();
    EXPECT_TRUE(set.link(1)->isStarted());
    EXPECT_TRUE(set.link(2)->isStarted());
    // The io_context is never run here, so no connection is established.
    EXPECT_FALSE(set.link(1)->isConnected());
    set.bringDownAll(); // posts teardown; must not crash even with io unrun
}

TEST(PtpLinkSet, ConstructionRejectsInvalidConfig) {
    asio::io_context io;
    // Duplicate linkId.
    EXPECT_THROW(PtpLinkSet({makeLink(1, Mode::Connect), makeLink(1, Mode::Listen)}, io),
                 std::runtime_error);
    // More than MAX_LINKS links.
    EXPECT_THROW(PtpLinkSet({makeLink(1, Mode::Connect), makeLink(2, Mode::Connect),
                             makeLink(3, Mode::Connect), makeLink(4, Mode::Connect),
                             makeLink(5, Mode::Connect)},
                            io),
                 std::runtime_error);
    // Empty peerIp.
    EXPECT_THROW(PtpLinkSet({makeLink(1, Mode::Listen, "")}, io), std::runtime_error);
}
