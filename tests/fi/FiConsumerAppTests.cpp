#include <gtest/gtest.h>

#include "fi/FiConsumerApp.hpp"
#include "fi/FiFacilitiesServer.hpp"
#include "tcp/TlsConfig.hpp"

#include <asio/io_context.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace tlcfi::fi;

namespace {

class TestConsumerApp : public FiConsumerApp {
public:
    TestConsumerApp()
        : FiConsumerApp("127.0.0.1", "0", "user", "password", "uri") {}

    using FiConsumerApp::processObjectUpdates;
    using FiConsumerApp::getStateMutex;
};

struct FacilitiesFixture {
    asio::io_context io;
    std::unique_ptr<FiFacilitiesServer> server;
    std::thread ioThread;
    unsigned short port{};

    // Binds an OS-assigned ephemeral port (port 0) to avoid fixed-port
    // collisions across tests; read the actual port from `port`.
    FacilitiesFixture() {
        server = std::make_unique<FiFacilitiesServer>(io, 0);
        port = server->port();
        server->start();
        ioThread = std::thread([this]() { io.run(); });
    }

    ~FacilitiesFixture() {
        server->stop();
        io.stop();
        if (ioThread.joinable()) {
            ioThread.join();
        }
    }
};

} // namespace

TEST(FiConsumerAppTests, MultiObjectStatesAreMatchedByArrayIndex) {
    TestConsumerApp app;

    tlcfi::genfi::ObjectStateUpdate update;
    update.objects.type = static_cast<int>(tlcfi::tlcfi::TLCObjectType::SignalGroup);
    update.objects.ids = {"SG1", "SG2"};
    update.states = {
        nlohmann::json{{"state", static_cast<int>(tlcfi::tlcfi::SignalGroupState::Dark)}},
        nlohmann::json{{"state", static_cast<int>(tlcfi::tlcfi::SignalGroupState::PermissiveMovementAllowed)}}
    };

    tlcfi::genfi::ObjectStateUpdateGroup group;
    group.update.push_back(std::move(update));
    app.processObjectUpdates(group);

    EXPECT_EQ(app.getSignalGroupState("SG1"), tlcfi::tlcfi::SignalGroupState::Dark);
    EXPECT_EQ(app.getSignalGroupState("SG2"),
              tlcfi::tlcfi::SignalGroupState::PermissiveMovementAllowed);
}

