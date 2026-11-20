#include <gtest/gtest.h>

#include "cif/ActuatedController.hpp"
#include "cif/CifBridge.hpp"
#include "cif/CifHost.hpp"
#include "fi/FiControllerApp.hpp"
#include "fi/FiFacilitiesServer.hpp"

#include <asio/io_context.hpp>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace tlcfi::fi;
using namespace tlcfi::tlcfi;

// CIF buffers are defined in ActuatedController.cpp (via CIF_PUBLIC).
// We access them here via extern declarations from CIF_INC.h.
#include "cif/CIF_INC.h"

namespace {

// JSON configuration for a simple 2-signal-group, 2-detector intersection
const char* TEST_CONFIG_JSON = R"({
    "signalGroups": [
        {
            "id": "SG1",
            "index": 0,
            "minGreen": 30,
            "maxGreen": 100,
            "amber": 30,
            "minRed": 0,
            "detectors": ["DET1"]
        },
        {
            "id": "SG2",
            "index": 1,
            "minGreen": 30,
            "maxGreen": 100,
            "amber": 30,
            "minRed": 0,
            "detectors": ["DET1"]
        }
    ],
    "detectors": [
        { "id": "DET1", "index": 0 }
    ],
    "inputs": [
        { "id": "IN1", "index": 0 }
    ],
    "outputs": [
        { "id": "OUT1", "index": 0 },
        { "id": "OUT2", "index": 1 }
    ],
    "conflicts": [
        { "from": "SG1", "to": "SG2", "intergreen": 30 },
        { "from": "SG2", "to": "SG1", "intergreen": 30 }
    ]
})";

struct CifTestFixture {
    asio::io_context io;
    std::unique_ptr<FiFacilitiesServer> server;
    std::thread ioThread;
    unsigned short port;

    // Pass 0 (the default) to bind an OS-assigned ephemeral port — most tests
    // use this and read the assigned port via `port`. Pass a specific port for
    // tests that need server-port stability across multiple fixture lifetimes
    // (e.g. CifHostTests.ReconnectionResetsAndResumes, where the FiControllerApp
    // outlives the first fixture and must reconnect to the same address).
    explicit CifTestFixture(unsigned short requestedPort = 0)
    {
        server = std::make_unique<FiFacilitiesServer>(io, requestedPort);
        port = server->port();
        server->start();
        ioThread = std::thread([this]() { io.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ~CifTestFixture() {
        server->stop();
        io.stop();
        if (ioThread.joinable()) {
            ioThread.join();
        }
    }

    template<typename Predicate>
    bool waitFor(Predicate&& pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        auto start = std::chrono::steady_clock::now();
        while (!pred()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (std::chrono::steady_clock::now() - start > timeout) {
                return false;
            }
        }
        return true;
    }
};

// Writes `payload` into the CIF_FILE_UBER ring and sets a FILEWISSEL marker at
// the start so that the first pollFileVlog() call triggers a file rotation.
void setupVlogRingForRotation(const std::string& payload) {
    const int fileBufferSize = 16384;
    CIF_FILE_UBER_LEES       = CIF_FILE_UBER;
    CIF_FILE_UBER_SCHRIJF    = CIF_FILE_UBER;
    CIF_FILE_UBER_FILEWISSEL = CIF_FILE_UBER;
    CIF_PB_FILE_UBER_FILEWISSEL = 1;
    CIF_FILE_UBER_STATUS     = 1;
    s_int8* writePtr = CIF_FILE_UBER_SCHRIJF;
    for (char c : payload) {
        *writePtr = static_cast<s_int8>(c);
        ++writePtr;
        if (writePtr >= (CIF_FILE_UBER + fileBufferSize))
            writePtr = CIF_FILE_UBER;
    }
    CIF_FILE_UBER_SCHRIJF = writePtr;
}

} // namespace

// ============================================================================
// 1. ACTUATED CONTROLLER UNIT TESTS (no TLC-FI, just CIF buffers)
// ============================================================================

TEST(ActuatedControllerTests, LoadConfigFromJson) {
    auto config = cif::loadConfigFromJson(TEST_CONFIG_JSON);
    EXPECT_EQ(config.signalGroups.size(), 2);
    EXPECT_EQ(config.detectors.size(), 1);
    EXPECT_EQ(config.signalGroups[0].id, "SG1");
    EXPECT_EQ(config.signalGroups[0].index, 0);
    EXPECT_EQ(config.signalGroups[0].minGreen, 30);
    EXPECT_EQ(config.signalGroups[0].maxGreen, 100);
    EXPECT_EQ(config.signalGroups[0].amber, 30);
    EXPECT_EQ(config.signalGroups[0].detectorIds.size(), 1);
    EXPECT_EQ(config.signalGroups[0].detectorIds[0], "DET1");
    EXPECT_EQ(config.detectors[0].id, "DET1");
    EXPECT_EQ(config.detectors[0].index, 0);
}

TEST(ActuatedControllerTests, InvalidIndexThrows) {
    std::string badSgJson = R"({
        "signalGroups": [{"id":"SG1","index":99,"minGreen":10,"maxGreen":50,"amber":10}]
    })";
    EXPECT_THROW(cif::loadConfigFromJson(badSgJson), std::runtime_error);

    std::string badDetJson = R"({
        "detectors": [{"id":"D1","index":99}]
    })";
    EXPECT_THROW(cif::loadConfigFromJson(badDetJson), std::runtime_error);
}

TEST(ActuatedControllerTests, InitSetsAllRed) {
    auto config = cif::loadConfigFromJson(TEST_CONFIG_JSON);
    cif::setIntersectionConfig(config);

    // Call init
    s_int16 result = applicatieprogramma(CIF_INIT);
    EXPECT_EQ(result, CIF_GEEN_FOUT);

    // All signal groups should be red
    EXPECT_EQ(CIF_GUS[0], CIF_ROOD);
    EXPECT_EQ(CIF_GUS[1], CIF_ROOD);
    // Note: CIF_GUSWIJZ is NOT set during init because the TLC already has
    // the signal groups in StopAndRemain state. We only signal changes when
    // the controller actually transitions a signal group during operation.
}

TEST(ActuatedControllerTests, FileUberSwitchRequestIsHandled) {
    auto config = cif::loadConfigFromJson(TEST_CONFIG_JSON);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    CIF_PB_FILE_UBER_FILEWISSEL = 1;
    applicatieprogramma(CIF_GEEN_INIT);

    EXPECT_EQ(CIF_PB_FILE_UBER_FILEWISSEL, 0) << "Controller should acknowledge switch request";
    ASSERT_NE(CIF_FILE_UBER_FILEWISSEL, nullptr) << "Controller should publish a switch point";
    EXPECT_NE(CIF_FILE_UBER_SCHRIJF, CIF_FILE_UBER_LEES) << "Controller should write file log data";
    EXPECT_EQ(static_cast<char>(*CIF_FILE_UBER_FILEWISSEL), 'S')
        << "Switch-request line should start with 'SWITCH ...'";
}

TEST(ActuatedControllerTests, NoRequestNoGreen) {
    auto config = cif::loadConfigFromJson(TEST_CONFIG_JSON);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    // No detector occupied → should stay red
    CIF_IS[0] = 0; // DET1 not occupied
    for (int i = 0; i < 100; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }

    EXPECT_EQ(CIF_GUS[0], CIF_ROOD);
    EXPECT_EQ(CIF_GUS[1], CIF_ROOD);
}

TEST(ActuatedControllerTests, DetectorRequestTriggersGreen) {
    auto config = cif::loadConfigFromJson(TEST_CONFIG_JSON);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    // Occupy detector → SG1 should go green, SG2 stays red due to conflict
    // (SG1 and SG2 have mutual conflicts defined in TEST_CONFIG_JSON)
    CIF_IS[0] = CIF_DET_BEZET;
    applicatieprogramma(CIF_GEEN_INIT);

    EXPECT_EQ(CIF_GUS[0], CIF_GROEN) << "SG1 should go green (first in list, no blocking conflicts)";
    EXPECT_EQ(CIF_GUS[1], CIF_ROOD) << "SG2 should stay red (blocked by SG1 being green)";
}

TEST(ActuatedControllerTests, MinGreenRespected) {
    // Use single SG config
    std::string json = R"({
        "signalGroups": [
            {"id":"SG1","index":0,"minGreen":30,"maxGreen":100,"amber":30,"minRed":0,"detectors":["D1"]}
        ],
        "detectors": [{"id":"D1","index":0}]
    })";
    auto config = cif::loadConfigFromJson(json);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    // Occupy detector to trigger green
    CIF_IS[0] = CIF_DET_BEZET;
    applicatieprogramma(CIF_GEEN_INIT); // Goes green, timer=0
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN);

    // Release detector immediately
    CIF_IS[0] = 0;

    // Run for minGreen - 1 ticks → should still be green
    for (int i = 0; i < 29; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN) << "Should stay green until minGreen";

    // One more tick → minGreen reached, no extension → amber
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GEEL) << "Should go amber after minGreen with no extension";
}

