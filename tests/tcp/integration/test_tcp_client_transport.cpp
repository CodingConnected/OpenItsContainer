#include <gtest/gtest.h>
#include "test_helpers.hpp"

#include "tcp/TcpClientTransport.hpp"
#include "tcp/TcpServer.hpp"
#include "tcp/TcpServerTransport.hpp"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class TcpClientTransportTest : public ::testing::Test {
protected:
    void SetUp() override {
        io_ = std::make_unique<asio::io_context>();
        runner_ = std::make_unique<test_helpers::IoContextRunner>(*io_);
    }

    void TearDown() override {
        runner_.reset();
        io_.reset();
    }

    // Creates a server on an OS-assigned ephemeral port (port 0); the actual
    // bound port is available via server->port(). Using port 0 avoids
    // fixed-port collisions / TIME_WAIT flakiness across tests.
    std::shared_ptr<tlcfi::tcp::TcpServer> createServer() {
        auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
        server->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return server;
    }

    // Helper to create and connect a client transport to a server
    std::shared_ptr<tlcfi::tcp::TcpClientTransport> createClientTransport(const std::string& host, unsigned short port) {
        auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, host, std::to_string(port));
        return client;
    }

    std::unique_ptr<asio::io_context> io_;
    std::unique_ptr<test_helpers::IoContextRunner> runner_;
};

// 3.1 Basic Lifecycle

TEST_F(TcpClientTransportTest, ClientConnectsToServer) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&serverTransport, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        acceptWaiter.notify();
    };
    
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    
    auto client = createClientTransport("127.0.0.1", testPort);
    client->onConnected = [&connected, &connectWaiter]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    // Wait for connection
    bool connectedReceived = connectWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(connectedReceived) << "Client should connect to server";
    EXPECT_TRUE(connected) << "onConnected callback should be invoked";
    
    // Wait for server to accept
    bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(500));
    EXPECT_TRUE(accepted) << "Server should accept connection";
    
    // Verify transport name
    EXPECT_EQ(client->name(), "tcp-client") << "Transport name should be 'tcp-client'";
    
    client->stop();
    server->stop();
}

TEST_F(TcpClientTransportTest, ClientStopsAndInvokesOnDisconnected) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    auto client = createClientTransport("127.0.0.1", testPort);
    
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    client->onConnected = [&connected, &connectWaiter]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    bool connectedReceived = connectWaiter.wait_for(std::chrono::milliseconds(2000));
    ASSERT_TRUE(connectedReceived) << "Client should connect first";
    
    bool disconnected = false;
    std::string disconnectReason;
    test_helpers::AsyncWaiter disconnectWaiter;
    
    client->onDisconnected = [&disconnected, &disconnectReason, &disconnectWaiter](const std::string& reason) {
        disconnected = true;
        disconnectReason = reason;
        disconnectWaiter.notify();
    };
    
    // Stop the client
    client->stop();
    
    // Wait for onDisconnected callback
    bool disconnectedReceived = disconnectWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(disconnectedReceived) << "onDisconnected callback should be invoked";
    EXPECT_TRUE(disconnected) << "Disconnected flag should be set";
    EXPECT_EQ(disconnectReason, "stop") << "Disconnect reason should be 'stop'";
    
    server->stop();
}

// 3.2 Connection Establishment

TEST_F(TcpClientTransportTest, ClientConnectsToLocalhost) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    
    auto client = createClientTransport("127.0.0.1", testPort);
    client->onConnected = [&connected, &connectWaiter]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    bool connectedReceived = connectWaiter.wait_for(std::chrono::milliseconds(2000));
    EXPECT_TRUE(connectedReceived) << "Client should connect to localhost";
    EXPECT_TRUE(connected) << "Connection should succeed";
    
    client->stop();
    server->stop();
}

TEST_F(TcpClientTransportTest, ClientHandlesConnectionFailure) {
    // Use a port that's unlikely to be in use (high port number)
    const unsigned short unusedPort = 65432;
    
    bool errorOccurred = false;
    std::string errorMessage;
    test_helpers::AsyncWaiter errorWaiter;
    
    auto client = createClientTransport("127.0.0.1", unusedPort);
    client->onError = [&errorOccurred, &errorMessage, &errorWaiter](const std::string& error) {
        errorOccurred = true;
        errorMessage = error;
        errorWaiter.notify();
    };
    
    client->start();
    
    // Wait for error callback (connection should fail)
    bool errorReceived = errorWaiter.wait_for(std::chrono::milliseconds(3000));
    EXPECT_TRUE(errorReceived) << "onError callback should be invoked on connection failure";
    EXPECT_TRUE(errorOccurred) << "Error should be reported";
    EXPECT_NE(errorMessage.find("connect:"), std::string::npos) << "Error message should indicate connection failure";
    
    client->stop();
}

