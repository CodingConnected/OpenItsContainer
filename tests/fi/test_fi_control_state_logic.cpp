#include <gtest/gtest.h>

#include "fi/FiControllerApp.hpp"
#include "fi/FiFacilitiesServer.hpp"
#include "models/tlc-fi/Enums.hpp"

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Client-side (ITS-CLA) coverage of the TLC-FI 2.0.1 §4.7.2 control-state
// decision tables (Tables 1-6) for branches the simplified FiFacilitiesServer
// stub never produces.
//
// The stub server only drives the forward happy path
// (NotConfigured -> Offline -> StartControl -> InControl) and then goes inert:
// it stays InControl and sends nothing further for that session. That makes
// post-InControl injection via TestableControllerApp deterministic. Injecting
// EndControl additionally resets the client's control-flow guards
// (inControlRequested_ / waitingForIntersectionControl_ / readyToControlRequested_,
// see FiControllerApp.cpp:276-283), which lets a test drive a second, fully
// controlled control cycle with arbitrary intersection state.
//
// See knowledge notes: facilities-server-collapsed-state-machine,
// control-state-table-test-coverage-gaps.

using nlohmann::json;
using tlcfi::fi::FiControllerApp;
using tlcfi::fi::FiFacilitiesServer;
using tlcfi::fi::LogLevel;
namespace tlc = tlcfi::tlcfi;
namespace genfi = tlcfi::genfi;

namespace {

struct ServerFixture {
    asio::io_context io;
    std::unique_ptr<FiFacilitiesServer> server;
    std::thread ioThread;
    unsigned short port;