TEST(ActuatedControllerTests, MaxGreenForcesAmber) {
    std::string json = R"({
        "signalGroups": [
            {"id":"SG1","index":0,"minGreen":30,"maxGreen":50,"amber":30,"minRed":0,"detectors":["D1"]}
        ],
        "detectors": [{"id":"D1","index":0}]
    })";
    auto config = cif::loadConfigFromJson(json);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    // Occupy detector to trigger green and keep extending
    CIF_IS[0] = CIF_DET_BEZET;
    applicatieprogramma(CIF_GEEN_INIT); // Goes green
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN);

    // Keep detector occupied through maxGreen
    for (int i = 0; i < 49; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN) << "Should still be green at maxGreen - 1";

    // maxGreen reached → forced amber regardless of detector
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GEEL) << "Should go amber at maxGreen even with detector occupied";
}

TEST(ActuatedControllerTests, AmberToRed) {
    std::string json = R"({
        "signalGroups": [
            {"id":"SG1","index":0,"minGreen":5,"maxGreen":5,"amber":10,"minRed":0,"detectors":["D1"]}
        ],
        "detectors": [{"id":"D1","index":0}]
    })";
    auto config = cif::loadConfigFromJson(json);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    // Trigger green
    CIF_IS[0] = CIF_DET_BEZET;
    applicatieprogramma(CIF_GEEN_INIT); // Green
    CIF_IS[0] = 0;

    // Run through maxGreen (5 ticks)
    for (int i = 0; i < 5; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_GEEL) << "Should be amber after maxGreen";

    // Run through amber time (10 ticks)
    for (int i = 0; i < 9; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_GEEL) << "Should still be amber";

    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_ROOD) << "Should be red after amber time";
}

TEST(ActuatedControllerTests, GreenExtendByDetector) {
    std::string json = R"({
        "signalGroups": [
            {"id":"SG1","index":0,"minGreen":5,"maxGreen":50,"amber":10,"minRed":0,"detectors":["D1"]}
        ],
        "detectors": [{"id":"D1","index":0}]
    })";
    auto config = cif::loadConfigFromJson(json);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    // Trigger green
    CIF_IS[0] = CIF_DET_BEZET;
    applicatieprogramma(CIF_GEEN_INIT); // Green
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN);

    // Release detector after minGreen
    for (int i = 0; i < 4; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    // minGreen = 5, timer now = 5 (0 at entry + 5 calls)
    // Detector still occupied → should extend
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN) << "Detector occupied past minGreen → extend";

    // Release detector
    CIF_IS[0] = 0;
    applicatieprogramma(CIF_GEEN_INIT);
    // timer = 7, minGreen = 5, no extension → amber
    EXPECT_EQ(CIF_GUS[0], CIF_GEEL) << "Detector released past minGreen → amber";
}

TEST(ActuatedControllerTests, FullCycleRedGreenAmberRed) {
    std::string json = R"({
        "signalGroups": [
            {"id":"SG1","index":0,"minGreen":3,"maxGreen":3,"amber":3,"minRed":0,"detectors":["D1"]}
        ],
        "detectors": [{"id":"D1","index":0}]
    })";
    auto config = cif::loadConfigFromJson(json);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_ROOD);

    // Trigger green
    CIF_IS[0] = CIF_DET_BEZET;
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN);
    CIF_IS[0] = 0;

    // Green for 3 ticks (minGreen = maxGreen = 3)
    for (int i = 0; i < 3; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_GEEL);

    // Amber for 3 ticks
    for (int i = 0; i < 3; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_ROOD);
}

// ============================================================================
// ACTUATED CONTROLLER PRIORITY TESTS (DSI-based priority for special vehicles)
// ============================================================================

TEST(ActuatedControllerTests, LoadConfigWithPriorityFields) {
    // Test loading of direction and priorityMaxGreen fields
    std::string json = R"({
        "signalGroups": [
            {
                "id": "SG1",
                "index": 0,
                "minGreen": 30,
                "maxGreen": 100,
                "amber": 30,
                "direction": 1,
                "priorityMaxGreen": 180,
                "detectors": ["D1"]
            },
            {
                "id": "SG2",
                "index": 1,
                "minGreen": 30,
                "maxGreen": 100,
                "amber": 30,
                "direction": 2,
                "priorityMaxGreen": 150,
                "detectors": ["D1"]
            },
            {
                "id": "SG3",
                "index": 2,
                "minGreen": 30,
                "maxGreen": 100,
                "amber": 30,
                "detectors": ["D1"]
            }
        ],
        "detectors": [{"id": "D1", "index": 0}]
    })";
    auto config = cif::loadConfigFromJson(json);

    EXPECT_EQ(config.signalGroups[0].direction, 1);
    EXPECT_EQ(config.signalGroups[0].priorityMaxGreen, 180);
    EXPECT_EQ(config.signalGroups[1].direction, 2);
    EXPECT_EQ(config.signalGroups[1].priorityMaxGreen, 150);
    EXPECT_EQ(config.signalGroups[2].direction, -1) << "Default direction should be -1";
    EXPECT_EQ(config.signalGroups[2].priorityMaxGreen, 180) << "Default priorityMaxGreen should be 180";
}

TEST(ActuatedControllerTests, DsiCheckinIncrementsPriorityCount) {
    // Test that a checkin event increments the priority vehicle count
    std::string json = R"({
        "signalGroups": [
            {"id": "SG1", "index": 0, "minGreen": 30, "maxGreen": 100, "amber": 30, "minRed": 0, "direction": 1, "detectors": ["D1"]},
            {"id": "SG2", "index": 1, "minGreen": 30, "maxGreen": 100, "amber": 30, "minRed": 0, "direction": 2, "detectors": ["D1"]}
        ],
        "detectors": [{"id": "D1", "index": 0}]
    })";
    auto config = cif::loadConfigFromJson(json);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    // Initially, no priority vehicles
    applicatieprogramma(CIF_GEEN_INIT);
    // We can't directly access g_sgState, but we can verify the behavior through green extension

    // Simulate DSI checkin for direction 1 (SG1)
    CIF_DSI[CIF_DSI_TYPE] = 1;  // Checkin
    CIF_DSI[CIF_DSI_DIR] = 1;   // Direction 1
    CIF_DSIWIJZ = 1;            // Set flag

    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_DSIWIJZ, 0) << "DSI flag should be cleared after processing";

    // Trigger green for SG1
    CIF_IS[0] = CIF_DET_BEZET;
    applicatieprogramma(CIF_GEEN_INIT); // Goes green
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN);

    // Keep detector occupied - with priority, should extend to priorityMaxGreen (180)
    CIF_IS[0] = CIF_DET_BEZET;
    for (int i = 0; i < 179; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN) << "Should stay green until priorityMaxGreen";

    // One more tick - should reach priorityMaxGreen and go amber
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GEEL) << "Should go amber at priorityMaxGreen";
}

TEST(ActuatedControllerTests, DsiCheckoutDecrementsPriorityCount) {
    // Test that a checkout event decrements the priority vehicle count
    // Note: This test is simplified to focus on the basic checkout behavior
    std::string json = R"({
        "signalGroups": [
            {"id": "SG1", "index": 0, "minGreen": 5, "maxGreen": 20, "amber": 5, "minRed": 0, "direction": 1, "priorityMaxGreen": 50, "detectors": ["D1"]}
        ],
        "detectors": [{"id": "D1", "index": 0}]
    })";
    auto config = cif::loadConfigFromJson(json);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    // Checkin - priority active
    CIF_DSI[CIF_DSI_TYPE] = 1;  // Checkin
    CIF_DSI[CIF_DSI_DIR] = 1;
    CIF_DSIWIJZ = 1;
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_DSIWIJZ, 0) << "DSI flag should be cleared after checkin";

    // Trigger green with detector
    CIF_IS[0] = CIF_DET_BEZET;
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN) << "Should go green with priority";

    // With priority, should extend beyond maxGreen (20)
    CIF_IS[0] = CIF_DET_BEZET;
    for (int i = 0; i < 25; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN) << "With priority, should extend beyond maxGreen (20)";

    // Complete the green cycle
    CIF_IS[0] = 0;
    for (int i = 0; i < 30; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_ROOD) << "Should be red after priorityMaxGreen + amber";

    // Checkout - priority removed
    CIF_DSI[CIF_DSI_TYPE] = 2;  // Checkout
    CIF_DSI[CIF_DSI_DIR] = 1;
    CIF_DSIWIJZ = 1;
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_DSIWIJZ, 0) << "DSI flag should be cleared after checkout";
}