TEST_F(TcpClientTransportTest, ClientHandlesDnsResolutionFailure) {
    bool errorOccurred = false;
    std::string errorMessage;
    test_helpers::AsyncWaiter errorWaiter;
    
    auto client = createClientTransport("invalid-hostname-that-does-not-exist.local", 80);
    client->onError = [&errorOccurred, &errorMessage, &errorWaiter](const std::string& error) {
        errorOccurred = true;
        errorMessage = error;
        errorWaiter.notify();
    };
    
    client->start();
    
    // Wait for error callback (DNS resolution should fail)
    bool errorReceived = errorWaiter.wait_for(std::chrono::milliseconds(5000));
    EXPECT_TRUE(errorReceived) << "onError callback should be invoked on DNS resolution failure";
    EXPECT_TRUE(errorOccurred) << "Error should be reported";
    EXPECT_NE(errorMessage.find("resolve:"), std::string::npos) << "Error message should indicate resolution failure";
    
    client->stop();
}

// 3.3 Data Reception

TEST_F(TcpClientTransportTest, ClientReceivesSingleMessage) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&serverTransport, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        acceptWaiter.notify();
    };
    
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    
    auto client = createClientTransport("127.0.0.1", testPort);
    client->onConnected = [&connected, &connectWaiter]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    bool connectedReceived = connectWaiter.wait_for(std::chrono::milliseconds(2000));
    ASSERT_TRUE(connectedReceived) << "Client should connect";
    
    bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    ASSERT_TRUE(accepted) << "Server should accept connection";
    ASSERT_NE(serverTransport, nullptr) << "Server transport should be created";
    
    serverTransport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::vector<std::uint8_t> receivedData;
    test_helpers::AsyncWaiter bytesWaiter;
    
    client->onBytes = [&receivedData, &bytesWaiter](const std::uint8_t* data, std::size_t size) {
        receivedData.insert(receivedData.end(), data, data + size);
        bytesWaiter.notify();
    };
    
    // Send data from server
    const std::string testMessage = "Hello, Client!";
    serverTransport->asyncSend(reinterpret_cast<const std::uint8_t*>(testMessage.data()), testMessage.size());
    
    // Wait for data to be received
    bool received = bytesWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(received) << "onBytes callback should be invoked";
    
    // Verify received data matches sent data
    EXPECT_EQ(receivedData.size(), testMessage.size()) << "Received data size should match sent data";
    EXPECT_TRUE(std::equal(receivedData.begin(), receivedData.end(),
                          testMessage.begin(), testMessage.end()))
        << "Received data should match sent data exactly";
    
    client->stop();
    serverTransport->stop();
    server->stop();
}

TEST_F(TcpClientTransportTest, ClientReceivesMultipleMessages) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&serverTransport, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        acceptWaiter.notify();
    };
    
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    
    auto client = createClientTransport("127.0.0.1", testPort);
    client->onConnected = [&connected, &connectWaiter]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    bool connectedReceived = connectWaiter.wait_for(std::chrono::milliseconds(2000));
    ASSERT_TRUE(connectedReceived);
    
    bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    ASSERT_TRUE(accepted);
    ASSERT_NE(serverTransport, nullptr);
    
    serverTransport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::vector<std::uint8_t> allReceivedData;
    int messageCount = 0;
    test_helpers::AsyncWaiter bytesWaiter;
    
    client->onBytes = [&allReceivedData, &messageCount, &bytesWaiter](const std::uint8_t* data, std::size_t size) {
        allReceivedData.insert(allReceivedData.end(), data, data + size);
        messageCount++;
        bytesWaiter.notify();
    };
    
    // Send first message
    const std::string message1 = "First message";
    serverTransport->asyncSend(reinterpret_cast<const std::uint8_t*>(message1.data()), message1.size());
    
    bool received1 = bytesWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(received1) << "First message should be received";
    EXPECT_EQ(messageCount, 1) << "Should have received 1 message";
    
    bytesWaiter.reset();
    
    // Send second message
    const std::string message2 = "Second message";
    serverTransport->asyncSend(reinterpret_cast<const std::uint8_t*>(message2.data()), message2.size());
    
    bool received2 = bytesWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(received2) << "Second message should be received";
    EXPECT_EQ(messageCount, 2) << "Should have received 2 messages";
    
    // Verify all data is received correctly
    std::string expectedData = message1 + message2;
    EXPECT_EQ(allReceivedData.size(), expectedData.size()) << "Total received data size should match";
    EXPECT_TRUE(std::equal(allReceivedData.begin(), allReceivedData.end(),
                          expectedData.begin(), expectedData.end()))
        << "All received data should match sent data";
    
    client->stop();
    serverTransport->stop();
    server->stop();
}

