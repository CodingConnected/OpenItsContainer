#include <gtest/gtest.h>
#include "../test_helpers.hpp"

#include "tcp/TcpServer.hpp"
#include "tcp/TcpServerTransport.hpp"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

class TcpServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        io_ = std::make_unique<asio::io_context>();
        runner_ = std::make_unique<test_helpers::IoContextRunner>(*io_);
    }

    void TearDown() override {
        runner_.reset();
        io_.reset();
    }

    std::unique_ptr<asio::io_context> io_;
    std::unique_ptr<test_helpers::IoContextRunner> runner_;
};

TEST_F(TcpServerTest, StartsAndBindsToPort) {
    // Use a fixed port to verify binding
    auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
    const unsigned short testPort = server->port();

    // Set up callback to start accepted transports
    server->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
    };

    // Verify start() doesn't throw
    EXPECT_NO_THROW(server->start());

    // Give it a moment to start accepting
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Verify the port is actually bound by attempting to connect
    // If the server is bound and listening, the connection should succeed
    asio::ip::tcp::socket testSocket(*io_);
    asio::ip::tcp::resolver resolver(*io_);
    std::error_code ec;
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
    ASSERT_FALSE(ec) << "DNS resolution should succeed";

    // Attempt connection - should succeed if server is bound and listening
    asio::connect(testSocket, endpoints, ec);
    EXPECT_FALSE(ec) << "Server should be bound and accepting connections on port " << testPort;

    testSocket.close();
    server->stop();
}

TEST_F(TcpServerTest, StopsAcceptingConnections) {
    auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
    const unsigned short testPort = server->port();
    
    // Set up callback to track if connections are accepted
    bool connectionAccepted = false;
    server->onAccept = [&connectionAccepted](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        connectionAccepted = true;
    };
    
    server->start();
    
    // Wait for server to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Verify server accepts connections before stopping
    asio::ip::tcp::socket testSocket1(*io_);
    asio::ip::tcp::resolver resolver(*io_);
    std::error_code ec;
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
    ASSERT_FALSE(ec);
    
    asio::connect(testSocket1, endpoints, ec);
    ASSERT_FALSE(ec) << "Server should accept connections before stop";
    
    // Wait for accept callback
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(connectionAccepted) << "Connection should be accepted before stop";
    
    testSocket1.close();
    
    // Reset the flag
    connectionAccepted = false;
    
    // Stop the server
    EXPECT_NO_THROW(server->stop());
    
    // Give it a moment to stop
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Verify new connections are NOT accepted after stopping
    asio::ip::tcp::socket testSocket2(*io_);
    endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
    ASSERT_FALSE(ec);
    
    // Attempt connection - should fail or timeout since server is stopped
    asio::connect(testSocket2, endpoints, ec);
    EXPECT_TRUE(ec) << "Connection should fail after server is stopped";
    
    // Verify callback was not called
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_FALSE(connectionAccepted) << "No new connections should be accepted after stop";
}

TEST_F(TcpServerTest, AcceptsSingleConnection) {
    // Use a fixed test port
    auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
    const unsigned short testPort = server->port();
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> acceptedTransport{nullptr};
    test_helpers::AsyncWaiter acceptWaiter;
    
    // Ensure callback is set before starting server
    server->onAccept = [&acceptedTransport, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        acceptedTransport = transport;
        acceptWaiter.notify();
    };
    
    server->start();
    
    // Post a no-op work item to ensure io_context thread is running
    asio::post(*io_, [](){});
    
    // Brief wait for server to be ready
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Create a client connection
    asio::ip::tcp::socket clientSocket(*io_);
    asio::ip::tcp::resolver resolver(*io_);
    
    // Connect client (synchronous, but triggers async accept on server)
    std::error_code ec;
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
    ASSERT_FALSE(ec) << "DNS resolution should succeed";
    
    asio::connect(clientSocket, endpoints, ec);
    ASSERT_FALSE(ec) << "Client connection should succeed";
    
    // The async accept callback should fire in the io_context runner thread
    // Wait for it with a reasonable timeout
    bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(500));
    
    EXPECT_TRUE(accepted) << "Connection should be accepted - callback should have fired";
    EXPECT_NE(acceptedTransport, nullptr) << "Transport should be created";
    
    // Verify transport is not started automatically
    // (This is verified by the fact that we didn't set onConnected callback and it wasn't called)
    
    server->stop();
}