TEST(ActuatedControllerTests, MultiplePriorityVehiclesCount) {
    // Test that multiple checkins are counted correctly
    std::string json = R"({
        "signalGroups": [
            {"id": "SG1", "index": 0, "minGreen": 30, "maxGreen": 50, "amber": 10, "minRed": 0, "direction": 1, "priorityMaxGreen": 200, "detectors": ["D1"]}
        ],
        "detectors": [{"id": "D1", "index": 0}]
    })";
    auto config = cif::loadConfigFromJson(json);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    // Two checkins for the same direction
    CIF_DSI[CIF_DSI_TYPE] = 1;
    CIF_DSI[CIF_DSI_DIR] = 1;
    CIF_DSIWIJZ = 1;
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_DSIWIJZ, 0);

    CIF_DSI[CIF_DSI_TYPE] = 1;
    CIF_DSI[CIF_DSI_DIR] = 1;
    CIF_DSIWIJZ = 1;
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_DSIWIJZ, 0);

    // Trigger green - should still be prioritized
    CIF_IS[0] = CIF_DET_BEZET;
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN);

    // With 2 priority vehicles, should still extend to priorityMaxGreen
    CIF_IS[0] = CIF_DET_BEZET;
    for (int i = 0; i < 100; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN) << "With priority vehicles, should extend beyond normal maxGreen";

    // First checkout
    CIF_DSI[CIF_DSI_TYPE] = 2;
    CIF_DSI[CIF_DSI_DIR] = 1;
    CIF_DSIWIJZ = 1;
    applicatieprogramma(CIF_GEEN_INIT);

    // Should still be prioritized (one vehicle still waiting)
    for (int i = 0; i < 50; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN) << "Should still be prioritized with one vehicle remaining";

    // Second checkout - no more priority vehicles
    CIF_DSI[CIF_DSI_TYPE] = 2;
    CIF_DSI[CIF_DSI_DIR] = 1;
    CIF_DSIWIJZ = 1;
    applicatieprogramma(CIF_GEEN_INIT);

    // After checkout completes, the priority should be cleared
    // (verified by subsequent behavior in next cycle)
}

TEST(ActuatedControllerTests, PrioritySignalGroupSkipsFairness) {
    // Test that signal groups with priority requests are allowed through fairness checks
    // Note: This test verifies the fairnessAllowsGreen() behavior with priority
    std::string json = R"({
        "signalGroups": [
            {"id": "SG1", "index": 0, "minGreen": 5, "maxGreen": 20, "amber": 5, "minRed": 0, "direction": 1, "priorityMaxGreen": 50, "detectors": ["D1"]},
            {"id": "SG2", "index": 1, "minGreen": 5, "maxGreen": 20, "amber": 5, "minRed": 0, "direction": 2, "detectors": ["D2"]}
        ],
        "detectors": [
            {"id": "D1", "index": 0},
            {"id": "D2", "index": 1}
        ],
        "conflicts": [
            {"from": "SG1", "to": "SG2", "intergreen": 10},
            {"from": "SG2", "to": "SG1", "intergreen": 10}
        ]
    })";
    auto config = cif::loadConfigFromJson(json);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    // Set both detectors occupied
    CIF_IS[0] = CIF_DET_BEZET;
    CIF_IS[1] = CIF_DET_BEZET;

    // SG1 goes green first
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN) << "SG1 should go green first";

    // Give SG1 priority
    CIF_DSI[CIF_DSI_TYPE] = 1;  // Checkin for SG1
    CIF_DSI[CIF_DSI_DIR] = 1;
    CIF_DSIWIJZ = 1;
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_DSIWIJZ, 0) << "DSI flag should be cleared after checkin";

    // Run SG1 through green - amber - red cycle
    CIF_IS[0] = 0;  // Clear detector to allow transition
    for (int i = 0; i < 20; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_ROOD) << "SG1 should be red after completing its cycle";

    // SG2 should now go green
    EXPECT_EQ(CIF_GUS[1], CIF_GROEN) << "SG2 should go green after SG1 completes";

    // Complete SG2's cycle
    CIF_IS[1] = 0;
    for (int i = 0; i < 20; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }

    // Both signal groups have been served, but SG1 has priority
    // When both have requests, SG1 should be prioritized in the sorted order
    CIF_IS[0] = CIF_DET_BEZET;
    CIF_IS[1] = CIF_DET_BEZET;
    applicatieprogramma(CIF_GEEN_INIT);

    // With priority, SG1 should go green first due to priority sorting
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN) << "SG1 with priority should go green first";
}

TEST(ActuatedControllerTests, UnknownDirectionIgnored) {
    // Test that DSI events with unknown directions are ignored
    std::string json = R"({
        "signalGroups": [
            {"id": "SG1", "index": 0, "minGreen": 30, "maxGreen": 50, "amber": 10, "minRed": 0, "direction": 1, "detectors": ["D1"]}
        ],
        "detectors": [{"id": "D1", "index": 0}]
    })";
    auto config = cif::loadConfigFromJson(json);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    // DSI event for direction 99 (unknown)
    CIF_DSI[CIF_DSI_TYPE] = 1;
    CIF_DSI[CIF_DSI_DIR] = 99;
    CIF_DSIWIJZ = 1;
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_DSIWIJZ, 0) << "Flag should be cleared even for unknown direction";

    // Trigger green - should use normal maxGreen (50), not priority
    CIF_IS[0] = CIF_DET_BEZET;
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN);

    CIF_IS[0] = CIF_DET_BEZET;
    for (int i = 0; i < 49; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN);

    // Should go amber at normal maxGreen (50) since priority was not set
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GEEL);
}

TEST(ActuatedControllerTests, PriorityWithNegativeDirection) {
    // Test that signal groups with direction=-1 don't receive priority
    std::string json = R"({
        "signalGroups": [
            {"id": "SG1", "index": 0, "minGreen": 30, "maxGreen": 50, "amber": 10, "minRed": 0, "detectors": ["D1"]}
        ],
        "detectors": [{"id": "D1", "index": 0}]
    })";
    auto config = cif::loadConfigFromJson(json);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    // DSI event for direction 1
    CIF_DSI[CIF_DSI_TYPE] = 1;
    CIF_DSI[CIF_DSI_DIR] = 1;
    CIF_DSIWIJZ = 1;
    applicatieprogramma(CIF_GEEN_INIT);

    // Trigger green - should use normal maxGreen since SG1 has no direction
    CIF_IS[0] = CIF_DET_BEZET;
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN);

    CIF_IS[0] = CIF_DET_BEZET;
    for (int i = 0; i < 49; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN);

    // Should go amber at normal maxGreen (no priority support)
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GEEL);
}

TEST(ActuatedControllerTests, PriorityExtendsMaxGreenNotMinGreen) {
    // Test that priority affects max green, not min green
    std::string json = R"({
        "signalGroups": [
            {"id": "SG1", "index": 0, "minGreen": 100, "maxGreen": 50, "amber": 10, "minRed": 0, "direction": 1, "priorityMaxGreen": 150, "detectors": ["D1"]}
        ],
        "detectors": [{"id": "D1", "index": 0}]
    })";
    auto config = cif::loadConfigFromJson(json);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    // Set priority
    CIF_DSI[CIF_DSI_TYPE] = 1;
    CIF_DSI[CIF_DSI_DIR] = 1;
    CIF_DSIWIJZ = 1;
    applicatieprogramma(CIF_GEEN_INIT);

    // Trigger green
    CIF_IS[0] = CIF_DET_BEZET;
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN);

    // Release detector immediately - should still stay green until minGreen (100)
    CIF_IS[0] = 0;
    for (int i = 0; i < 99; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN) << "Should stay green until minGreen even without detector";

    // At minGreen (100), without detector, should go amber
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GEEL) << "Should go amber at minGreen when no extension";
}

TEST(ActuatedControllerTests, NoDsiFlagMeansNoProcessing) {
    // Test that when CIF_DSIWIJZ=0, no DSI processing occurs
    std::string json = R"({
        "signalGroups": [
            {"id": "SG1", "index": 0, "minGreen": 30, "maxGreen": 50, "amber": 10, "minRed": 0, "direction": 1, "priorityMaxGreen": 100, "detectors": ["D1"]}
        ],
        "detectors": [{"id": "D1", "index": 0}]
    })";
    auto config = cif::loadConfigFromJson(json);
    cif::setIntersectionConfig(config);
    applicatieprogramma(CIF_INIT);

    // Set DSI data but don't set the flag
    CIF_DSI[CIF_DSI_TYPE] = 1;
    CIF_DSI[CIF_DSI_DIR] = 1;
    CIF_DSIWIJZ = 0;  // Flag NOT set

    applicatieprogramma(CIF_GEEN_INIT);

    // Trigger green - should use normal maxGreen (50) since DSI wasn't processed
    CIF_IS[0] = CIF_DET_BEZET;
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN);

    CIF_IS[0] = CIF_DET_BEZET;
    for (int i = 0; i < 49; ++i) {
        applicatieprogramma(CIF_GEEN_INIT);
    }
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN);

    // Should go amber at normal maxGreen (50)
    applicatieprogramma(CIF_GEEN_INIT);
    EXPECT_EQ(CIF_GUS[0], CIF_GEEL) << "Should use normal maxGreen when DSI flag not set";
}

// ============================================================================
// 2. CIF BRIDGE TESTS (mapping + translation)
// ============================================================================

TEST(CifBridgeTests, SignalGroupMapping) {
    CifTestFixture fixture(12601);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    bridge.setSignalGroupCount(2);
    bridge.setSignalGroupMapping("SG1", 0);
    bridge.setSignalGroupMapping("SG2", 1);

    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG1"), 0);
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG2"), 1);
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG_UNKNOWN"), -1);

    EXPECT_EQ(bridge.translateIndexToSignalGroupId(0), "SG1");
    EXPECT_EQ(bridge.translateIndexToSignalGroupId(1), "SG2");
    EXPECT_EQ(bridge.translateIndexToSignalGroupId(99), "");

    app.stop();
}