TEST_F(TcpClientTransportTest, ClientReceivesLargeMessage) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&serverTransport, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        acceptWaiter.notify();
    };
    
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    
    auto client = createClientTransport("127.0.0.1", testPort);
    client->onConnected = [&connected, &connectWaiter]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    bool connectedReceived = connectWaiter.wait_for(std::chrono::milliseconds(2000));
    ASSERT_TRUE(connectedReceived);
    
    bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    ASSERT_TRUE(accepted);
    ASSERT_NE(serverTransport, nullptr);
    
    serverTransport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::vector<std::uint8_t> allReceivedData;
    int chunkCount = 0;
    test_helpers::AsyncWaiter bytesWaiter;
    
    client->onBytes = [&allReceivedData, &chunkCount, &bytesWaiter](const std::uint8_t* data, std::size_t size) {
        allReceivedData.insert(allReceivedData.end(), data, data + size);
        chunkCount++;
        bytesWaiter.notify();
    };
    
    // Send message larger than read buffer (8KB)
    const size_t largeMessageSize = 10 * 1024;
    std::vector<std::uint8_t> largeMessage(largeMessageSize);
    for (size_t i = 0; i < largeMessageSize; ++i) {
        largeMessage[i] = static_cast<std::uint8_t>(i % 256);
    }
    
    serverTransport->asyncSend(largeMessage.data(), largeMessageSize);
    
    // Wait for all chunks to be received
    int receivedChunks = 0;
    for (int i = 0; i < 10 && allReceivedData.size() < largeMessageSize; ++i) {
        if (bytesWaiter.wait_for(std::chrono::milliseconds(500))) {
            receivedChunks++;
            bytesWaiter.reset();
        } else {
            break;
        }
    }
    
    // Give a bit more time for any remaining chunks
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    EXPECT_GE(chunkCount, 2) << "Large message should be received in multiple chunks (buffer is 8KB)";
    EXPECT_EQ(allReceivedData.size(), largeMessageSize) << "Complete message should be received";
    
    // Verify data integrity
    EXPECT_TRUE(std::equal(allReceivedData.begin(), allReceivedData.end(),
                          largeMessage.begin(), largeMessage.end()))
        << "Received large message should match sent data exactly";
    
    client->stop();
    serverTransport->stop();
    server->stop();
}

// 3.5 Data Transmission

TEST_F(TcpClientTransportTest, ClientSendsSingleMessage) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&serverTransport, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        acceptWaiter.notify();
    };
    
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    
    auto client = createClientTransport("127.0.0.1", testPort);
    client->onConnected = [&connected, &connectWaiter]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    bool connectedReceived = connectWaiter.wait_for(std::chrono::milliseconds(2000));
    ASSERT_TRUE(connectedReceived);
    
    bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    ASSERT_TRUE(accepted);
    ASSERT_NE(serverTransport, nullptr);
    
    serverTransport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::vector<std::uint8_t> receivedData;
    test_helpers::AsyncWaiter bytesWaiter;
    
    serverTransport->onBytes = [&receivedData, &bytesWaiter](const std::uint8_t* data, std::size_t size) {
        receivedData.insert(receivedData.end(), data, data + size);
        bytesWaiter.notify();
    };
    
    // Send data from client
    const std::string testMessage = "Hello from client!";
    client->asyncSend(reinterpret_cast<const std::uint8_t*>(testMessage.data()), testMessage.size());
    
    // Wait for data to be received
    bool received = bytesWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(received) << "Server should receive data";
    
    // Verify received data matches sent data
    EXPECT_EQ(receivedData.size(), testMessage.size()) << "Received data size should match sent data";
    EXPECT_TRUE(std::equal(receivedData.begin(), receivedData.end(),
                          testMessage.begin(), testMessage.end()))
        << "Received data should match sent data exactly";
    
    client->stop();
    serverTransport->stop();
    server->stop();
}

