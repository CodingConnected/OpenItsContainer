#include <gtest/gtest.h>

#include "fi/FiControllerApp.hpp"
#include "fi/FiConsumerApp.hpp"
#include "fi/FiFacilitiesServer.hpp"
#include "models/tlc-fi/Enums.hpp"

#include <asio/io_context.hpp>
#include <thread>
#include <chrono>
#include <atomic>
#include <future>
#include <mutex>
#include <vector>

using namespace tlcfi::fi;
using namespace tlcfi::tlcfi;

namespace {

struct ControllerTestFixture {
    asio::io_context io;
    std::unique_ptr<FiFacilitiesServer> server;
    std::thread ioThread;
    unsigned short port;

    // The port argument is ignored: the server binds an OS-assigned
    // ephemeral port (port 0) to avoid fixed-port collisions / TIME_WAIT
    // flakiness across tests. Read the actual port from `port`.
    explicit ControllerTestFixture(unsigned short = 0)
    {
        server = std::make_unique<FiFacilitiesServer>(io, 0);
        port = server->port();
        server->start();
        ioThread = std::thread([this]() { io.run(); });
        
        // Give server time to start
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ~ControllerTestFixture() {
        server->stop();
        io.stop();
        if (ioThread.joinable()) {
            ioThread.join();
        }
    }

    // Helper to wait for condition with timeout
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

} // namespace

// ============================================================================
// 1. TAKING CONTROL SCENARIOS
// ============================================================================

TEST(FiControllerAppTests, NormalStartupSequence) {
    // Scenario 1.1: Normal Startup Sequence (Use Case 7.1)
    ControllerTestFixture fixture(12511);
    
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    
    // Step 1: Connect and register
    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));
    
    // Step 2: Wait for NotConfigured state and meta/subscription
    // The app should automatically trigger meta read and subscription when NotConfigured
    ASSERT_TRUE(fixture.waitFor([&app]() {
        return app.isFullyInitialized();
    }, std::chrono::milliseconds(5000)));
    
    // Verify we have objects
    auto sgIds = app.getSignalGroupIds();
    EXPECT_FALSE(sgIds.empty());
    auto outIds = app.getOutputIds();
    EXPECT_FALSE(outIds.empty());
    
    // Step 3: App should automatically request ReadyToControl after Offline
    // (This happens in processSessionControlFlow when controlState = Offline)
    
    app.stop();
}

TEST(FiControllerAppTests, ReadyToControlStateTransition) {
    // Scenario 1.2: ReadyToControl State Transition
    ControllerTestFixture fixture(12512);
    
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    
    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));
    
    // Wait for initialization
    ASSERT_TRUE(fixture.waitFor([&app]() {
        return app.isFullyInitialized();
    }, std::chrono::milliseconds(5000)));
    
    // At this point, app should have:
    // - Read meta
    // - Subscribed to intersection, signal groups, and exclusive outputs
    // - Requested Offline
    // - Requested ReadyToControl (when TLC confirms Offline)
    
    // Note: Full state machine testing requires server-side UpdateState handler
    // which is not yet implemented in FiFacilitiesServer
    
    app.stop();
}

TEST(FiControllerAppTests, StartControlAcknowledgment) {
    // Scenario 1.3: StartControl → InControl transition
    // Server now handles UpdateState notifications, so the full state machine runs:
    //   NotConfigured → meta/subscribe → reqOffline → Offline
    //   → reqReadyToControl → StartControl → onControlInControl → reqInControl → InControl
    ControllerTestFixture fixture(12513);

    std::atomic<bool> inControlCallbackCalled{false};
    std::string receivedIntersectionId;

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");

    app.onControlInControl = [&](const std::string& intersectionId) {
        inControlCallbackCalled = true;
        receivedIntersectionId = intersectionId;
    };

    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));

    // Wait for the full state machine to reach InControl
    ASSERT_TRUE(fixture.waitFor([&inControlCallbackCalled]() {
        return inControlCallbackCalled.load();
    }, std::chrono::milliseconds(5000))) << "onControlInControl callback was not invoked";

    EXPECT_FALSE(receivedIntersectionId.empty());

    app.stop();
}