TEST(CifBridgeTests, DetectorMapping) {
    CifTestFixture fixture(12602);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    bridge.setDetectorCount(2);
    bridge.setDetectorMapping("DET1", 0);
    bridge.setDetectorMapping("DET2", 1);

    EXPECT_EQ(bridge.translateDetectorIdToIndex("DET1"), 0);
    EXPECT_EQ(bridge.translateDetectorIdToIndex("DET2"), 1);
    EXPECT_EQ(bridge.translateDetectorIdToIndex("UNKNOWN"), -1);

    app.stop();
}

TEST(CifBridgeTests, DuplicateMappingIgnored) {
    CifTestFixture fixture(12603);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    bridge.setSignalGroupCount(1);
    bridge.setSignalGroupMapping("SG1", 0);
    bridge.setSignalGroupMapping("SG1", 5); // duplicate, should be ignored

    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG1"), 0) << "Duplicate mapping should be ignored";

    app.stop();
}

TEST(CifBridgeTests, OutputsUseSignalGroupCountOffset) {
    // Test that outputs are placed after signal groups in CIF_GUS[]
    // With 2 signal groups, output index 0 should map to CIF_GUS[2]
    CifTestFixture fixture(12604);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    // Configure 2 signal groups
    bridge.setSignalGroupCount(2);
    bridge.setSignalGroupMapping("SG1", 0);
    bridge.setSignalGroupMapping("SG2", 1);

    // Configure outputs with type-local indices 0 and 1
    bridge.setOutputMapping("OUT1", 0);
    bridge.setOutputMapping("OUT2", 1);

    // Signal groups should map to their direct indices
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG1"), 0);
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG2"), 1);

    // Outputs should be offset by signal group count (2)
    // OUT1 (local index 0) -> CIF_GUS[2]
    // OUT2 (local index 1) -> CIF_GUS[3]
    EXPECT_EQ(bridge.translateOutputIdToIndex("OUT1"), 2) << "OUT1 should map to CIF_GUS[signalGroupCount + 0]";
    EXPECT_EQ(bridge.translateOutputIdToIndex("OUT2"), 3) << "OUT2 should map to CIF_GUS[signalGroupCount + 1]";

    app.stop();
}

TEST(CifBridgeTests, InputsUseDetectorCountOffset) {
    // Test that inputs are placed after detectors in CIF_IS[]
    // With 3 detectors, input index 0 should map to CIF_IS[3]
    CifTestFixture fixture(12605);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    // Configure 3 detectors
    bridge.setDetectorCount(3);
    bridge.setDetectorMapping("DET1", 0);
    bridge.setDetectorMapping("DET2", 1);
    bridge.setDetectorMapping("DET3", 2);

    // Configure inputs with type-local indices 0 and 1
    bridge.setInputMapping("IN1", 0);
    bridge.setInputMapping("IN2", 1);

    // Detectors should map to their direct indices
    EXPECT_EQ(bridge.translateDetectorIdToIndex("DET1"), 0);
    EXPECT_EQ(bridge.translateDetectorIdToIndex("DET2"), 1);
    EXPECT_EQ(bridge.translateDetectorIdToIndex("DET3"), 2);

    // Inputs should be offset by detector count (3)
    // IN1 (local index 0) -> CIF_IS[3]
    // IN2 (local index 1) -> CIF_IS[4]
    EXPECT_EQ(bridge.translateInputIdToIndex("IN1"), 3) << "IN1 should map to CIF_IS[detectorCount + 0]";
    EXPECT_EQ(bridge.translateInputIdToIndex("IN2"), 4) << "IN2 should map to CIF_IS[detectorCount + 1]";

    app.stop();
}

// ============================================================================
// 2b. MANUAL COUPLE TESTS (manuallyCoupledItems)
// ============================================================================
//
// buildMappings() ingests the (cifName, index) pairs the host scans from the
// controller. Manual couples map a controller slot under its tlcfiName instead
// of its cifName and win over identity matches. Most of these assert directly
// via translate*/getUnmatchedManualCouples/getUnmappedControllerItems and so
// need no live TLC catalog; the app is constructed but never connected.

namespace {
using Scan = std::vector<std::pair<std::string, int>>;
}

TEST(ManualCoupleTests, BasicCoupleMapsUnderTlcfiName) {
    CifTestFixture fixture;
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    cif::ManualCouples mc;
    mc.signalGroups.push_back({"us01", "US001"});
    bridge.setManualCouples(mc);
    bridge.setSignalGroupCount(2);

    // Controller reports an ordinary "SG0" at 0 and the coupled "US001" at 1.
    bridge.buildMappings(Scan{{"SG0", 0}, {"US001", 1}}, Scan{}, Scan{}, Scan{});

    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("us01"), 1)  << "coupled under tlcfiName";
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("US001"), -1) << "not coupled under cifName";
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG0"), 0)   << "auto identity match still works";
    EXPECT_EQ(bridge.translateIndexToSignalGroupId(1), "us01")  << "reverse index reports the TLC id";

    std::vector<cif::ManualCouple> uSg, uOut, uDet, uIn;
    bridge.getUnmatchedManualCouples(uSg, uOut, uDet, uIn);
    EXPECT_TRUE(uSg.empty() && uOut.empty() && uDet.empty() && uIn.empty());

    std::vector<std::string> oSg, oOut, oDet, oIn;
    bridge.getUnmappedControllerItems(oSg, oOut, oDet, oIn);
    EXPECT_TRUE(oSg.empty() && oOut.empty() && oDet.empty() && oIn.empty());

    app.stop();
}

TEST(ManualCoupleTests, PrecedenceIsOrderIndependent) {
    // Whether the literal "us01" comes before or after "US001" in the scan, the
    // couple must win: us01 -> index(US001), and the literal us01 is orphaned.
    for (const Scan& scan : {Scan{{"us01", 0}, {"US001", 1}},
                             Scan{{"US001", 1}, {"us01", 0}}}) {
        CifTestFixture fixture;
        FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
        cif::CifBridge bridge(app);

        cif::ManualCouples mc;
        mc.signalGroups.push_back({"us01", "US001"});
        bridge.setManualCouples(mc);
        bridge.setSignalGroupCount(2);
        bridge.buildMappings(scan, Scan{}, Scan{}, Scan{});

        EXPECT_EQ(bridge.translateSignalGroupIdToIndex("us01"), 1)
            << "couple must win regardless of scan order";

        std::vector<std::string> oSg, oOut, oDet, oIn;
        bridge.getUnmappedControllerItems(oSg, oOut, oDet, oIn);
        ASSERT_EQ(oSg.size(), 1U) << "literal us01 slot is coupled to nothing";
        EXPECT_EQ(oSg[0], "us01");

        app.stop();
    }
}

TEST(ManualCoupleTests, ShadowedItemDeclaredControllerOnlyIsNotOrphan) {
    // The engineer resolves the collision explicitly by declaring the literal
    // us01 as controller-only. Then nothing is orphaned.
    CifTestFixture fixture;
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    cif::ManualCouples mc;
    mc.signalGroups.push_back({"us01", "US001"});
    bridge.setManualCouples(mc);
    cif::IgnoredItems ig;
    ig.signalGroups.insert("us01");
    bridge.setIgnoredItems(ig);
    bridge.setSignalGroupCount(2);

    bridge.buildMappings(Scan{{"us01", 0}, {"US001", 1}}, Scan{}, Scan{}, Scan{});

    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("us01"), 1) << "manual couple stands";
    std::vector<std::string> oSg, oOut, oDet, oIn;
    bridge.getUnmappedControllerItems(oSg, oOut, oDet, oIn);
    EXPECT_TRUE(oSg.empty()) << "shadowed item is declared controller-only, so not orphaned";

    app.stop();
}

TEST(ManualCoupleTests, MissingCifNameIsReportedUnmatched) {
    CifTestFixture fixture;
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    cif::ManualCouples mc;
    mc.detectors.push_back({"d1", "DTEST"});  // DTEST does not exist on the controller
    bridge.setManualCouples(mc);
    bridge.setDetectorCount(1);
    bridge.buildMappings(Scan{}, Scan{}, Scan{{"OTHER", 0}}, Scan{});

    std::vector<cif::ManualCouple> uSg, uOut, uDet, uIn;
    bridge.getUnmatchedManualCouples(uSg, uOut, uDet, uIn);
    ASSERT_EQ(uDet.size(), 1U);
    EXPECT_EQ(uDet[0].cifName, "DTEST");
    EXPECT_EQ(uDet[0].tlcfiName, "d1");
    EXPECT_TRUE(uSg.empty() && uOut.empty() && uIn.empty());

    EXPECT_EQ(bridge.translateDetectorIdToIndex("d1"), -1);

    app.stop();
}