TEST_F(TcpClientTransportTest, ClientSendsMultipleQueuedMessages) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&serverTransport, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        acceptWaiter.notify();
    };
    
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    
    auto client = createClientTransport("127.0.0.1", testPort);
    client->onConnected = [&connected, &connectWaiter]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    bool connectedReceived = connectWaiter.wait_for(std::chrono::milliseconds(2000));
    ASSERT_TRUE(connectedReceived);
    
    bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    ASSERT_TRUE(accepted);
    ASSERT_NE(serverTransport, nullptr);
    
    serverTransport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::vector<std::uint8_t> allReceivedData;
    test_helpers::AsyncWaiter bytesWaiter;
    
    serverTransport->onBytes = [&allReceivedData, &bytesWaiter](const std::uint8_t* data, std::size_t size) {
        allReceivedData.insert(allReceivedData.end(), data, data + size);
        bytesWaiter.notify();
    };
    
    // Send multiple messages rapidly
    const std::string message1 = "First";
    const std::string message2 = "Second";
    const std::string message3 = "Third";
    
    client->asyncSend(reinterpret_cast<const std::uint8_t*>(message1.data()), message1.size());
    client->asyncSend(reinterpret_cast<const std::uint8_t*>(message2.data()), message2.size());
    client->asyncSend(reinterpret_cast<const std::uint8_t*>(message3.data()), message3.size());
    
    // Wait for all data to be received
    std::string expectedData = message1 + message2 + message3;
    
    // May receive in one or multiple chunks
    int attempts = 0;
    while (allReceivedData.size() < expectedData.size() && attempts < 10) {
        if (bytesWaiter.wait_for(std::chrono::milliseconds(500))) {
            bytesWaiter.reset();
        }
        attempts++;
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    EXPECT_EQ(allReceivedData.size(), expectedData.size()) << "Should receive all bytes";
    
    // Verify data matches and is in correct order
    EXPECT_TRUE(std::equal(allReceivedData.begin(), allReceivedData.end(),
                          expectedData.begin(), expectedData.end()))
        << "All messages should be received in order";
    
    client->stop();
    serverTransport->stop();
    server->stop();
}

TEST_F(TcpClientTransportTest, ClientHandlesConcurrentAsyncSendCalls) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&serverTransport, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        acceptWaiter.notify();
    };
    
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    
    auto client = createClientTransport("127.0.0.1", testPort);
    client->onConnected = [&connected, &connectWaiter]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    bool connectedReceived = connectWaiter.wait_for(std::chrono::milliseconds(2000));
    ASSERT_TRUE(connectedReceived);
    
    bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    ASSERT_TRUE(accepted);
    ASSERT_NE(serverTransport, nullptr);
    
    serverTransport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Send messages from multiple threads simultaneously
    const int numThreads = 4;
    const int messagesPerThread = 10;
    const std::string baseMessage = "Thread";
    
    std::vector<std::thread> threads;
    std::atomic<int> sendCount{0};
    
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&client, &baseMessage, &sendCount, t, messagesPerThread]() {
            for (int i = 0; i < messagesPerThread; ++i) {
                std::string msg = baseMessage + std::to_string(t) + "-" + std::to_string(i);
                client->asyncSend(reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size());
                sendCount++;
            }
        });
    }
    
    // Wait for all threads to finish
    for (auto& thread : threads) {
        thread.join();
    }
    
    EXPECT_EQ(sendCount.load(), numThreads * messagesPerThread) << "All asyncSend calls should complete";
    
    // Give time for all data to be sent
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // No crash means thread-safety is working
    // We don't verify exact data here, just that concurrent calls don't crash
    
    client->stop();
    serverTransport->stop();
    server->stop();
}

