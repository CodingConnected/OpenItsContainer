#include <gtest/gtest.h>

#include "fi/FiConsumerApp.hpp"
#include "fi/FiControllerApp.hpp"
#include "fi/FiFacilitiesServer.hpp"
#include "models/tlc-fi/Enums.hpp"

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

// Functional TLC-FI tests (remediation plan Phase 4.4):
//   - NotifyEvent / event-dispatch path;
//   - server protocol-error responses (registration rejection);
//   - watchdog timeout disconnect (no-registration timeout);
//   - server-triggered EndControl handling.

using nlohmann::json;
using tlcfi::fi::FiConsumerApp;
using tlcfi::fi::FiControllerApp;
using tlcfi::fi::FiFacilitiesServer;
namespace tlc = tlcfi::tlcfi;
namespace genfi = tlcfi::genfi;

namespace {

// Runs a FiFacilitiesServer on its own io_context thread for the test.
struct ServerFixture {
    asio::io_context io;
    std::unique_ptr<FiFacilitiesServer> server;
    std::thread ioThread;
    unsigned short port;

    // The port argument is ignored: the server binds an OS-assigned
    // ephemeral port (port 0) to avoid fixed-port collisions across tests.
    explicit ServerFixture(unsigned short = 0) {
        server = std::make_unique<FiFacilitiesServer>(io, 0);
        port = server->port();
        server->start();
        ioThread = std::thread([this]() { io.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ~ServerFixture() {
        server->stop();
        io.stop();
        if (ioThread.joinable()) {
            ioThread.join();
        }
    }

    template <typename Predicate>
    bool waitFor(Predicate&& pred,
                 std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
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

// Exposes the protected event-dispatch entry point so a test can inject a
// NotifyEvent group exactly as the JSON-RPC notification path would.
class TestableConsumerApp : public FiConsumerApp {
public:
    using FiConsumerApp::FiConsumerApp;
    void injectEvent(const genfi::NotifyEventGroup& g) { processEventUpdates(g); }
};

// Exposes the protected Session-update entry point so a test can simulate the
// server sending a Session UpdateState (e.g. controlState = EndControl).
class TestableControllerApp : public FiControllerApp {
public:
    using FiControllerApp::FiControllerApp;
    void injectSessionUpdate(const genfi::ObjectStateUpdateGroup& g) {
        processSessionUpdate(g);
    }
};

} // namespace

// ============================================================================
// NotifyEvent / event path
// ============================================================================

TEST(FiFunctionalTests, NotifyEventDispatchesSpecialVehicleEvent) {
    TestableConsumerApp app("127.0.0.1", "1", "u", "p", "uri");

    bool called = false;
    tlc::SpecialVehicleEvent received;
    app.onSpecialVehicleEvent = [&](const tlc::SpecialVehicleEvent& e) {
        called = true;
        received = e;
    };

    genfi::NotifyEventGroup g;
    g.objects.type = static_cast<int>(tlc::TLCObjectType::SpecialVehicleEventGenerator);
    g.objects.ids = {"SPV1"};
    g.events.push_back(json{{"lineNr", 42}, {"companyNr", 7}});

    app.injectEvent(g);

    EXPECT_TRUE(called) << "onSpecialVehicleEvent must fire for a SPV-generator NotifyEvent";
    ASSERT_TRUE(received.lineNr.has_value());
    EXPECT_EQ(*received.lineNr, 42);
    ASSERT_TRUE(received.companyNr.has_value());
    EXPECT_EQ(*received.companyNr, 7);
}

TEST(FiFunctionalTests, NotifyEventDispatchesEveryEventInGroup) {
    TestableConsumerApp app("127.0.0.1", "1", "u", "p", "uri");

    int count = 0;
    app.onSpecialVehicleEvent = [&](const tlc::SpecialVehicleEvent&) { ++count; };

    genfi::NotifyEventGroup g;
    g.objects.type = static_cast<int>(tlc::TLCObjectType::SpecialVehicleEventGenerator);
    g.objects.ids = {"SPV1"};
    g.events.push_back(json{{"lineNr", 1}});
    g.events.push_back(json{{"lineNr", 2}});
    g.events.push_back(json{{"lineNr", 3}});

    app.injectEvent(g);

    EXPECT_EQ(count, 3) << "every event in the group must be dispatched";
}

TEST(FiFunctionalTests, NotifyEventForUnhandledTypeIsIgnored) {
    TestableConsumerApp app("127.0.0.1", "1", "u", "p", "uri");

    bool called = false;
    app.onSpecialVehicleEvent = [&](const tlc::SpecialVehicleEvent&) { called = true; };

    // A NotifyEvent for a non-event object type must be silently ignored.
    genfi::NotifyEventGroup g;
    g.objects.type = static_cast<int>(tlc::TLCObjectType::Detector);
    g.objects.ids = {"DET1"};
    g.events.push_back(json{{"lineNr", 99}});

    app.injectEvent(g);

    EXPECT_FALSE(called);
}

// ============================================================================
// Server protocol-error responses
// ============================================================================

TEST(FiFunctionalTests, ServerRejectsSecondRegistrationForSameUsername) {
    // The server raises an FiProtocolError (converted to a JSON-RPC error) when
    // a username is registered twice. The second registerApp() must fail.
    ServerFixture fixture(12540);

    FiConsumerApp first("127.0.0.1", std::to_string(fixture.port), "dupuser", "pw", "uri-a");
    ASSERT_TRUE(first.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(first.registerApp(std::chrono::milliseconds(2000)));

    FiConsumerApp second("127.0.0.1", std::to_string(fixture.port), "dupuser", "pw", "uri-b");
    ASSERT_TRUE(second.connect(std::chrono::milliseconds(2000)));
    EXPECT_FALSE(second.registerApp(std::chrono::milliseconds(2000)))
        << "the server must reject a duplicate username registration";

    first.stop();
    second.stop();
}

// ============================================================================
// Watchdog timeout disconnect
// ============================================================================

TEST(FiFunctionalTests, ServerDropsUnregisteredConnectionAfterTimeout) {
    // A client that connects but never registers must be dropped by the
    // server's no-registration watchdog (NO_REGISTRATION_TIMEOUT = 10s, the
    // timeout monitor sweeps every 1s).
    ServerFixture fixture(12541);

    asio::io_context clientIo;
    asio::ip::tcp::socket socket(clientIo);
    std::error_code ec;
    socket.connect(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), fixture.port), ec);
    ASSERT_FALSE(ec) << "raw client failed to connect: " << ec.message();

    // A read completes with an error (EOF/reset) once the server closes the
    // unregistered connection.
    bool closed = false;
    std::array<char, 64> buf{};
    socket.async_read_some(asio::buffer(buf),
        [&](std::error_code readEc, std::size_t) {
            if (readEc) closed = true;
        });

    clientIo.run_for(std::chrono::seconds(15));

    EXPECT_TRUE(closed)
        << "server must close a connection that never completes registration";
}

// ============================================================================
// Server-triggered EndControl
// ============================================================================

TEST(FiFunctionalTests, ServerTriggeredEndControlInvokesCallback) {
    // Drive a controller app to InControl, then simulate the server sending a
    // Session UpdateState with controlState = EndControl. The controller must
    // invoke onControlEndControl with the intersection id.
    ServerFixture fixture(12542);

    std::atomic<bool> inControl{false};
    std::atomic<bool> endControl{false};
    std::string endIntersection;

    TestableControllerApp app("127.0.0.1", std::to_string(fixture.port),
                              "ctrluser", "pw", "uri");
    app.onControlInControl = [&](const std::string&) { inControl = true; };
    app.onControlEndControl = [&](const std::string& id) {
        endControl = true;
        endIntersection = id;
    };

    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));

    ASSERT_TRUE(fixture.waitFor([&inControl]() { return inControl.load(); },
                                std::chrono::milliseconds(5000)))
        << "controller did not reach InControl";

    auto sessionId = app.getSessionId();
    ASSERT_TRUE(sessionId.has_value());

    // Build the Session UpdateState the server would send to take back control.
    genfi::ObjectStateUpdate upd;
    upd.objects.type = static_cast<int>(tlc::TLCObjectType::Session);
    upd.objects.ids = {*sessionId};
    upd.states.push_back(json{
        {"controlState", static_cast<int>(tlc::ControlState::EndControl)},
        {"reqIntersection", "INT-1"}});
    genfi::ObjectStateUpdateGroup g;
    g.update.push_back(std::move(upd));

    app.injectSessionUpdate(g);

    EXPECT_TRUE(endControl.load()) << "onControlEndControl must fire on EndControl";
    EXPECT_FALSE(endIntersection.empty());

    app.stop();
}