TEST(ManualCoupleTests, DuplicateControllerNameIsOrphaned) {
    // No manual couple: two controller items with the same name. The second is
    // coupled to nothing and reported (pathological controller config).
    CifTestFixture fixture;
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    bridge.setSignalGroupCount(2);
    bridge.buildMappings(Scan{{"SG1", 0}, {"SG1", 1}}, Scan{}, Scan{}, Scan{});

    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG1"), 0) << "first wins";
    std::vector<std::string> oSg, oOut, oDet, oIn;
    bridge.getUnmappedControllerItems(oSg, oOut, oDet, oIn);
    ASSERT_EQ(oSg.size(), 1U);
    EXPECT_EQ(oSg[0], "SG1");

    app.stop();
}

TEST(ManualCoupleTests, CouplesApplyPerTypeWithOffsets) {
    CifTestFixture fixture;
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    cif::ManualCouples mc;
    mc.signalGroups.push_back({"sgTlc", "SGCIF"});
    mc.outputs.push_back({"outTlc", "OUTCIF"});
    mc.detectors.push_back({"detTlc", "DETCIF"});
    mc.inputs.push_back({"inTlc", "INCIF"});
    bridge.setManualCouples(mc);
    bridge.setSignalGroupCount(2);  // outputs offset by 2
    bridge.setDetectorCount(3);     // inputs offset by 3

    bridge.buildMappings(Scan{{"SGCIF", 0}}, Scan{{"OUTCIF", 1}},
                         Scan{{"DETCIF", 0}}, Scan{{"INCIF", 1}});

    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("sgTlc"), 0);
    EXPECT_EQ(bridge.translateOutputIdToIndex("outTlc"), 1 + 2) << "output offset by signalGroupCount";
    EXPECT_EQ(bridge.translateDetectorIdToIndex("detTlc"), 0);
    EXPECT_EQ(bridge.translateInputIdToIndex("inTlc"), 1 + 3) << "input offset by detectorCount";

    app.stop();
}

TEST(ManualCoupleTests, ManualIgnoredAndAutoCoexist) {
    CifTestFixture fixture;
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    cif::ManualCouples mc;
    mc.signalGroups.push_back({"tlcB", "cifB"});
    bridge.setManualCouples(mc);
    cif::IgnoredItems ig;
    ig.signalGroups.insert("cifIgnored");
    bridge.setIgnoredItems(ig);
    bridge.setSignalGroupCount(3);

    // cifA -> auto, cifB -> manual (as tlcB), cifIgnored -> controller-only.
    bridge.buildMappings(Scan{{"cifA", 0}, {"cifB", 1}, {"cifIgnored", 2}},
                         Scan{}, Scan{}, Scan{});

    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("cifA"), 0)  << "auto";
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("tlcB"), 1)  << "manual under tlcfiName";
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("cifB"), -1) << "manual cifName not a key";
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("cifIgnored"), -1) << "ignored, not mapped";

    std::vector<std::string> imSg, imOut, imDet, imIn;
    bridge.getUnmatchedIgnoredItems(imSg, imOut, imDet, imIn);
    EXPECT_TRUE(imSg.empty()) << "cifIgnored matched a controller item";

    std::vector<cif::ManualCouple> uSg, uOut, uDet, uIn;
    bridge.getUnmatchedManualCouples(uSg, uOut, uDet, uIn);
    EXPECT_TRUE(uSg.empty());

    std::vector<std::string> oSg, oOut, oDet, oIn;
    bridge.getUnmappedControllerItems(oSg, oOut, oDet, oIn);
    EXPECT_TRUE(oSg.empty());

    app.stop();
}

TEST(ManualCoupleTests, MultipleCouplesInOneType) {
    CifTestFixture fixture;
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    cif::ManualCouples mc;
    mc.signalGroups.push_back({"a", "A"});
    mc.signalGroups.push_back({"b", "B"});
    bridge.setManualCouples(mc);
    bridge.setSignalGroupCount(2);
    bridge.buildMappings(Scan{{"A", 0}, {"B", 1}}, Scan{}, Scan{}, Scan{});

    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("a"), 0);
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("b"), 1);

    std::vector<cif::ManualCouple> uSg, uOut, uDet, uIn;
    bridge.getUnmatchedManualCouples(uSg, uOut, uDet, uIn);
    EXPECT_TRUE(uSg.empty());

    app.stop();
}

TEST(ManualCoupleTests, ManualTlcfiNameParticipatesInStaleCheck) {
    // A manual couple inserts the tlcfiName as the map key. With an (unconnected)
    // empty TLC catalog, every mapped key is stale — proving the manual key is
    // the one checked against the TLC, not the cifName. This is exactly how a
    // manual tlcfiName absent from the TLC becomes a fatal stale error.
    CifTestFixture fixture;
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    cif::ManualCouples mc;
    mc.signalGroups.push_back({"us01", "US001"});
    bridge.setManualCouples(mc);
    bridge.setSignalGroupCount(1);
    bridge.buildMappings(Scan{{"US001", 0}}, Scan{}, Scan{}, Scan{});

    std::vector<std::string> staleSg, staleOut, staleDet, staleIn;
    bridge.getStaleObjects(staleSg, staleOut, staleDet, staleIn);
    ASSERT_EQ(staleSg.size(), 1U);
    EXPECT_EQ(staleSg[0], "us01") << "stale key is the tlcfiName, not the cifName";
    EXPECT_FALSE(bridge.validateMappings());

    app.stop();
}

TEST(CifBridgeTests, FileVlogDrainClearsSwitchStateAndFinalizesFile) {
    CifTestFixture fixture(12606);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    auto testDir = std::filesystem::temp_directory_path() / "openitscontainer_filevlog_test";
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
    std::filesystem::create_directories(testDir, ec);
    ASSERT_FALSE(ec) << "Failed to create test directory: " << ec.message();

    bridge.configureFileLogging(true, testDir.string(), "TestSystem");

    const std::string payload = "TIME 2026-03-09 12:00:00\r\n";
    setupVlogRingForRotation(payload);
    bridge.pollFileVlog();

    EXPECT_EQ(CIF_FILE_UBER_LEES, CIF_FILE_UBER_SCHRIJF)
        << "Bridge should drain all available bytes";
    EXPECT_EQ(CIF_PB_FILE_UBER_FILEWISSEL, 0)
        << "Bridge should clear PB switch request after boundary is consumed";
    EXPECT_EQ(CIF_FILE_UBER_FILEWISSEL, nullptr)
        << "Bridge should clear consumed switch pointer";

    bridge.reset(); // Finalizes current .tmp file into .vlg

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(testDir)) {
        if (entry.path().extension() == ".vlg") {
            files.push_back(entry.path());
        }
    }

    ASSERT_EQ(files.size(), 1U) << "Exactly one finalized .vlg file expected";

    std::ifstream in(files.front(), std::ios::binary);
    ASSERT_TRUE(in.is_open()) << "Expected .vlg file to be readable";
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(content, payload);

    std::filesystem::remove_all(testDir, ec);
    app.stop();
}

// ============================================================================
// 3. CIF HOST INTEGRATION TESTS
// ============================================================================

TEST(CifHostTests, ControlLoopStartsOnInControl) {
    // Test that CifHost starts the control loop when InControl is reached
    CifTestFixture fixture(12610);

    auto config = cif::loadConfigFromJson(TEST_CONFIG_JSON);
    cif::setIntersectionConfig(config);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    // Bridge mappings are built automatically by host.init() from the actuated
    // controller's getCifItemName() contract (driven by the config set above),
    // and validated against the FiFacilitiesServer catalog when meta arrives.
    cif::CifHost host(app, bridge);

    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));

    // Wait for InControl (state machine completes)
    std::atomic<bool> inControlReached{false};
    auto originalCallback = app.onControlInControl;
    app.onControlInControl = [&](const std::string& id) {
        inControlReached = true;
    };

    // Init the host (installs the onControlInControl hook that starts the loop)
    host.init();

    ASSERT_TRUE(fixture.waitFor([&app]() {
        return app.isFullyInitialized();
    }, std::chrono::milliseconds(5000))) << "App did not complete initialization";

    // Wait a bit for the control loop to start and run some iterations
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // After init + InControl, the controller should have been initialized
    // All signal groups should be red (from applicatieprogramma(CIF_INIT))
    EXPECT_EQ(CIF_GUS[0], CIF_ROOD);
    EXPECT_EQ(CIF_GUS[1], CIF_ROOD);

    host.stopLoop();
    app.stop();
}

// ============================================================================
// 2b. CIF BRIDGE RESET TESTS
// ============================================================================

TEST(CifBridgeTests, ResetPreservesMappings) {
    // Verify that reset() preserves ID-to-index mappings (they are configuration,
    // not runtime state) while clearing pending requests and DSI queue.
    CifTestFixture fixture(12620);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    // Populate mappings
    bridge.setSignalGroupCount(2);
    bridge.setDetectorCount(1);
    bridge.setSignalGroupMapping("SG1", 0);
    bridge.setSignalGroupMapping("SG2", 1);
    bridge.setOutputMapping("OUT1", 0);
    bridge.setDetectorMapping("DET1", 0);
    bridge.setInputMapping("IN1", 0);

    // Reset
    bridge.reset();

    // All mappings should still exist
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG1"), 0)
        << "SG mapping should persist after reset";
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG2"), 1);
    EXPECT_EQ(bridge.translateOutputIdToIndex("OUT1"), 2)
        << "Output mapping should persist after reset";
    EXPECT_EQ(bridge.translateDetectorIdToIndex("DET1"), 0)
        << "Detector mapping should persist after reset";
    EXPECT_EQ(bridge.translateInputIdToIndex("IN1"), 1)
        << "Input mapping should persist after reset";

    // Reverse mappings should also persist
    EXPECT_EQ(bridge.translateIndexToSignalGroupId(0), "SG1")
        << "Reverse SG mapping should persist after reset";
    EXPECT_EQ(bridge.translateIndexToOutputId(0), "OUT1")
        << "Reverse output mapping should persist after reset";

    app.stop();
}