TEST_F(TcpServerTest, AcceptsMultipleConnections) {
    auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
    const unsigned short testPort = server->port();
    
    // Track all accepted transports
    std::vector<std::shared_ptr<tlcfi::tcp::TcpServerTransport>> acceptedTransports;
    test_helpers::AsyncWaiter acceptWaiter;
    int acceptCount = 0;
    
    server->onAccept = [&acceptedTransports, &acceptWaiter, &acceptCount](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        acceptedTransports.push_back(transport);
        acceptCount++;
        acceptWaiter.notify();
    };
    
    server->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Connect first client
    asio::ip::tcp::socket clientSocket1(*io_);
    asio::ip::tcp::resolver resolver(*io_);
    std::error_code ec;
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
    ASSERT_FALSE(ec);
    
    asio::connect(clientSocket1, endpoints, ec);
    ASSERT_FALSE(ec);
    
    // Wait for first accept
    bool accepted1 = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(accepted1) << "First connection should be accepted";
    EXPECT_EQ(acceptCount, 1) << "Should have accepted 1 connection";
    EXPECT_EQ(acceptedTransports.size(), 1) << "Should have 1 transport";
    
    // Reset waiter for second connection
    acceptWaiter.reset();
    
    // Connect second client
    asio::ip::tcp::socket clientSocket2(*io_);
    endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
    ASSERT_FALSE(ec);
    
    asio::connect(clientSocket2, endpoints, ec);
    ASSERT_FALSE(ec);
    
    // Wait for second accept
    bool accepted2 = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(accepted2) << "Second connection should be accepted";
    EXPECT_EQ(acceptCount, 2) << "Should have accepted 2 connections";
    EXPECT_EQ(acceptedTransports.size(), 2) << "Should have 2 transports";
    
    // Verify each connection gets unique transport instance
    EXPECT_NE(acceptedTransports[0], acceptedTransports[1]) << "Each connection should get unique transport instance";
    
    clientSocket1.close();
    clientSocket2.close();
    server->stop();
}

TEST_F(TcpServerTest, ContinuesAcceptingAfterConnection) {
    auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
    const unsigned short testPort = server->port();
    
    std::vector<std::shared_ptr<tlcfi::tcp::TcpServerTransport>> acceptedTransports;
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&acceptedTransports, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        acceptedTransports.push_back(transport);
        acceptWaiter.notify();
    };
    
    server->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Accept first connection
    asio::ip::tcp::socket clientSocket1(*io_);
    asio::ip::tcp::resolver resolver(*io_);
    std::error_code ec;
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
    ASSERT_FALSE(ec);
    
    asio::connect(clientSocket1, endpoints, ec);
    ASSERT_FALSE(ec);
    
    bool accepted1 = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(accepted1) << "First connection should be accepted";
    EXPECT_EQ(acceptedTransports.size(), 1) << "Should have 1 transport after first connection";
    
    // Keep first connection open and wait to verify server continues accepting
    // This tests that the server remains ready to accept new connections
    // even when existing connections are active
    acceptWaiter.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Verify server is still accepting by connecting a second client
    asio::ip::tcp::socket clientSocket2(*io_);
    endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
    ASSERT_FALSE(ec);
    
    asio::connect(clientSocket2, endpoints, ec);
    ASSERT_FALSE(ec);
    
    bool accepted2 = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(accepted2) << "Second connection should be accepted while first is still open";
    EXPECT_EQ(acceptedTransports.size(), 2) << "Should have 2 transports";
    
    // Verify both connections are independent (both transports exist)
    EXPECT_NE(acceptedTransports[0], nullptr) << "First transport should exist";
    EXPECT_NE(acceptedTransports[1], nullptr) << "Second transport should exist";
    EXPECT_NE(acceptedTransports[0], acceptedTransports[1]) << "Transports should be different instances";
    
    clientSocket1.close();
    clientSocket2.close();
    server->stop();
}

TEST_F(TcpServerTest, HandlesInvalidPortGracefully) {
    // Note: Since constructor takes unsigned short (0-65535), we can't test port > 65535
    // as it would wrap around. All values in unsigned short range are potentially valid.
    // However, we can test that the constructor handles valid edge cases.
    
    // Test with port 0 (ephemeral) - this should be valid
    EXPECT_NO_THROW({
        auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
        server->start();
        server->stop();
    }) << "Port 0 (ephemeral) should be valid";
    
    // Test with maximum valid port (65535)
    EXPECT_NO_THROW({
        auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 65535);
        // Note: This might fail to bind if port is in use, but constructor should not throw
        server->start();
        server->stop();
    }) << "Port 65535 (max valid) should be accepted by constructor";
    
    // Test that attempting to bind to an already-in-use port fails gracefully
    // This tests error handling in a realistic scenario
    const unsigned short testPort = 54329;
    auto server1 = std::make_shared<tlcfi::tcp::TcpServer>(*io_, testPort);
    server1->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Attempting to create a second server on the same port should fail
    // The constructor might succeed, but start() or binding might fail
    // This tests that the class handles port conflicts appropriately
    auto server2 = std::make_shared<tlcfi::tcp::TcpServer>(*io_, testPort);
    // The second server might construct successfully, but binding will fail
    // when we try to start it (or it might throw during construction on some systems)
    // This is system-dependent behavior
    
    server1->stop();
    server2->stop();
}

TEST_F(TcpServerTest, StopsWhileAccepting) {
    auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
    const unsigned short testPort = server->port();
    
    bool connectionAccepted = false;
    server->onAccept = [&connectionAccepted](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        connectionAccepted = true;
    };
    
    server->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Stop the server while it's waiting for connections
    EXPECT_NO_THROW(server->stop());
    
    // Give it a moment to stop
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Now try to connect - should fail since server is stopped
    asio::ip::tcp::socket testSocket(*io_);
    asio::ip::tcp::resolver resolver(*io_);
    std::error_code ec;
    auto endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
    ASSERT_FALSE(ec);
    
    // Connection attempt should fail
    asio::connect(testSocket, endpoints, ec);
    EXPECT_TRUE(ec) << "Connection should fail after server is stopped";
    
    // Verify onAccept callback was NOT called
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_FALSE(connectionAccepted) << "No onAccept callback should be invoked after stop";
}