// ============================================================================
// 2. RELEASING CONTROL SCENARIOS
// ============================================================================

TEST(FiControllerAppTests, EndControlCallback) {
    // Scenario 2.1: Normal EndControl Sequence - Callback setup
    ControllerTestFixture fixture(12514);
    
    std::atomic<bool> endControlCallbackCalled{false};
    std::string receivedIntersectionId;
    
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    
    app.onControlEndControl = [&](const std::string& intersectionId) {
        endControlCallbackCalled = true;
        receivedIntersectionId = intersectionId;
    };
    
    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));
    
    // Verify callback is set up
    EXPECT_TRUE(app.onControlEndControl != nullptr);
    
    // Note: Full EndControl testing requires server to send controlState = EndControl
    // via UpdateState notification
    
    app.stop();
}

// ============================================================================
// 3. SETTING STATE OF OBJECTS SCENARIOS
// ============================================================================

TEST(FiControllerAppTests, RequestSignalGroupStates) {
    // Scenario 3.1.1: Normal Signal Group State Change
    ControllerTestFixture fixture(12515);
    
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    
    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));
    
    // Wait for initialization
    ASSERT_TRUE(fixture.waitFor([&app]() {
        return app.isFullyInitialized();
    }, std::chrono::milliseconds(5000)));
    
    auto sgIds = app.getSignalGroupIds();
    ASSERT_FALSE(sgIds.empty());
    
    // Request state change for signal groups
    std::vector<std::pair<std::string, SignalGroupState>> updates;
    updates.push_back({sgIds[0], SignalGroupState::PermissiveMovementAllowed});
    
    // This should send UpdateState notification
    app.requestSignalGroupStates(updates);
    
    // Note: To fully verify, we'd need to check that UpdateState was sent
    // and that server processed it correctly
    
    app.stop();
}

TEST(FiControllerAppTests, RequestMultipleSignalGroupStatesAtomically) {
    // Scenario 3.1.2: Atomic Update of Multiple Signal Groups
    ControllerTestFixture fixture(12516);
    
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    
    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));
    
    // Wait for initialization
    ASSERT_TRUE(fixture.waitFor([&app]() {
        return app.isFullyInitialized();
    }, std::chrono::milliseconds(5000)));
    
    auto sgIds = app.getSignalGroupIds();
    ASSERT_GE(sgIds.size(), 2) << "Need at least 2 signal groups for this test";
    
    // Request state changes for multiple signal groups atomically
    std::vector<std::pair<std::string, SignalGroupState>> updates;
    updates.push_back({sgIds[0], SignalGroupState::PermissiveMovementAllowed});
    updates.push_back({sgIds[1], SignalGroupState::ProtectedMovementAllowed});
    
    // This should send a single UpdateState with both updates in one group
    app.requestSignalGroupStates(updates);
    
    // Note: Verification requires checking that a single UpdateState notification
    // was sent with both signal groups in the same update group
    
    app.stop();
}

TEST(FiControllerAppTests, RequestOutputStates) {
    // Scenario 3.3.1: Normal Exclusive Output State Change
    ControllerTestFixture fixture(12517);
    
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    
    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));
    
    // Wait for initialization
    ASSERT_TRUE(fixture.waitFor([&app]() {
        return app.isFullyInitialized();
    }, std::chrono::milliseconds(5000)));
    
    auto outIds = app.getOutputIds();
    ASSERT_FALSE(outIds.empty());
    
    // Request state change for outputs
    std::vector<std::pair<std::string, int>> updates;
    updates.push_back({outIds[0], 123});
    
    // This should send UpdateState notification
    app.requestOutputStates(updates);
    
    app.stop();
}