TEST(CifBridgeTests, DuplicateMappingAfterResetIgnored) {
    // Verify that after reset(), duplicate mappings are still ignored since
    // mappings persist across reset (configuration is set once at startup).
    CifTestFixture fixture(12621);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    // First population
    bridge.setSignalGroupCount(1);
    bridge.setSignalGroupMapping("SG1", 0);
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG1"), 0);

    // Reset (clears pending requests, not mappings)
    bridge.reset();

    // Attempting to re-add the same mapping should be ignored (duplicate)
    bridge.setSignalGroupMapping("SG1", 5);
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG1"), 0)
        << "Original mapping should be preserved, duplicate ignored";

    app.stop();
}

TEST(CifBridgeTests, ResetClearsPendingRequests) {
    // Verify that reset() clears pending signal group and output requests
    // so that stale pending state from a previous connection doesn't prevent
    // sending updates after reconnection.
    CifTestFixture fixture(12622);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    bridge.setSignalGroupCount(1);
    bridge.setDetectorCount(0);
    bridge.setSignalGroupMapping("SG1", 0);
    bridge.setOutputMapping("OUT1", 0);

    // Connect and initialize to enable bridge start
    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));

    ASSERT_TRUE(fixture.waitFor([&app]() {
        return app.isFullyInitialized();
    }, std::chrono::milliseconds(5000)));

    bridge.start();

    // Simulate a desired output change (writes to CIF_GUS, which creates pending requests)
    CIF_GUS[0] = CIF_GROEN;
    CIF_GUSWIJZ = CIF_GESCHREVEN;
    bridge.processDesiredOutputChanges();

    // At this point there should be pending SG requests for SG1

    // Reset should clear those pending requests
    bridge.reset();

    // After reset, re-populate mappings
    bridge.setSignalGroupCount(1);
    bridge.setSignalGroupMapping("SG1", 0);

    // Set the same desired state again
    CIF_GUS[0] = CIF_GROEN;
    CIF_GUSWIJZ = CIF_GESCHREVEN;

    // This should NOT skip the update due to stale pending state
    // (We can't easily verify the request was sent without mocking,
    // but at minimum processDesiredOutputChanges should not crash
    // and the pending request tracking should work from scratch)
    bridge.processDesiredOutputChanges();

    app.stop();
}

TEST(CifBridgeTests, ValidateMappingsStableAfterReset) {
    // Verify that validateMappings() still succeeds after reset(), since
    // mappings are configuration that persists across reconnections.
    CifTestFixture fixture(12623);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));

    ASSERT_TRUE(fixture.waitFor([&app]() {
        return app.isFullyInitialized();
    }, std::chrono::milliseconds(5000)));

    // Populate all required mappings for the server's catalog
    bridge.setSignalGroupCount(2);
    bridge.setDetectorCount(1);
    bridge.setSignalGroupMapping("SG1", 0);
    bridge.setSignalGroupMapping("SG2", 1);
    bridge.setOutputMapping("OUT1", 0);
    bridge.setOutputMapping("OUT2", 1);
    bridge.setDetectorMapping("DET1", 0);
    bridge.setInputMapping("IN1", 0);

    EXPECT_TRUE(bridge.validateMappings()) << "Mappings should be valid before reset";

    // Reset clears pending requests and DSI queue, but not mappings
    bridge.reset();
    EXPECT_TRUE(bridge.validateMappings()) << "Mappings should still be valid after reset";

    app.stop();
}

// ============================================================================
// 3. CIF HOST INTEGRATION TESTS
// ============================================================================

TEST(CifHostTests, ReconnectionResetsAndResumes) {
    // Full integration test: verify that on connection loss, the CifHost stops
    // the control loop and resets the bridge, and that on reconnection the full
    // state machine restarts and the controller resumes operation.
    //
    // Flow:
    // 1. Connect → InControl → controller running (CIF_GUS = red)
    // 2. Kill server → onConnectionLost → loop stops, bridge resets
    // 3. Fresh server → reconnect → InControl again → controller resumes

    auto config = cif::loadConfigFromJson(TEST_CONFIG_JSON);
    cif::setIntersectionConfig(config);

    FiControllerApp app("127.0.0.1", "12650", "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);
    cif::CifHost host(app, bridge);

    std::atomic<int> inControlCount{0};
    std::atomic<bool> connectionLostFired{false};

    auto setupBridgeMappings = [&bridge]() {
        bridge.setSignalGroupCount(2);
        bridge.setDetectorCount(1);
        bridge.setSignalGroupMapping("SG1", 0);
        bridge.setSignalGroupMapping("SG2", 1);
        bridge.setDetectorMapping("DET1", 0);
        bridge.setOutputMapping("OUT1", 0);
        bridge.setOutputMapping("OUT2", 1);
        bridge.setInputMapping("IN1", 0);
    };

    setupBridgeMappings();

    // Helper to wait for condition
    auto waitFor = [](auto&& pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        auto start = std::chrono::steady_clock::now();
        while (!pred()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (std::chrono::steady_clock::now() - start > timeout) return false;
        }
        return true;
    };

    // --- Phase 1: First connection ---
    {
        CifTestFixture fixture(12650);
        host.init();

        // Chain our tracking callbacks AFTER host.init() (which sets its own)
        auto hostOnInControl = app.onControlInControl;
        app.onControlInControl = [&, hostOnInControl](const std::string& id) {
            if (hostOnInControl) hostOnInControl(id);
            inControlCount++;
        };

        auto hostOnConnectionLost = app.onConnectionLost;
        app.onConnectionLost = [&, hostOnConnectionLost](const std::string& reason) {
            if (hostOnConnectionLost) hostOnConnectionLost(reason);
            connectionLostFired = true;
        };

        ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
        ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));

        ASSERT_TRUE(waitFor([&inControlCount]() {
            return inControlCount.load() >= 1;
        })) << "First InControl not reached";

        // Wait for control loop to run a few iterations
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Controller should have initialized - all SGs should be red
        EXPECT_EQ(CIF_GUS[0], CIF_ROOD) << "SG1 should be red after init";
        EXPECT_EQ(CIF_GUS[1], CIF_ROOD) << "SG2 should be red after init";

        // Bridge mappings should be valid
        EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG1"), 0);
    }
    // Server destroyed → connection lost

    // --- Phase 2: Verify disconnection cleanup ---
    ASSERT_TRUE(waitFor([&connectionLostFired]() {
        return connectionLostFired.load();
    })) << "onConnectionLost was not invoked after server shutdown";

    // After connection lost, bridge should be reset (pending requests cleared)
    // but mappings persist since they are configuration
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG1"), 0)
        << "Bridge mappings should persist after connection lost";
    EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG2"), 1)
        << "Bridge mappings should persist after connection lost";

    // Clean up app state for reconnection
    app.cleanup();
    connectionLostFired = false;

    // --- Phase 3: Reconnect ---
    // Mappings persist across reset, no need to re-populate

    {
        CifTestFixture fixture2(12650);

        ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
        ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));

        ASSERT_TRUE(waitFor([&inControlCount]() {
            return inControlCount.load() >= 2;
        })) << "Second InControl not reached after reconnection";

        // Wait for control loop to restart
        std::this_thread::sleep_for(std::chrono::milliseconds(300));

        // Controller should be running again
        EXPECT_EQ(CIF_GUS[0], CIF_ROOD) << "SG1 should be red after reconnection";
        EXPECT_EQ(CIF_GUS[1], CIF_ROOD) << "SG2 should be red after reconnection";

        // Bridge mappings should be valid again
        EXPECT_EQ(bridge.translateSignalGroupIdToIndex("SG1"), 0);
        EXPECT_TRUE(bridge.validateMappings()) << "Bridge mappings should be valid after reconnection";

        host.stopLoop();
        app.stop();
    }
}