    ServerFixture() {
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

// Exposes the protected Session/Object update entry points so a test can
// simulate exactly what a spec-compliant server would send, regardless of what
// the stub server actually does.
class TestableControllerApp : public FiControllerApp {
public:
    using FiControllerApp::FiControllerApp;
    void injectSessionUpdate(const genfi::ObjectStateUpdateGroup& g) { processSessionUpdate(g); }
    void injectObjectUpdate(const genfi::ObjectStateUpdateGroup& g) { processObjectUpdates(g); }
    void injectEvent(const genfi::NotifyEventGroup& g) { processEventUpdates(g); }
};

// Thread-safe capture of log lines (logs may be emitted from the io thread).
struct LogCapture {
    std::mutex mtx;
    std::vector<std::string> lines;

    void attach(TestableControllerApp& app) {
        app.onLog = [this](LogLevel, const std::string& message) {
            std::lock_guard<std::mutex> lock(mtx);
            lines.push_back(message);
        };
    }

    bool sawContaining(const std::string& needle) {
        std::lock_guard<std::mutex> lock(mtx);
        for (const auto& l : lines) {
            if (l.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mtx);
        lines.clear();
    }
};

genfi::ObjectStateUpdateGroup sessionUpdate(const std::string& sessionId, const json& state) {
    genfi::ObjectStateUpdate upd;
    upd.objects.type = static_cast<int>(tlc::TLCObjectType::Session);
    upd.objects.ids = {sessionId};
    upd.states.push_back(state);
    genfi::ObjectStateUpdateGroup g;
    g.update.push_back(std::move(upd));
    return g;
}

genfi::NotifyEventGroup sessionEventGroup(const std::vector<json>& events) {
    genfi::NotifyEventGroup g;
    g.objects.type = static_cast<int>(tlc::TLCObjectType::Session);
    g.objects.ids = {"sess"};
    for (const auto& e : events) g.events.push_back(e);
    return g;
}

genfi::ObjectStateUpdateGroup intersectionUpdate(const std::string& intersectionId, int state) {
    genfi::ObjectStateUpdate upd;
    upd.objects.type = static_cast<int>(tlc::TLCObjectType::Intersection);
    upd.objects.ids = {intersectionId};
    upd.states.push_back(json{{"state", state}});
    genfi::ObjectStateUpdateGroup g;
    g.update.push_back(std::move(upd));
    return g;
}

// Connects, registers, and drives the app through the stub server's happy path
// until onControlInControl fires. Returns the session id (always set on success).
std::string driveToInControl(ServerFixture& fixture, TestableControllerApp& app,
                             std::atomic<bool>& inControl) {
    EXPECT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    EXPECT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));
    EXPECT_TRUE(fixture.waitFor([&inControl]() { return inControl.load(); },
                                std::chrono::milliseconds(5000)))
        << "controller did not reach InControl via the stub server";
    auto sessionId = app.getSessionId();
    EXPECT_TRUE(sessionId.has_value());
    return sessionId.value_or("");
}

} // namespace

// ============================================================================
// Table 6 (EndControl): unwind + re-entry
// ============================================================================

// After EndControl, a subsequent server-confirmed Offline must re-arm the
// ReadyToControl request so the controller can take control again without a
// full reconnect (FiControllerApp.cpp:276-283). This exercises the
// EndControl -> ... -> ReadyToControl re-entry path that the stub server's
// state machine cannot drive on its own.
TEST(FiControlStateLogicTests, EndControlReArmsReadyToControlRequest) {
    ServerFixture fixture;

    std::atomic<bool> inControl{false};
    std::atomic<bool> endControl{false};
    TestableControllerApp app("127.0.0.1", std::to_string(fixture.port),
                              "ctrl-rearm", "pw", "uri");
    app.onControlInControl = [&](const std::string&) { inControl = true; };
    app.onControlEndControl = [&](const std::string&) { endControl = true; };

    LogCapture logs;
    logs.attach(app);

    std::string sessionId = driveToInControl(fixture, app, inControl);
    ASSERT_FALSE(sessionId.empty());

    // Server takes back control.
    app.injectSessionUpdate(sessionUpdate(
        sessionId, json{{"controlState", static_cast<int>(tlc::ControlState::EndControl)},
                        {"reqIntersection", "INT-1"}}));
    ASSERT_TRUE(fixture.waitFor([&endControl]() { return endControl.load(); }))
        << "onControlEndControl must fire on EndControl";

    // Now the controller should be re-armed: a fresh Offline confirmation must
    // make it request ReadyToControl again.
    logs.clear();
    app.injectSessionUpdate(sessionUpdate(
        sessionId, json{{"controlState", static_cast<int>(tlc::ControlState::Offline)},
                        {"reqIntersection", "INT-1"}}));

    EXPECT_TRUE(fixture.waitFor(
        [&logs]() { return logs.sawContaining("sent Session.reqControlState=ReadyToControl"); }))
        << "controller must re-request ReadyToControl after EndControl (re-arm)";

    app.stop();
}

// EndControl with no reqIntersection in the payload must still fire the callback
// using the controller's already-known intersection id
// (FiControllerApp.cpp:268-275 fallback).
TEST(FiControlStateLogicTests, EndControlWithoutReqIntersectionUsesKnownIntersection) {
    ServerFixture fixture;

    std::atomic<bool> inControl{false};
    std::atomic<bool> endControl{false};
    std::string endIntersection;
    TestableControllerApp app("127.0.0.1", std::to_string(fixture.port),
                              "ctrl-fallback", "pw", "uri");
    app.onControlInControl = [&](const std::string&) { inControl = true; };
    app.onControlEndControl = [&](const std::string& id) {
        endIntersection = id;
        endControl = true;
    };

    std::string sessionId = driveToInControl(fixture, app, inControl);
    ASSERT_FALSE(sessionId.empty());

    // EndControl carrying ONLY controlState (no reqIntersection).
    app.injectSessionUpdate(sessionUpdate(
        sessionId, json{{"controlState", static_cast<int>(tlc::ControlState::EndControl)}}));

    ASSERT_TRUE(fixture.waitFor([&endControl]() { return endControl.load(); }))
        << "onControlEndControl must fire even without reqIntersection in the payload";
    EXPECT_FALSE(endIntersection.empty())
        << "callback must fall back to the known intersection id";

    app.stop();
}

// ============================================================================
// Tables 2/3 (Offline / ReadyToControl): the state the stub server collapses
// ============================================================================

// The stub server jumps Offline -> StartControl and never exposes a
// ReadyToControl controlState. A spec-compliant server would. Verify the client
// tolerates observing ReadyToControl as a no-op: it must NOT spuriously fire the
// InControl/EndControl callbacks and must remain functional.
TEST(FiControlStateLogicTests, ServerIssuedReadyToControlIsToleratedNoOp) {
    ServerFixture fixture;

    std::atomic<int> inControlCount{0};
    std::atomic<bool> inControl{false};
    std::atomic<bool> endControl{false};
    TestableControllerApp app("127.0.0.1", std::to_string(fixture.port),
                              "ctrl-rtc", "pw", "uri");
    app.onControlInControl = [&](const std::string&) {
        inControlCount.fetch_add(1);
        inControl = true;
    };
    app.onControlEndControl = [&](const std::string&) { endControl = true; };

    std::string sessionId = driveToInControl(fixture, app, inControl);
    ASSERT_FALSE(sessionId.empty());
    int baseline = inControlCount.load();

    // A spec-compliant server exposes ReadyToControl explicitly. Our client
    // never sees it from the stub; make sure it does no harm.
    app.injectSessionUpdate(sessionUpdate(
        sessionId, json{{"controlState", static_cast<int>(tlc::ControlState::ReadyToControl)},
                        {"reqIntersection", "INT-1"}}));

    // Give any (incorrect) async reaction a chance to occur.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(inControlCount.load(), baseline)
        << "observing ReadyToControl must not (re)trigger InControl";
    EXPECT_FALSE(endControl.load())
        << "observing ReadyToControl must not trigger EndControl";

    app.stop();
}

// ============================================================================
// Table 4 (StartControl): Intersection.state = Control gating
// ============================================================================

// Per Table 4, when StartControl is acknowledged with reqControlState=InControl
// the controller may only execute signal-group control once Intersection.state
// becomes Control. FiControllerApp implements this by deferring the
// onControlInControl callback until the intersection reaches Control
// (FiControllerApp.cpp:248-260, completeInControlTransition).
//
// The stub server always presents the intersection already in Control, so this
// branch is never exercised by the live happy path. Here we drive a second
// control cycle by hand with the intersection NOT yet in Control.
TEST(FiControlStateLogicTests, StartControlDefersInControlUntilIntersectionReachesControl) {
    ServerFixture fixture;

    std::atomic<int> inControlCount{0};
    std::atomic<bool> inControl{false};
    std::atomic<bool> endControl{false};
    std::string lastInControlIntersection;
    TestableControllerApp app("127.0.0.1", std::to_string(fixture.port),
                              "ctrl-gate", "pw", "uri");
    app.onControlInControl = [&](const std::string& id) {
        lastInControlIntersection = id;
        inControlCount.fetch_add(1);
        inControl = true;
    };
    app.onControlEndControl = [&](const std::string&) { endControl = true; };

    std::string sessionId = driveToInControl(fixture, app, inControl);
    ASSERT_FALSE(sessionId.empty());
    int afterFirstCycle = inControlCount.load();

    // End the first control cycle: this also resets the controller's control-flow
    // guards so we can drive a fresh StartControl.
    app.injectSessionUpdate(sessionUpdate(
        sessionId, json{{"controlState", static_cast<int>(tlc::ControlState::EndControl)},
                        {"reqIntersection", "INT-1"}}));
    ASSERT_TRUE(fixture.waitFor([&endControl]() { return endControl.load(); }));

    // Move the intersection OUT of Control (e.g. AllRed) before the next StartControl.
    app.injectObjectUpdate(
        intersectionUpdate("INT-1", static_cast<int>(tlc::IntersectionControlState::AllRed)));

    // Server re-grants control. Intersection is not in Control yet, so the
    // controller must defer onControlInControl.
    app.injectSessionUpdate(sessionUpdate(
        sessionId, json{{"controlState", static_cast<int>(tlc::ControlState::StartControl)},
                        {"reqIntersection", "INT-1"}}));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(inControlCount.load(), afterFirstCycle)
        << "onControlInControl must be deferred while Intersection.state != Control";

    // Intersection now reaches Control -> the deferred transition completes.
    app.injectObjectUpdate(
        intersectionUpdate("INT-1", static_cast<int>(tlc::IntersectionControlState::Control)));

    EXPECT_TRUE(fixture.waitFor(
        [&]() { return inControlCount.load() == afterFirstCycle + 1; }))
        << "onControlInControl must fire once the intersection reaches Control";
    EXPECT_EQ(lastInControlIntersection, "INT-1");

    app.stop();
}

// ============================================================================
// Tables 4/5 (Error): controlState = Error during control flow
// ============================================================================

// Several decision-table columns drive the application to controlState=Error
// (invalid requested control state, timeouts). The controller must handle an
// Error notification during control flow without crashing and remain usable.
TEST(FiControlStateLogicTests, ErrorStateDuringControlIsHandledGracefully) {
    ServerFixture fixture;

    std::atomic<bool> inControl{false};
    TestableControllerApp app("127.0.0.1", std::to_string(fixture.port),
                              "ctrl-error", "pw", "uri");
    app.onControlInControl = [&](const std::string&) { inControl = true; };

    std::string sessionId = driveToInControl(fixture, app, inControl);
    ASSERT_FALSE(sessionId.empty());

    // Server reports the session has gone to Error.
    ASSERT_NO_THROW(app.injectSessionUpdate(sessionUpdate(
        sessionId, json{{"controlState", static_cast<int>(tlc::ControlState::Error)},
                        {"reqIntersection", "INT-1"}})));

    // The app must still be responsive (no deadlock / corrupted state): issuing a
    // control request must not throw.
    auto sgIds = app.getSignalGroupIds();
    if (!sgIds.empty()) {
        std::vector<std::pair<std::string, tlc::SignalGroupState>> updates{
            {sgIds[0], tlc::SignalGroupState::StopAndRemain}};
        EXPECT_NO_THROW(app.requestSignalGroupStates(updates));
    }

    app.stop();
}

// ============================================================================
// SessionEvent handling (§7.7/§7.9 exceptions, GEN-FI §6.5) — delivered via
// NotifyEvent with objects.type = Session.
// ============================================================================

// A lifecycle SessionEvent (FacilitiesStopping) must fire onSessionEvent but NOT
// onSessionError — lifecycle codes (< 1000) are not errors. Pure dispatch test:
// no server/registration needed (the NotifyEvent path is not session-id gated).
TEST(FiControlStateLogicTests, SessionLifecycleEventFiresOnSessionEventOnly) {
    TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");

    std::atomic<int> eventCode{-1};
    std::atomic<bool> errorFired{false};
    app.onSessionEvent = [&](const genfi::SessionEvent& ev) { eventCode = ev.code; };
    app.onSessionError = [&](int, const std::string&) { errorFired = true; };

    app.injectEvent(sessionEventGroup(
        {json{{"code", static_cast<int>(genfi::SessionEventCode::FacilitiesStopping)}}}));

    EXPECT_EQ(eventCode.load(), static_cast<int>(genfi::SessionEventCode::FacilitiesStopping));
    EXPECT_FALSE(errorFired.load()) << "lifecycle codes must not be treated as errors";
}

// A failure SessionEvent (code >= 1000) must fire BOTH onSessionEvent (with parsed
// SessionEventInformation) and onSessionError (with a decoded reason).
TEST(FiControlStateLogicTests, SessionFailureEventFiresOnSessionErrorWithDecodedReason) {
    TestableControllerApp app("127.0.0.1", "1", "u", "p", "uri");

    std::atomic<bool> eventFired{false};
    bool infoParsed = false;
    std::atomic<int> errorCode{-1};
    std::string errorReason;
    app.onSessionEvent = [&](const genfi::SessionEvent& ev) {
        eventFired = true;
        if (ev.info.has_value()) {
            infoParsed = (ev.info->id == "SG1" && ev.info->attribute == "reqState");
        }
    };
    app.onSessionError = [&](int code, const std::string& reason) {
        errorCode = code;
        errorReason = reason;
    };

    const int code =
        static_cast<int>(tlc::SessionEventCodeExt::UpdateStateFailedIncorrectControlState);
    app.injectEvent(sessionEventGroup({json{
        {"code", code},
        {"info", json{{"type", 3}, {"id", "SG1"}, {"attribute", "reqState"}}}}}));

    EXPECT_TRUE(eventFired.load());
    EXPECT_TRUE(infoParsed) << "SessionEventInformation must be deserialized";
    EXPECT_EQ(errorCode.load(), code);
    EXPECT_NE(errorReason.find("UpdateStateFailedIncorrectControlState"), std::string::npos)
        << "reason should decode the code; got: " << errorReason;
}

// A failure SessionEvent during control must reset the control-flow guards so the
// controller re-arms after the (Facilities-initiated) reconnect. Verified
// behaviorally: after the failure event, a fresh Offline re-issues ReadyToControl.
TEST(FiControlStateLogicTests, SessionFailureEventResetsControlGuardsAllowingReArm) {
    ServerFixture fixture;

    std::atomic<bool> inControl{false};
    TestableControllerApp app("127.0.0.1", std::to_string(fixture.port),
                              "ctrl-sessfail", "pw", "uri");
    app.onControlInControl = [&](const std::string&) { inControl = true; };

    LogCapture logs;
    logs.attach(app);

    std::string sessionId = driveToInControl(fixture, app, inControl);
    ASSERT_FALSE(sessionId.empty());

    // Facilities rejects an UpdateState -> failure SessionEvent (it would then close
    // the connection). This must reset readyToControlRequested_/inControlRequested_.
    app.injectEvent(sessionEventGroup({json{
        {"code", static_cast<int>(tlc::SessionEventCodeExt::UpdateStateFailedIncorrectIntersection)}}}));

    // A fresh Offline confirmation must now re-trigger the ReadyToControl request.
    logs.clear();
    app.injectSessionUpdate(sessionUpdate(
        sessionId, json{{"controlState", static_cast<int>(tlc::ControlState::Offline)},
                        {"reqIntersection", "INT-1"}}));

    EXPECT_TRUE(fixture.waitFor(
        [&logs]() { return logs.sawContaining("sent Session.reqControlState=ReadyToControl"); }))
        << "control-flow guards must be reset by the failure event so re-arm works";

    app.stop();
}