TEST(FiConsumerAppTests, ObjectUpdateCallbacksDoNotHoldStateMutex) {
    TestConsumerApp app;
    std::atomic<bool> signalGroupCallbackRan{false};
    std::atomic<bool> outputCallbackRan{false};
    std::atomic<bool> detectorCallbackRan{false};
    std::atomic<bool> inputCallbackRan{false};

    app.onSignalGroupUpdate = [&](const tlcfi::tlcfi::SignalGroup& sg) {
        std::lock_guard<std::mutex> lock(app.getStateMutex());
        EXPECT_EQ(sg.state, tlcfi::tlcfi::SignalGroupState::Dark);
        signalGroupCallbackRan = true;
    };
    app.onOutputUpdate = [&](const tlcfi::tlcfi::Output& out) {
        EXPECT_EQ(app.getOutput(out.id).state, out.state);
        outputCallbackRan = true;
    };
    app.onDetectorUpdate = [&](const tlcfi::tlcfi::Detector& det) {
        EXPECT_EQ(app.getDetector(det.id).state, det.state);
        detectorCallbackRan = true;
    };
    app.onInputUpdate = [&](const tlcfi::tlcfi::Input& inp) {
        EXPECT_EQ(app.getInput(inp.id).state, inp.state);
        inputCallbackRan = true;
    };

    tlcfi::genfi::ObjectStateUpdate signalGroupUpdate;
    signalGroupUpdate.objects.type = static_cast<int>(tlcfi::tlcfi::TLCObjectType::SignalGroup);
    signalGroupUpdate.objects.ids = {"SG1"};
    signalGroupUpdate.states = {
        nlohmann::json{{"state", static_cast<int>(tlcfi::tlcfi::SignalGroupState::Dark)}}};

    tlcfi::genfi::ObjectStateUpdate outputUpdate;
    outputUpdate.objects.type = static_cast<int>(tlcfi::tlcfi::TLCObjectType::Output);
    outputUpdate.objects.ids = {"OUT1"};
    outputUpdate.states = {nlohmann::json{{"state", 1}}};

    tlcfi::genfi::ObjectStateUpdate detectorUpdate;
    detectorUpdate.objects.type = static_cast<int>(tlcfi::tlcfi::TLCObjectType::Detector);
    detectorUpdate.objects.ids = {"DET1"};
    detectorUpdate.states = {
        nlohmann::json{{"state", static_cast<int>(tlcfi::tlcfi::DetectorState::Occupied)}}};

    tlcfi::genfi::ObjectStateUpdate inputUpdate;
    inputUpdate.objects.type = static_cast<int>(tlcfi::tlcfi::TLCObjectType::Input);
    inputUpdate.objects.ids = {"IN1"};
    inputUpdate.states = {nlohmann::json{{"state", 1}}};

    tlcfi::genfi::ObjectStateUpdateGroup group;
    group.update = {signalGroupUpdate, outputUpdate, detectorUpdate, inputUpdate};
    app.processObjectUpdates(group);

    EXPECT_TRUE(signalGroupCallbackRan.load());
    EXPECT_TRUE(outputCallbackRan.load());
    EXPECT_TRUE(detectorCallbackRan.load());
    EXPECT_TRUE(inputCallbackRan.load());
}

TEST(FiConsumerAppIntegration, RegisterReadMetaAndSubscribeAll) {
    FacilitiesFixture fixture;

    FiConsumerApp app("127.0.0.1", std::to_string(fixture.port), "user", "pw", "uri");

    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.readMeta(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.subscribeAll(std::chrono::milliseconds(2000)));

    EXPECT_TRUE(app.isFullyInitialized());
    auto sgIds = app.getSignalGroupIds();
    EXPECT_FALSE(sgIds.empty());
    auto outIds = app.getOutputIds();
    EXPECT_FALSE(outIds.empty());

    app.stop();
}

TEST(FiConsumerAppIntegration, AliveUsesFixedSendCadence) {
    FacilitiesFixture fixture;
    std::mutex mutex;
    std::condition_variable received;
    std::vector<std::chrono::steady_clock::time_point> arrivalTimes;
    fixture.server->onAliveReceived = [&]() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            arrivalTimes.push_back(std::chrono::steady_clock::now());
        }
        received.notify_all();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    };

    FiConsumerApp app("127.0.0.1", std::to_string(fixture.port), "user", "pw", "uri");
    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));

    {
        std::unique_lock<std::mutex> lock(mutex);
        ASSERT_TRUE(received.wait_for(lock, std::chrono::seconds(7), [&]() {
            return arrivalTimes.size() >= 3;
        }));
    }
    app.stop();

    ASSERT_GE(arrivalTimes.size(), 3u);
    for (std::size_t i = 1; i < 3; ++i) {
        const auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(
            arrivalTimes[i] - arrivalTimes[i - 1]);
        EXPECT_LT(interval, std::chrono::milliseconds(2500));
        EXPECT_GT(interval, std::chrono::milliseconds(1500));
    }
}

TEST(FiConsumerAppIntegration, AliveFailuresDisconnectForRecovery) {
    FacilitiesFixture fixture;
    fixture.server->onAliveReceived = []() {
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    };

    FiConsumerApp app("127.0.0.1", std::to_string(fixture.port), "user", "pw", "uri");
    ASSERT_TRUE(app.connect(std::chrono::milliseconds(2000)));
    ASSERT_TRUE(app.registerApp(std::chrono::milliseconds(2000)));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (app.isConnected() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    EXPECT_FALSE(app.isConnected());
    app.stop();
}