TEST(CifHostTests, ControllerRunsWithDetectorInput) {
    // Test the full chain: detector → CIF_IS → controller → CIF_GUS → bridge
    CifTestFixture fixture(12611);

    // Use a config where SG1 uses DET1 with short timings for faster test
    std::string json = R"({
        "signalGroups": [
            {"id":"SG1","index":0,"minGreen":3,"maxGreen":5,"amber":3,"minRed":0,"detectors":["DET1"]}
        ],
        "detectors": [{"id":"DET1","index":0}]
    })";
    auto config = cif::loadConfigFromJson(json);
    cif::setIntersectionConfig(config);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    // Map all objects from the FiFacilitiesServer catalog
    // First set counts (used as offsets for outputs and inputs in combined arrays)
    bridge.setSignalGroupCount(2);
    bridge.setDetectorCount(1);

    bridge.setSignalGroupMapping("SG1", 0);
    bridge.setSignalGroupMapping("SG2", 1);
    bridge.setDetectorMapping("DET1", 0);
    bridge.setOutputMapping("OUT1", 0);
    bridge.setOutputMapping("OUT2", 1);
    bridge.setInputMapping("IN1", 0);

    cif::CifHost host(app, bridge);
    host.init();

    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));

    // Wait for InControl + loop running
    ASSERT_TRUE(fixture.waitFor([&app]() {
        return app.isFullyInitialized();
    }, std::chrono::milliseconds(5000)));

    // Wait for control loop to start
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Verify initial state is red
    EXPECT_EQ(CIF_GUS[0], CIF_ROOD);

    // Simulate detector occupied (as if the bridge received it from TLC-FI)
    CIF_IS[0] = CIF_DET_BEZET;
    CIF_ISWIJZ = CIF_GESCHREVEN;

    // Wait for controller to process (a few 100ms cycles)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Controller should have gone green
    EXPECT_EQ(CIF_GUS[0], CIF_GROEN) << "Controller should go green when detector occupied";

    // Release detector
    CIF_IS[0] = 0;
    CIF_ISWIJZ = CIF_GESCHREVEN;

    // Wait for minGreen + amber (3 + 3 = 6 ticks = 600ms), plus some margin
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    // Should be back to red after green + amber cycle
    EXPECT_EQ(CIF_GUS[0], CIF_ROOD) << "Controller should return to red after cycle completes";

    host.stopLoop();
    app.stop();
}

// ============================================================================
// 4. CIF BRIDGE DSI TESTS (SpecialVehicleEvent → CIF_DSI[])
// ============================================================================

TEST(CifBridgeDsiTests, EmptyQueueSetsDsiwijzZero) {
    // When DSI queue is empty, prepareDsiBuffer should set CIF_DSIWIJZ = 0
    CifTestFixture fixture(12700);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    CIF_DSIWIJZ = 1; // Set to non-zero to verify it gets cleared
    bridge.prepareDsiBuffer();

    EXPECT_EQ(CIF_DSIWIJZ, 0) << "CIF_DSIWIJZ should be 0 when no events queued";

    app.stop();
}

TEST(CifBridgeDsiTests, SingleEventPopulatesDsiBuffer) {
    // A single queued event should populate CIF_DSI[] and set CIF_DSIWIJZ = 1
    CifTestFixture fixture(12701);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    SpecialVehicleEvent evt;
    evt.virtualLoop = 42;
    evt.vehType = 3;
    evt.lineNr = 100;
    evt.serviceNr = 200;
    evt.companyNr = 5;
    evt.vehId = 1234;
    evt.directionSG = 2;
    evt.status = 1;
    evt.priorityClass = PriorityClass::Conditional;
    evt.punctuality = PunctualityClass::OnTime;
    evt.punctualityTime = -30;
    evt.length = 12;
    evt.speed = 8;
    evt.distToStopLine = 150;
    evt.timeToStopLine = 20;
    evt.journeyNr = 5678;
    evt.journeyCat = 3;
    evt.routePT = 7;
    evt.type = AnnouncementType::Checkin;
    evt.activationPointNr = 999;
    evt.reserve23 = 11;
    evt.reserve24 = 22;

    bridge.handleSpecialVehicleEvent(evt);
    bridge.prepareDsiBuffer();

    EXPECT_EQ(CIF_DSIWIJZ, 1) << "CIF_DSIWIJZ should be 1 when event was delivered";

    EXPECT_EQ(CIF_DSI[CIF_DSI_LUS], 42);
    EXPECT_EQ(CIF_DSI[CIF_DSI_VTG], 3);
    EXPECT_EQ(CIF_DSI[CIF_DSI_LYN], 100);
    EXPECT_EQ(CIF_DSI[CIF_DSI_WDNST], 200);
    EXPECT_EQ(CIF_DSI[CIF_DSI_BEDR], 5);
    EXPECT_EQ(CIF_DSI[CIF_DSI_NUM], 1234);
    EXPECT_EQ(CIF_DSI[CIF_DSI_DIR], 2);
    EXPECT_EQ(CIF_DSI[CIF_DSI_STAT], 1);
    EXPECT_EQ(CIF_DSI[CIF_DSI_PRI], static_cast<int>(PriorityClass::Conditional));
    EXPECT_EQ(CIF_DSI[CIF_DSI_STP], static_cast<int>(PunctualityClass::OnTime));
    EXPECT_EQ(CIF_DSI[CIF_DSI_TSTP], -30);
    EXPECT_EQ(CIF_DSI[CIF_DSI_LEN], 12);
    EXPECT_EQ(CIF_DSI[CIF_DSI_SPD], 8);
    EXPECT_EQ(CIF_DSI[CIF_DSI_LSS], 150);
    EXPECT_EQ(CIF_DSI[CIF_DSI_TSS], 20);
    EXPECT_EQ(CIF_DSI[CIF_DSI_RIT], 5678);
    EXPECT_EQ(CIF_DSI[CIF_DSI_RITC], 3);
    EXPECT_EQ(CIF_DSI[CIF_DSI_ROUT], 7);
    EXPECT_EQ(CIF_DSI[CIF_DSI_TYPE], static_cast<int>(AnnouncementType::Checkin));
    EXPECT_EQ(CIF_DSI[CIF_DSI_OVC], 999);
    EXPECT_EQ(CIF_DSI[CIF_DSI_RES1], 11);
    EXPECT_EQ(CIF_DSI[CIF_DSI_RES2], 22);

    app.stop();
}

TEST(CifBridgeDsiTests, DefaultValuesForOptionalFields) {
    // When optional fields are not set, defaults should be used
    CifTestFixture fixture(12702);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    // Create an event with only the type set
    SpecialVehicleEvent evt;
    evt.type = AnnouncementType::Checkin;

    bridge.handleSpecialVehicleEvent(evt);
    bridge.prepareDsiBuffer();

    EXPECT_EQ(CIF_DSIWIJZ, 1);
    EXPECT_EQ(CIF_DSI[CIF_DSI_TYPE], static_cast<int>(AnnouncementType::Checkin));

    // Unset fields should be 0
    EXPECT_EQ(CIF_DSI[CIF_DSI_LUS], 0);
    EXPECT_EQ(CIF_DSI[CIF_DSI_VTG], 0);
    EXPECT_EQ(CIF_DSI[CIF_DSI_LYN], 0);
    EXPECT_EQ(CIF_DSI[CIF_DSI_NUM], 0);

    // Fields with non-zero defaults
    EXPECT_EQ(CIF_DSI[CIF_DSI_TSTP], CIF_DSI_TSTP_DEF)
        << "Unset punctualityTime should default to " << CIF_DSI_TSTP_DEF;
    EXPECT_EQ(CIF_DSI[CIF_DSI_LSS], CIF_DSI_LSS_DEF)
        << "Unset distToStopLine should default to " << CIF_DSI_LSS_DEF;
    EXPECT_EQ(CIF_DSI[CIF_DSI_TSS], CIF_DSI_TSS_DEF)
        << "Unset timeToStopLine should default to " << CIF_DSI_TSS_DEF;

    app.stop();
}

TEST(CifBridgeDsiTests, MultipleEventsDeliveredOnePerCycle) {
    // Multiple queued events should be delivered one per prepareDsiBuffer() call
    CifTestFixture fixture(12703);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    // Queue 3 events with distinct virtualLoop values
    for (int i = 1; i <= 3; ++i) {
        SpecialVehicleEvent evt;
        evt.virtualLoop = i * 10;
        evt.type = AnnouncementType::Checkin;
        bridge.handleSpecialVehicleEvent(evt);
    }

    // First call: should deliver event 1
    bridge.prepareDsiBuffer();
    EXPECT_EQ(CIF_DSIWIJZ, 1);
    EXPECT_EQ(CIF_DSI[CIF_DSI_LUS], 10);

    // Second call: should deliver event 2
    bridge.prepareDsiBuffer();
    EXPECT_EQ(CIF_DSIWIJZ, 1);
    EXPECT_EQ(CIF_DSI[CIF_DSI_LUS], 20);

    // Third call: should deliver event 3
    bridge.prepareDsiBuffer();
    EXPECT_EQ(CIF_DSIWIJZ, 1);
    EXPECT_EQ(CIF_DSI[CIF_DSI_LUS], 30);

    // Fourth call: queue empty
    bridge.prepareDsiBuffer();
    EXPECT_EQ(CIF_DSIWIJZ, 0) << "Queue should be empty after all events delivered";

    app.stop();
}

