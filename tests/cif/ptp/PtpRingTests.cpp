#include "cif/ptp/PtpRing.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using cif::ptp::PtpRing;

TEST(PtpRing, EmptyInitially) {
    PtpRing ring(16);
    EXPECT_TRUE(ring.empty());
    char c;
    EXPECT_FALSE(ring.pop(c));
}

TEST(PtpRing, PushPopFifoOrder) {
    PtpRing ring(16);
    ASSERT_TRUE(ring.push('a'));
    ASSERT_TRUE(ring.push('b'));
    ASSERT_TRUE(ring.push('c'));
    EXPECT_FALSE(ring.empty());

    char c;
    ASSERT_TRUE(ring.pop(c)); EXPECT_EQ(c, 'a');
    ASSERT_TRUE(ring.pop(c)); EXPECT_EQ(c, 'b');
    ASSERT_TRUE(ring.pop(c)); EXPECT_EQ(c, 'c');
    EXPECT_TRUE(ring.empty());
}

TEST(PtpRing, CapacityHonoredAndReportsFull) {
    PtpRing ring(4);
    EXPECT_EQ(ring.capacity(), 4U);
    // Exactly `capacity` bytes fit; the next push fails.
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(ring.push(static_cast<char>('0' + i)));
    }
    EXPECT_FALSE(ring.push('x')); // full, dropped
    // Draining lets writes resume.
    char c;
    ASSERT_TRUE(ring.pop(c));
    EXPECT_EQ(c, '0');
    EXPECT_TRUE(ring.push('x'));
}

TEST(PtpRing, WrapsAroundRepeatedly) {
    PtpRing ring(4);
    char c;
    // Many push/pop cycles must wrap the indices without corruption.
    for (int i = 0; i < 1000; ++i) {
        const char v = static_cast<char>(i & 0x7F);
        ASSERT_TRUE(ring.push(v));
        ASSERT_TRUE(ring.pop(c));
        EXPECT_EQ(c, v);
    }
    EXPECT_TRUE(ring.empty());
}

TEST(PtpRing, ResetDiscardsContents) {
    PtpRing ring(16);
    ring.push('a');
    ring.push('b');
    ring.reset();
    EXPECT_TRUE(ring.empty());
    char c;
    EXPECT_FALSE(ring.pop(c));
}

TEST(PtpRing, RoundTripsAllByteValues) {
    PtpRing ring(300);
    for (int v = 0; v <= 255; ++v) {
        ASSERT_TRUE(ring.push(static_cast<char>(v)));
    }
    for (int v = 0; v <= 255; ++v) {
        char c;
        ASSERT_TRUE(ring.pop(c));
        // Compare as unsigned so 0x80..0xFF (incl. the EOB-lookalike 0xFF) match.
        EXPECT_EQ(static_cast<unsigned char>(c), static_cast<unsigned char>(v));
    }
}

// Single-producer / single-consumer stress: one thread pushes a long known
// sequence (with spin-retry on full), another pops and verifies exact order.
TEST(PtpRing, SpscThreadedIntegrity) {
    PtpRing ring(64); // small, so both full and empty are hit constantly
    constexpr int N = 200000;

    std::thread producer([&] {
        for (int i = 0; i < N; ++i) {
            const char v = static_cast<char>(i & 0xFF);
            while (!ring.push(v)) {
                std::this_thread::yield();
            }
        }
    });

    int mismatches = 0;
    std::thread consumer([&] {
        for (int i = 0; i < N; ++i) {
            char c;
            while (!ring.pop(c)) {
                std::this_thread::yield();
            }
            if (static_cast<unsigned char>(c) != static_cast<unsigned char>(i & 0xFF)) {
                ++mismatches;
            }
        }
    });

    producer.join();
    consumer.join();
    EXPECT_EQ(mismatches, 0);
    EXPECT_TRUE(ring.empty());
}