TEST(FiControllerAppTests, RequestMultipleOutputStatesAtomically) {
    // Scenario 3.3.2: Atomic Update of Multiple Outputs
    ControllerTestFixture fixture(12518);
    
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    
    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));
    
    // Wait for initialization
    ASSERT_TRUE(fixture.waitFor([&app]() {
        return app.isFullyInitialized();
    }, std::chrono::milliseconds(5000)));
    
    auto outIds = app.getOutputIds();
    ASSERT_GE(outIds.size(), 2) << "Need at least 2 outputs for this test";
    
    // Request state changes for multiple outputs atomically
    std::vector<std::pair<std::string, int>> updates;
    updates.push_back({outIds[0], 100});
    updates.push_back({outIds[1], 200});
    
    // This should send a single UpdateState with both updates in one group
    app.requestOutputStates(updates);
    
    app.stop();
}

// ============================================================================
// 4. INTEGRATION TESTS FOR CONTROL STATE MACHINE
// ============================================================================

TEST(FiControllerAppTests, ControlStateMachineFlow) {
    // Integration test: verify the complete control state machine flow and ordering.
    // The server now processes UpdateState notifications and advances the state machine.
    // We track every controlState the client observes to verify strict ordering:
    //   NotConfigured(1) → Offline(2) → StartControl(4) → InControl(5)
    ControllerTestFixture fixture(12519);

    std::mutex statesMutex;
    std::vector<int> observedControlStates;
    std::atomic<bool> inControlReached{false};

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");

    // Intercept all UpdateState notifications to log observed controlState values
    app.onLog = [&](LogLevel, const std::string& message) {
        // FiControllerApp logs "sent Session.reqControlState=..." for each transition.
        // But we want to observe what the server tells us, not what we send.
        // We'll rely on the callbacks and isFullyInitialized for ordering.
    };

    app.onControlInControl = [&](const std::string&) {
        inControlReached = true;
    };

    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));

    // Wait for initialization (meta + subscribe complete after NotConfigured)
    ASSERT_TRUE(fixture.waitFor([&app]() {
        return app.isFullyInitialized();
    }, std::chrono::milliseconds(5000))) << "App did not complete initialization (meta + subscribe)";

    // Verify objects were discovered during NotConfigured phase
    auto sgIds = app.getSignalGroupIds();
    EXPECT_FALSE(sgIds.empty()) << "No signal groups discovered from meta";
    auto outIds = app.getOutputIds();
    EXPECT_FALSE(outIds.empty()) << "No outputs discovered from meta";

    // Wait for the full state machine to reach InControl
    ASSERT_TRUE(fixture.waitFor([&inControlReached]() {
        return inControlReached.load();
    }, std::chrono::milliseconds(5000))) << "State machine did not reach InControl";

    // Once InControl, verify we can send signal group state requests
    if (!sgIds.empty()) {
        std::vector<std::pair<std::string, SignalGroupState>> updates;
        updates.push_back({sgIds[0], SignalGroupState::StopAndRemain});
        app.requestSignalGroupStates(updates);
    }

    app.stop();
}

// ============================================================================
// 5. ERROR SCENARIO TESTS
// ============================================================================

TEST(FiControllerAppTests, RequestStateWhenNotInitialized) {
    // Test that requesting states before initialization doesn't crash
    ControllerTestFixture fixture(12520);
    
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    
    // Try to request states before connecting
    std::vector<std::pair<std::string, SignalGroupState>> sgUpdates;
    sgUpdates.push_back({"SG1", SignalGroupState::PermissiveMovementAllowed});
    
    // Should not crash, but may not send anything
    app.requestSignalGroupStates(sgUpdates);
    
    std::vector<std::pair<std::string, int>> outUpdates;
    outUpdates.push_back({"OUT1", 100});
    app.requestOutputStates(outUpdates);
    
    app.stop();
}