TEST(CifBridgeDsiTests, LocationConvertedToDms) {
    // Test decimal degrees → DMS conversion for location fields
    CifTestFixture fixture(12704);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    SpecialVehicleEvent evt;
    evt.type = AnnouncementType::Checkin;

    // Amsterdam: 52.370216, 4.895168
    tlcfi::genfi::Location loc;
    loc.latitude = 52.370216;
    loc.longitude = 4.895168;
    evt.location = loc;

    bridge.handleSpecialVehicleEvent(evt);
    bridge.prepareDsiBuffer();

    // Latitude 52.370216 → 52° 22' 12.78" (12.7776 → 12 sec, 77 hundredths)
    EXPECT_EQ(CIF_DSI[CIF_DSI_XGRAD], 52);
    EXPECT_EQ(CIF_DSI[CIF_DSI_XMIN], 22);
    EXPECT_EQ(CIF_DSI[CIF_DSI_XSEC], 12);
    EXPECT_EQ(CIF_DSI[CIF_DSI_XHSEC], 77); // 0.7776 * 100 = 77 (truncated)

    // Longitude 4.895168 → 4° 53' 42.60" (42.6048 → 42 sec, 60 hundredths)
    EXPECT_EQ(CIF_DSI[CIF_DSI_YGRAD], 4);
    EXPECT_EQ(CIF_DSI[CIF_DSI_YMIN], 53);
    EXPECT_EQ(CIF_DSI[CIF_DSI_YSEC], 42);
    EXPECT_EQ(CIF_DSI[CIF_DSI_YHSEC], 60); // 0.6048 * 100 = 60 (truncated)

    app.stop();
}

TEST(CifBridgeDsiTests, DateTimeFieldsMapped) {
    // Test DateTime fields are correctly mapped to CIF_DSI[]
    CifTestFixture fixture(12705);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    SpecialVehicleEvent evt;
    evt.type = AnnouncementType::Checkout;

    tlcfi::tlcfi::DateTime dt;
    dt.y = 2026;
    dt.m = 2;
    dt.d = 19;
    dt.h = 14;
    dt.min = 30;
    dt.s = 45;
    evt.dateTime = dt;

    bridge.handleSpecialVehicleEvent(evt);
    bridge.prepareDsiBuffer();

    EXPECT_EQ(CIF_DSI[CIF_DSI_JAAR], 2026);
    EXPECT_EQ(CIF_DSI[CIF_DSI_MND], 2);
    EXPECT_EQ(CIF_DSI[CIF_DSI_DAG], 19);
    EXPECT_EQ(CIF_DSI[CIF_DSI_UUR], 14);
    EXPECT_EQ(CIF_DSI[CIF_DSI_MIN], 30);
    EXPECT_EQ(CIF_DSI[CIF_DSI_SEC], 45);

    app.stop();
}

TEST(CifBridgeDsiTests, ResetClearsDsiQueue) {
    // Verify that reset() clears queued DSI events
    CifTestFixture fixture(12706);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    // Queue some events
    for (int i = 0; i < 5; ++i) {
        SpecialVehicleEvent evt;
        evt.virtualLoop = i;
        bridge.handleSpecialVehicleEvent(evt);
    }

    // Reset should clear the queue
    bridge.reset();

    bridge.prepareDsiBuffer();
    EXPECT_EQ(CIF_DSIWIJZ, 0) << "DSI queue should be empty after reset()";

    app.stop();
}

TEST(CifBridgeDsiTests, AllAnnouncementTypes) {
    // Test that all AnnouncementType values are correctly mapped
    CifTestFixture fixture(12707);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    struct TestCase {
        AnnouncementType type;
        int expected;
    };

    std::vector<TestCase> cases = {
        { AnnouncementType::NoInformation, 0 },
        { AnnouncementType::Checkin, 1 },
        { AnnouncementType::Checkout, 2 },
        { AnnouncementType::PreCheckin, 3 },
    };

    for (const auto& tc : cases) {
        SpecialVehicleEvent evt;
        evt.type = tc.type;
        bridge.handleSpecialVehicleEvent(evt);
        bridge.prepareDsiBuffer();
        EXPECT_EQ(CIF_DSI[CIF_DSI_TYPE], tc.expected)
            << "AnnouncementType " << tc.expected << " should map correctly";
    }

    app.stop();
}

TEST(CifBridgeDsiTests, PreviousBufferClearedOnNewEvent) {
    // Verify that CIF_DSI[] is fully cleared before writing new event data,
    // so fields from a previous event don't bleed through
    CifTestFixture fixture(12708);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    // First event: fully populated
    SpecialVehicleEvent evt1;
    evt1.virtualLoop = 99;
    evt1.vehType = 5;
    evt1.lineNr = 300;
    evt1.type = AnnouncementType::Checkin;
    bridge.handleSpecialVehicleEvent(evt1);

    // Second event: minimal (only type)
    SpecialVehicleEvent evt2;
    evt2.type = AnnouncementType::Checkout;
    bridge.handleSpecialVehicleEvent(evt2);

    // Deliver first event
    bridge.prepareDsiBuffer();
    EXPECT_EQ(CIF_DSI[CIF_DSI_LUS], 99);
    EXPECT_EQ(CIF_DSI[CIF_DSI_LYN], 300);

    // Deliver second event — previous values should be cleared
    bridge.prepareDsiBuffer();
    EXPECT_EQ(CIF_DSI[CIF_DSI_LUS], 0) << "virtualLoop from previous event should be cleared";
    EXPECT_EQ(CIF_DSI[CIF_DSI_VTG], 0) << "vehType from previous event should be cleared";
    EXPECT_EQ(CIF_DSI[CIF_DSI_LYN], 0) << "lineNr from previous event should be cleared";
    EXPECT_EQ(CIF_DSI[CIF_DSI_TYPE], static_cast<int>(AnnouncementType::Checkout));

    app.stop();
}

// ============================================================================
// CIF BRIDGE FILE VLOG PRUNING TESTS
// ============================================================================

TEST(CifBridgeTests, FileVlogPrunesOldFilesWhenMaxFilesExceeded) {
    // Pre-seed maxFiles .vlg files with past timestamps, then drive one rotation
    // through the ring buffer. The rotation finalizes the active .tmp → .vlg
    // (total maxFiles + 1), and pruneOldVlogFiles removes the oldest, leaving
    // exactly maxFiles files on disk.
    CifTestFixture fixture(12607);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    auto testDir = std::filesystem::temp_directory_path() / "openitscontainer_vlog_prune_test";
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
    std::filesystem::create_directories(testDir, ec);
    ASSERT_FALSE(ec) << "Failed to create test directory: " << ec.message();

    const std::string systemName = "PruneSystem";
    const int maxFiles = 2;

    // Pre-create maxFiles .vlg files with ascending past timestamps so they sort
    // before today's file and are identified as the oldest by pruneOldVlogFiles().
    for (int i = 0; i < maxFiles; ++i) {
        const std::string name = systemName + "_20260101_00000" + std::to_string(i) + ".vlg";
        std::ofstream f(testDir / name);
        f << "old log " << i;
    }

    bridge.configureFileLogging(true, testDir.string(), systemName, maxFiles);

    // Drive a file-switch through the ring: pollFileVlog() hits the FILEWISSEL
    // marker → rotateFile() (finalizes nothing yet — no active .tmp) → opens
    // new .tmp and writes the payload to it.
    setupVlogRingForRotation("TIME 2026-07-31 12:00:00\r\n");
    bridge.pollFileVlog();

    // reset() finalizes the active .tmp → .vlg, then calls pruneOldVlogFiles()
    // because maxFiles > 0. Total before pruning: maxFiles + 1 → after: maxFiles.
    bridge.reset();

    int vlgCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(testDir, ec)) {
        if (entry.path().extension() == ".vlg")
            ++vlgCount;
    }

    EXPECT_EQ(vlgCount, maxFiles)
        << "pruneOldVlogFiles should leave exactly maxFiles .vlg files";

    std::filesystem::remove_all(testDir, ec);
    app.stop();
}

TEST(CifBridgeTests, FileVlogMaxFilesZero_DoesNotPrune) {
    // With cifFileLogMaxFiles == 0 (the default / unlimited sentinel),
    // no pruning occurs regardless of how many .vlg files exist.
    CifTestFixture fixture(12608);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    cif::CifBridge bridge(app);

    auto testDir = std::filesystem::temp_directory_path() / "openitscontainer_vlog_noprune_test";
    std::error_code ec;
    std::filesystem::remove_all(testDir, ec);
    std::filesystem::create_directories(testDir, ec);
    ASSERT_FALSE(ec) << "Failed to create test directory: " << ec.message();

    const std::string systemName = "NoPruneSystem";
    const int seedFiles = 3;

    for (int i = 0; i < seedFiles; ++i) {
        const std::string name = systemName + "_20260101_00000" + std::to_string(i) + ".vlg";
        std::ofstream f(testDir / name);
        f << "old log " << i;
    }

    bridge.configureFileLogging(true, testDir.string(), systemName, /*maxFiles=*/0);

    setupVlogRingForRotation("TIME 2026-07-31 12:00:00\r\n");
    bridge.pollFileVlog();
    bridge.reset();  // adds one more .vlg, but maxFiles=0 means no pruning

    int vlgCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(testDir, ec)) {
        if (entry.path().extension() == ".vlg")
            ++vlgCount;
    }

    EXPECT_EQ(vlgCount, seedFiles + 1)
        << "With maxFiles=0 (unlimited), all .vlg files should be kept";

    std::filesystem::remove_all(testDir, ec);
    app.stop();
}
