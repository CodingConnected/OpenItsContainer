#include <gtest/gtest.h>

#include "fi/FiAppHost.hpp"
#include "fi/FiFacilitiesServer.hpp"
#include "fi/FiConsumerApp.hpp"

#include <asio/io_context.hpp>
#include <thread>
#include <atomic>

using namespace tlcfi::fi;

namespace {

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

TEST(FiAppHostIntegration, ReconnectsAfterClientCleanup) {
    FacilitiesFixture fixture;

    FiConsumerApp app("127.0.0.1", std::to_string(fixture.port), "user", "pw", "uri");
    FiAppHost::Config cfg;
    cfg.connectTimeout = std::chrono::milliseconds(2000);
    cfg.operationTimeout = std::chrono::milliseconds(2000);
    cfg.initialRetryDelay = std::chrono::milliseconds(200);
    cfg.maxRetryDelay = std::chrono::milliseconds(1000);

    FiAppHost host(app, cfg);

    std::atomic<bool> shouldRun{true};
    std::thread hostThread([&]() { host.run(shouldRun); });

    // Wait for initial connection
    for (int i = 0; i < 50 && !app.isFullyInitialized(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ASSERT_TRUE(app.isFullyInitialized());

    // Simulate drop by cleaning up the client; host should reconnect
    app.cleanup();
    
    // Clear registration throttling to allow immediate re-registration
    // (The server normally enforces a 42-second wait between registrations)
    fixture.server->clearRegistrationThrottling("user");

    for (int i = 0; i < 50 && !app.isFullyInitialized(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    EXPECT_TRUE(app.isFullyInitialized());

    shouldRun = false;
    if (hostThread.joinable()) {
        hostThread.join();
    }
    app.stop();
}