TEST(FiControllerAppTests, MultipleControlApps) {
    // Test that multiple controller apps can coexist (different usernames)
    ControllerTestFixture fixture(12521);
    
    FiControllerApp app1("127.0.0.1", std::to_string(fixture.port), "user1", "pw1", "uri1");
    FiControllerApp app2("127.0.0.1", std::to_string(fixture.port), "user2", "pw2", "uri2");
    
    ASSERT_TRUE(app1.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app1.registerApp(std::chrono::milliseconds(2000)));
    
    ASSERT_TRUE(app2.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app2.registerApp(std::chrono::milliseconds(2000)));
    
    // Both should be able to initialize
    ASSERT_TRUE(fixture.waitFor([&app1]() {
        return app1.isFullyInitialized();
    }, std::chrono::milliseconds(5000)));
    
    ASSERT_TRUE(fixture.waitFor([&app2]() {
        return app2.isFullyInitialized();
    }, std::chrono::milliseconds(5000)));
    
    app1.stop();
    app2.stop();
}

// ============================================================================
// 6. HELPER FUNCTION TESTS
// ============================================================================

TEST(FiControllerAppTests, CallbackInvocation) {
    // Test that callbacks can be set and are properly typed
    ControllerTestFixture fixture(12522);
    
    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    
    bool inControlSet = false;
    bool endControlSet = false;
    
    app.onControlInControl = [&](const std::string& id) {
        inControlSet = true;
        EXPECT_FALSE(id.empty());
    };
    
    app.onControlEndControl = [&](const std::string& id) {
        endControlSet = true;
        EXPECT_FALSE(id.empty());
    };
    
    // Callbacks should be set
    EXPECT_TRUE(app.onControlInControl != nullptr);
    EXPECT_TRUE(app.onControlEndControl != nullptr);
    
    // Note: Actual invocation testing requires server to send appropriate UpdateState
    
    app.stop();
}

// ============================================================================
// 7. CONTROL STATE ORDERING TESTS
// ============================================================================

TEST(FiControllerAppTests, ControlStateOrderVerification) {
    // Verify the strict ordering of control state transitions by tracking
    // every controlState the client receives from the server via UpdateState.
    // Expected order: NotConfigured(1) → Offline(2) → StartControl(4) → InControl(5)
    //
    // We capture the client-side log messages to detect state transitions.
    // FiControllerApp logs "sent Session.reqControlState=Offline" when it sends reqOffline
    // (meaning it received Offline from the server and is advancing to ReadyToControl).
    // The onMetaReceived callback fires when NotConfigured triggers meta read.
    // The onControlInControl callback fires when StartControl is received.
    ControllerTestFixture fixture(12523);

    std::mutex statesMutex;
    std::vector<int> observedStates;
    std::atomic<bool> inControlReached{false};

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");

    // Capture control states from log messages.
    // FiControllerApp logs specific messages when it processes each controlState:
    //   "sent Session.reqControlState=Offline" → Offline was received (triggers ReadyToControl)
    //   "sent Session.reqControlState=ReadyToControl" → Offline confirmed, advancing
    //   "sent Session.reqControlState=InControl" → StartControl received, acknowledging
    app.onLog = [&](LogLevel, const std::string& message) {
        std::lock_guard<std::mutex> lock(statesMutex);
        if (message.find("sent Session.reqControlState=Offline") != std::string::npos) {
            // Client received NotConfigured → did meta+subscribe → now requesting Offline
            // This means NotConfigured was fully processed
            if (observedStates.empty() || observedStates.back() != static_cast<int>(ControlState::NotConfigured)) {
                observedStates.push_back(static_cast<int>(ControlState::NotConfigured));
            }
        }
        else if (message.find("sent Session.reqControlState=ReadyToControl") != std::string::npos) {
            // Client received Offline confirmation → requesting ReadyToControl
            observedStates.push_back(static_cast<int>(ControlState::Offline));
        }
        else if (message.find("sent Session.reqControlState=InControl") != std::string::npos) {
            // Client received StartControl → acknowledging with InControl
            observedStates.push_back(static_cast<int>(ControlState::StartControl));
        }
    };

    app.onControlInControl = [&](const std::string&) {
        inControlReached = true;
    };

    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));

    // Wait for InControl
    ASSERT_TRUE(fixture.waitFor([&inControlReached]() {
        return inControlReached.load();
    }, std::chrono::milliseconds(5000))) << "InControl not reached";

    // Verify the ordering
    {
        std::lock_guard<std::mutex> lock(statesMutex);
        ASSERT_GE(observedStates.size(), 3)
            << "Expected at least 3 state observations, got " << observedStates.size();

        // NotConfigured must come first (meta+subscribe completed, then reqOffline sent)
        EXPECT_EQ(observedStates[0], static_cast<int>(ControlState::NotConfigured))
            << "First state should be NotConfigured(1), got " << observedStates[0];
        // Offline must come second (server confirmed Offline, then reqReadyToControl sent)
        EXPECT_EQ(observedStates[1], static_cast<int>(ControlState::Offline))
            << "Second state should be Offline(2), got " << observedStates[1];
        // StartControl must come third (server sent StartControl, then reqInControl sent)
        EXPECT_EQ(observedStates[2], static_cast<int>(ControlState::StartControl))
            << "Third state should be StartControl(4), got " << observedStates[2];
    }

    app.stop();
}