TEST_F(TcpClientTransportTest, ClientSendsBeforeConnectionEstablished) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&serverTransport, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        acceptWaiter.notify();
    };
    
    // Create client but don't start it yet
    auto client = createClientTransport("127.0.0.1", testPort);
    
    // Attempt to send message before connection is established - should throw
    const std::string testMessage = "Message before connection";
    EXPECT_THROW(
        client->asyncSend(reinterpret_cast<const std::uint8_t*>(testMessage.data()), testMessage.size()),
        std::runtime_error
    ) << "Should throw when sending before connection is established";
    
    // Now start the client
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    
    client->onConnected = [&connected, &connectWaiter]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    bool connectedReceived = connectWaiter.wait_for(std::chrono::milliseconds(2000));
    ASSERT_TRUE(connectedReceived);
    
    bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    ASSERT_TRUE(accepted);
    ASSERT_NE(serverTransport, nullptr);
    
    serverTransport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::vector<std::uint8_t> receivedData;
    test_helpers::AsyncWaiter bytesWaiter;
    
    serverTransport->onBytes = [&receivedData, &bytesWaiter](const std::uint8_t* data, std::size_t size) {
        receivedData.insert(receivedData.end(), data, data + size);
        bytesWaiter.notify();
    };
    
    // Now send after connection is established - should work
    client->asyncSend(reinterpret_cast<const std::uint8_t*>(testMessage.data()), testMessage.size());
    
    // Wait for the message to be received
    bool received = bytesWaiter.wait_for(std::chrono::milliseconds(2000));
    EXPECT_TRUE(received) << "Message should be sent after connection is established";
    
    // Verify received data matches
    EXPECT_EQ(receivedData.size(), testMessage.size()) << "Received data size should match sent data";
    EXPECT_TRUE(std::equal(receivedData.begin(), receivedData.end(),
                          testMessage.begin(), testMessage.end()))
        << "Received data should match sent data exactly";
    
    client->stop();
    serverTransport->stop();
    server->stop();
}

// 3.6 Error Handling

TEST_F(TcpClientTransportTest, ClientHandlesWriteErrors) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&serverTransport, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        acceptWaiter.notify();
    };
    
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    
    auto client = createClientTransport("127.0.0.1", testPort);
    client->onConnected = [&connected, &connectWaiter]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    bool connectedReceived = connectWaiter.wait_for(std::chrono::milliseconds(2000));
    ASSERT_TRUE(connectedReceived);
    
    bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    ASSERT_TRUE(accepted);
    ASSERT_NE(serverTransport, nullptr);
    
    serverTransport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Set up disconnection callback BEFORE stopping server (to avoid race condition)
    bool disconnected = false;
    test_helpers::AsyncWaiter disconnectWaiter;
    
    client->onDisconnected = [&disconnected, &disconnectWaiter](const std::string& reason) {
        disconnected = true;
        disconnectWaiter.notify();
    };
    
    // Close server connection
    serverTransport->stop();
    server->stop();
    
    // Wait for client to detect disconnection
    bool disconnectReceived = disconnectWaiter.wait_for(std::chrono::milliseconds(2000));
    ASSERT_TRUE(disconnectReceived) << "Client should detect server disconnection";
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Attempt to send after connection is closed - should throw
    const std::string testMessage = "This should fail";
    EXPECT_THROW(
        client->asyncSend(reinterpret_cast<const std::uint8_t*>(testMessage.data()), testMessage.size()),
        std::runtime_error
    ) << "Should throw when sending after connection is closed";
    
    client->stop();
}

TEST_F(TcpClientTransportTest, ClientHandlesInvalidHostPort) {
    // Test with invalid host that will fail to resolve
    bool errorOccurred = false;
    std::string errorMessage;
    test_helpers::AsyncWaiter errorWaiter;
    
    // Use a hostname that definitely won't resolve
    auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "invalid-host-that-does-not-exist-12345.example.com", "80");
    client->onError = [&errorOccurred, &errorMessage, &errorWaiter](const std::string& error) {
        errorOccurred = true;
        errorMessage = error;
        errorWaiter.notify();
    };
    
    client->start();
    
    // Wait for error callback (resolve should fail)
    bool errorReceived = errorWaiter.wait_for(std::chrono::milliseconds(3000));
    EXPECT_TRUE(errorReceived) << "onError callback should be invoked for invalid host";
    EXPECT_TRUE(errorOccurred) << "Error should be reported";
    EXPECT_NE(errorMessage.find("resolve:"), std::string::npos) << "Error message should indicate resolve failure";
    
    client->stop();
}