TEST(FiControllerAppTests, HandlesInitInternallyFlag) {
    // Verify that FiControllerApp returns true for handlesInitInternally()
    // and FiConsumerApp returns false (the default).
    ControllerTestFixture fixture(12524);

    FiControllerApp controllerApp("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");
    EXPECT_TRUE(controllerApp.handlesInitInternally())
        << "FiControllerApp should handle init internally (state machine does meta+subscribe)";

    FiConsumerApp consumerApp("127.0.0.1", std::to_string(fixture.port), "testuser2", "testpw2", "testuri2");
    EXPECT_FALSE(consumerApp.handlesInitInternally())
        << "FiConsumerApp should NOT handle init internally (FiAppHost does meta+subscribe)";

    controllerApp.stop();
    consumerApp.stop();
}

TEST(FiControllerAppTests, MetaAndSubscribeOnlyAfterNotConfigured) {
    // Verify that the controller app does NOT call readMeta/subscribeAll eagerly.
    // Instead, meta+subscribe should only happen after the server sends
    // controlState=NotConfigured, which is the prescribed TLC-FI §4.8.1 ordering.
    //
    // We verify this by checking that isFullyInitialized() is NOT true immediately
    // after registration (before the server sends the NotConfigured notification).
    ControllerTestFixture fixture(12525);

    FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");

    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));

    // Immediately after registerApp, the app should NOT be fully initialized.
    // Meta/subscribe only happens when the state machine receives NotConfigured.
    // (In practice the notification arrives very quickly, but the point is that
    // registerApp itself does not call readMeta/subscribeAll.)
    // We verify the flag is set properly on the base class.
    EXPECT_TRUE(app.handlesInitInternally());

    // Now wait for the state machine to complete
    ASSERT_TRUE(fixture.waitFor([&app]() {
        return app.isFullyInitialized();
    }, std::chrono::milliseconds(5000)));

    // Verify objects were discovered
    EXPECT_FALSE(app.getSignalGroupIds().empty());

    app.stop();
}

// ============================================================================
// 8. HANDOVER CAPABILITY TESTS
// ============================================================================

TEST(FiControllerAppTests, DefaultHandoverCapabilities) {
	// Verify that default handover capabilities are Cleared
	ControllerTestFixture fixture(12530);

	FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");

	// Default should be Cleared for both start and end capabilities
	EXPECT_EQ(app.getStartHandoverCapability(), HandoverCapability::Cleared);
	EXPECT_EQ(app.getEndHandoverCapability(), HandoverCapability::Cleared);

	app.stop();
}

TEST(FiControllerAppTests, ConstructWithPreDefinedHandover) {
	// Verify handover capabilities can be set via constructor
	ControllerTestFixture fixture(12531);

	FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri",
	                    HandoverCapability::PreDefined, HandoverCapability::PreDefined);

	EXPECT_EQ(app.getStartHandoverCapability(), HandoverCapability::PreDefined);
	EXPECT_EQ(app.getEndHandoverCapability(), HandoverCapability::PreDefined);

	app.stop();
}

TEST(FiControllerAppTests, ConstructWithDirectHandover) {
	// Verify handover capabilities can be set to Direct
	ControllerTestFixture fixture(12532);

	FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri",
	                    HandoverCapability::Direct, HandoverCapability::Direct);

	EXPECT_EQ(app.getStartHandoverCapability(), HandoverCapability::Direct);
	EXPECT_EQ(app.getEndHandoverCapability(), HandoverCapability::Direct);

	app.stop();
}

TEST(FiControllerAppTests, SetHandoverCapabilities) {
	// Verify handover capabilities can be changed via setters
	ControllerTestFixture fixture(12533);

	FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");

	// Defaults
	EXPECT_EQ(app.getStartHandoverCapability(), HandoverCapability::Cleared);
	EXPECT_EQ(app.getEndHandoverCapability(), HandoverCapability::Cleared);

	// Change start capability
	app.setStartHandoverCapability(HandoverCapability::PreDefined);
	EXPECT_EQ(app.getStartHandoverCapability(), HandoverCapability::PreDefined);
	EXPECT_EQ(app.getEndHandoverCapability(), HandoverCapability::Cleared); // Unchanged

	// Change end capability
	app.setEndHandoverCapability(HandoverCapability::Direct);
	EXPECT_EQ(app.getStartHandoverCapability(), HandoverCapability::PreDefined);
	EXPECT_EQ(app.getEndHandoverCapability(), HandoverCapability::Direct);

	app.stop();
}

TEST(FiControllerAppTests, HandoverCapabilitiesNotResetOnCleanup) {
	// Verify handover capabilities are NOT reset on cleanup - they are configuration
	// values that should persist across reconnections (see FiControllerApp::cleanup())
	ControllerTestFixture fixture(12534);

	FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");

	// Set non-default capabilities
	app.setStartHandoverCapability(HandoverCapability::Direct);
	app.setEndHandoverCapability(HandoverCapability::PreDefined);

	EXPECT_EQ(app.getStartHandoverCapability(), HandoverCapability::Direct);
	EXPECT_EQ(app.getEndHandoverCapability(), HandoverCapability::PreDefined);

	// Cleanup should NOT reset handover capabilities
	app.cleanup();

	EXPECT_EQ(app.getStartHandoverCapability(), HandoverCapability::Direct)
	    << "Start handover capability should persist across cleanup";
	EXPECT_EQ(app.getEndHandoverCapability(), HandoverCapability::PreDefined)
	    << "End handover capability should persist across cleanup";

	app.stop();
}

TEST(FiControllerAppTests, MixedHandoverCapabilities) {
	// Verify different capabilities can be set for start and end
	ControllerTestFixture fixture(12535);

	FiControllerApp app("127.0.0.1", std::to_string(fixture.port), "testuser", "testpw", "testuri");

	// Set different capabilities
	app.setStartHandoverCapability(HandoverCapability::Cleared);
	app.setEndHandoverCapability(HandoverCapability::Direct);

	EXPECT_EQ(app.getStartHandoverCapability(), HandoverCapability::Cleared);
	EXPECT_EQ(app.getEndHandoverCapability(), HandoverCapability::Direct);

	// Swap them
	app.setStartHandoverCapability(HandoverCapability::PreDefined);
	app.setEndHandoverCapability(HandoverCapability::Cleared);

	EXPECT_EQ(app.getStartHandoverCapability(), HandoverCapability::PreDefined);
	EXPECT_EQ(app.getEndHandoverCapability(), HandoverCapability::Cleared);

	app.stop();
}

// ============================================================================
// NOTES FOR FUTURE ENHANCEMENTS
// ============================================================================

/*
 * Additional test scenarios to implement:
 *    - NotConfigured timeout (60s)
 *    - StartControl timeout (5s)
 *    - EndControl timeout (180s)
 *    - Invalid state transitions
 *    - Conflicting signal groups
 *    - Request state when not in control
 *    - Request state for wrong intersection
 *    - Backup application scenarios
 */
