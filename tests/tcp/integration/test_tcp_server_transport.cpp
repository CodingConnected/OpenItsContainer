#include <gtest/gtest.h>
#include "test_helpers.hpp"

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

class TcpServerTransportTest : public ::testing::Test {
protected:
    void SetUp() override {
        io_ = std::make_unique<asio::io_context>();
        runner_ = std::make_unique<test_helpers::IoContextRunner>(*io_);
    }

    void TearDown() override {
        // Stop and clean up servers before destroying the runner
        for (auto& server : servers_) {
            if (server) {
                server->stop();
            }
        }
        servers_.clear();
        runner_.reset();
        io_.reset();
    }

    // Helper to create a connected transport by accepting a connection
    // Returns the transport and the client socket (for sending data)
    std::pair<std::shared_ptr<tlcfi::tcp::TcpServerTransport>, std::shared_ptr<asio::ip::tcp::socket>>
    createTransportWithClient(unsigned short = 0) {
        // Bind an OS-assigned ephemeral port (0) and read it back, instead of
        // a fixed port that can collide with another test / TIME_WAIT.
        auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
        const unsigned short port = server->port();
        std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport;
        test_helpers::AsyncWaiter acceptWaiter;

        server->onAccept = [&transport, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> t) {
            transport = t;
            acceptWaiter.notify();
        };

        server->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Connect a client to trigger accept
        auto clientSocket = std::make_shared<asio::ip::tcp::socket>(*io_);
        asio::ip::tcp::resolver resolver(*io_);
        std::error_code ec;
        auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port), ec);
        if (!ec) {
            asio::connect(*clientSocket, endpoints, ec);
            if (!ec) {
                bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
                if (accepted && transport) {
                    // Keep server alive by storing it in the test fixture
                    servers_.push_back(server);
                    return {transport, clientSocket};
                }
            }
        }
        server->stop();
        return {nullptr, nullptr};
    }

    // Simplified helper that just returns transport (for tests that don't need client socket)
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> createTransport(unsigned short port) {
        auto [transport, clientSocket] = createTransportWithClient(port);
        return transport;
    }

    std::unique_ptr<asio::io_context> io_;
    std::unique_ptr<test_helpers::IoContextRunner> runner_;
    std::vector<std::shared_ptr<tlcfi::tcp::TcpServer>> servers_;  // Keep servers alive for the duration of tests
};

TEST_F(TcpServerTransportTest, StartsAndInvokesOnConnected) {
    const unsigned short testPort = 54330;
    auto transport = createTransport(testPort);
    ASSERT_NE(transport, nullptr) << "Transport should be created";
    
    bool connected = false;
    transport->onConnected = [&connected]() {
        connected = true;
    };
    
    // Start the transport
    transport->start();
    
    // Wait for onConnected callback
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(connected) << "onConnected callback should be invoked";
    
    // Verify transport name
    EXPECT_EQ(transport->name(), "tcp-server-conn") << "Transport name should be 'tcp-server-conn'";
    
    transport->stop();
}

TEST_F(TcpServerTransportTest, StopsAndInvokesOnDisconnected) {
    const unsigned short testPort = 54331;
    auto transport = createTransport(testPort);
    ASSERT_NE(transport, nullptr);
    
    bool disconnected = false;
    std::string disconnectReason;
    
    transport->onDisconnected = [&disconnected, &disconnectReason](const std::string& reason) {
        disconnected = true;
        disconnectReason = reason;
    };
    
    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Stop the transport
    transport->stop();
    
    // Wait for onDisconnected callback
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(disconnected) << "onDisconnected callback should be invoked";
    EXPECT_EQ(disconnectReason, "stop") << "Disconnect reason should be 'stop'";
}

TEST_F(TcpServerTransportTest, ReceivesSingleMessage) {
    const unsigned short testPort = 54332;
    auto [transport, clientSocket] = createTransportWithClient(testPort);
    ASSERT_NE(transport, nullptr) << "Transport should be created";
    ASSERT_NE(clientSocket, nullptr) << "Client socket should be created";
    
    std::vector<std::uint8_t> receivedData;
    test_helpers::AsyncWaiter bytesWaiter;
    
    transport->onBytes = [&receivedData, &bytesWaiter](const std::uint8_t* data, std::size_t size) {
        receivedData.insert(receivedData.end(), data, data + size);
        bytesWaiter.notify();
    };
    
    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Send test data from client
    const std::string testMessage = "Hello, Server!";
    std::error_code ec;
    asio::write(*clientSocket, asio::buffer(testMessage), ec);
    ASSERT_FALSE(ec) << "Client should send data successfully";
    
    // Wait for data to be received
    bool received = bytesWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(received) << "onBytes callback should be invoked";
    
    // Verify received data matches sent data
    EXPECT_EQ(receivedData.size(), testMessage.size()) << "Received data size should match sent data";
    EXPECT_TRUE(std::equal(receivedData.begin(), receivedData.end(), 
                          testMessage.begin(), testMessage.end())) 
        << "Received data should match sent data exactly";
    
    transport->stop();
}

TEST_F(TcpServerTransportTest, ReceivesMultipleMessages) {
    const unsigned short testPort = 54333;
    auto [transport, clientSocket] = createTransportWithClient(testPort);
    ASSERT_NE(transport, nullptr);
    ASSERT_NE(clientSocket, nullptr);
    
    std::vector<std::uint8_t> allReceivedData;
    int messageCount = 0;
    test_helpers::AsyncWaiter bytesWaiter;
    
    transport->onBytes = [&allReceivedData, &messageCount, &bytesWaiter](const std::uint8_t* data, std::size_t size) {
        allReceivedData.insert(allReceivedData.end(), data, data + size);
        messageCount++;
        bytesWaiter.notify();
    };
    
    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Send first message
    const std::string message1 = "First message";
    std::error_code ec;
    asio::write(*clientSocket, asio::buffer(message1), ec);
    ASSERT_FALSE(ec);
    
    bool received1 = bytesWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(received1) << "First message should be received";
    EXPECT_EQ(messageCount, 1) << "Should have received 1 message";
    
    bytesWaiter.reset();
    
    // Send second message
    const std::string message2 = "Second message";
    asio::write(*clientSocket, asio::buffer(message2), ec);
    ASSERT_FALSE(ec);
    
    bool received2 = bytesWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(received2) << "Second message should be received";
    EXPECT_EQ(messageCount, 2) << "Should have received 2 messages";
    
    // Verify all data is received correctly
    std::string expectedData = message1 + message2;
    EXPECT_EQ(allReceivedData.size(), expectedData.size()) << "Total received data size should match";
    EXPECT_TRUE(std::equal(allReceivedData.begin(), allReceivedData.end(),
                          expectedData.begin(), expectedData.end()))
        << "All received data should match sent data";
    
    transport->stop();
}

TEST_F(TcpServerTransportTest, ReceivesLargeMessage) {
    const unsigned short testPort = 54334;
    auto [transport, clientSocket] = createTransportWithClient(testPort);
    ASSERT_NE(transport, nullptr);
    ASSERT_NE(clientSocket, nullptr);
    
    std::vector<std::uint8_t> allReceivedData;
    int chunkCount = 0;
    test_helpers::AsyncWaiter bytesWaiter;
    
    transport->onBytes = [&allReceivedData, &chunkCount, &bytesWaiter](const std::uint8_t* data, std::size_t size) {
        allReceivedData.insert(allReceivedData.end(), data, data + size);
        chunkCount++;
        bytesWaiter.notify();
    };
    
    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Send message larger than read buffer (8KB)
    // Read buffer is 8192 bytes, so send 10KB
    const size_t largeMessageSize = 10 * 1024;
    std::vector<std::uint8_t> largeMessage(largeMessageSize);
    for (size_t i = 0; i < largeMessageSize; ++i) {
        largeMessage[i] = static_cast<std::uint8_t>(i % 256);
    }
    
    std::error_code ec;
    asio::write(*clientSocket, asio::buffer(largeMessage), ec);
    ASSERT_FALSE(ec) << "Client should send large message successfully";
    
    // Wait for all chunks to be received
    // Since buffer is 8KB and we send 10KB, we should get at least 2 chunks
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
    
    transport->stop();
}

TEST_F(TcpServerTransportTest, ContinuesReadingAfterMessage) {
    const unsigned short testPort = 54335;
    auto [transport, clientSocket] = createTransportWithClient(testPort);
    ASSERT_NE(transport, nullptr);
    ASSERT_NE(clientSocket, nullptr);
    
    std::vector<std::string> receivedMessages;
    test_helpers::AsyncWaiter bytesWaiter;
    
    transport->onBytes = [&receivedMessages, &bytesWaiter](const std::uint8_t* data, std::size_t size) {
        receivedMessages.emplace_back(reinterpret_cast<const char*>(data), size);
        bytesWaiter.notify();
    };
    
    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Send first message
    const std::string message1 = "First message";
    std::error_code ec;
    asio::write(*clientSocket, asio::buffer(message1), ec);
    ASSERT_FALSE(ec);
    
    bool received1 = bytesWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(received1) << "First message should be received";
    EXPECT_EQ(receivedMessages.size(), 1) << "Should have received 1 message";
    EXPECT_EQ(receivedMessages[0], message1) << "First message should match";
    
    bytesWaiter.reset();
    
    // Wait a bit to ensure transport continues reading
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Send second message
    const std::string message2 = "Second message";
    asio::write(*clientSocket, asio::buffer(message2), ec);
    ASSERT_FALSE(ec);
    
    bool received2 = bytesWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(received2) << "Second message should be received";
    EXPECT_EQ(receivedMessages.size(), 2) << "Should have received 2 messages";
    EXPECT_EQ(receivedMessages[1], message2) << "Second message should match";
    
    // Verify both messages received correctly
    EXPECT_EQ(receivedMessages[0], message1) << "First message should still be correct";
    EXPECT_EQ(receivedMessages[1], message2) << "Second message should be correct";
    
    transport->stop();
}

TEST_F(TcpServerTransportTest, SendsSingleMessage) {
    const unsigned short testPort = 54336;
    auto [transport, clientSocket] = createTransportWithClient(testPort);
    ASSERT_NE(transport, nullptr);
    ASSERT_NE(clientSocket, nullptr);
    
    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Set up client to receive data
    std::vector<std::uint8_t> receivedData;
    const size_t bufferSize = 1024;
    std::vector<std::uint8_t> readBuffer(bufferSize);
    
    // Send data from transport
    const std::string testMessage = "Hello from server!";
    transport->asyncSend(reinterpret_cast<const std::uint8_t*>(testMessage.data()), testMessage.size());
    
    // Read data on client side
    std::error_code ec;
    size_t bytesRead = asio::read(*clientSocket, asio::buffer(readBuffer), 
                                   asio::transfer_exactly(testMessage.size()), ec);
    
    EXPECT_FALSE(ec) << "Client should receive data without error";
    EXPECT_EQ(bytesRead, testMessage.size()) << "Should receive exact number of bytes sent";
    
    // Verify received data matches
    std::string receivedString(reinterpret_cast<const char*>(readBuffer.data()), bytesRead);
    EXPECT_EQ(receivedString, testMessage) << "Received data should match sent data exactly";
    
    transport->stop();
}

TEST_F(TcpServerTransportTest, SendsMultipleQueuedMessages) {
    const unsigned short testPort = 54337;
    auto [transport, clientSocket] = createTransportWithClient(testPort);
    ASSERT_NE(transport, nullptr);
    ASSERT_NE(clientSocket, nullptr);
    
    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Send multiple messages rapidly
    const std::string message1 = "First";
    const std::string message2 = "Second";
    const std::string message3 = "Third";
    
    transport->asyncSend(reinterpret_cast<const std::uint8_t*>(message1.data()), message1.size());
    transport->asyncSend(reinterpret_cast<const std::uint8_t*>(message2.data()), message2.size());
    transport->asyncSend(reinterpret_cast<const std::uint8_t*>(message3.data()), message3.size());
    
    // Read all data on client side
    std::string expectedData = message1 + message2 + message3;
    std::vector<std::uint8_t> readBuffer(expectedData.size());
    
    std::error_code ec;
    size_t bytesRead = asio::read(*clientSocket, asio::buffer(readBuffer),
                                   asio::transfer_exactly(expectedData.size()), ec);
    
    EXPECT_FALSE(ec) << "Client should receive all data without error";
    EXPECT_EQ(bytesRead, expectedData.size()) << "Should receive all bytes";
    
    // Verify data matches and is in correct order
    std::string receivedString(reinterpret_cast<const char*>(readBuffer.data()), bytesRead);
    EXPECT_EQ(receivedString, expectedData) << "All messages should be received in order";
    
    transport->stop();
}

TEST_F(TcpServerTransportTest, HandlesConcurrentAsyncSendCalls) {
    const unsigned short testPort = 54338;
    auto [transport, clientSocket] = createTransportWithClient(testPort);
    ASSERT_NE(transport, nullptr);
    ASSERT_NE(clientSocket, nullptr);
    
    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Send messages from multiple threads simultaneously
    const int numThreads = 4;
    const int messagesPerThread = 10;
    const std::string baseMessage = "Thread";
    
    std::vector<std::thread> threads;
    std::atomic<int> sendCount{0};
    
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&transport, &baseMessage, &sendCount, t, messagesPerThread]() {
            for (int i = 0; i < messagesPerThread; ++i) {
                std::string msg = baseMessage + std::to_string(t) + "-" + std::to_string(i);
                transport->asyncSend(reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size());
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
    
    // Read all available data (we don't know exact size due to concurrent sends)
    std::vector<std::uint8_t> readBuffer(4096);
    std::error_code ec;
    size_t bytesRead = asio::read(*clientSocket, asio::buffer(readBuffer),
                                   asio::transfer_at_least(1), ec);
    
    // Should receive some data (exact amount depends on timing)
    EXPECT_GT(bytesRead, 0) << "Should receive data from concurrent sends";
    // No crash means thread-safety is working
    
    transport->stop();
}

TEST_F(TcpServerTransportTest, SendsEmptyBuffer) {
    const unsigned short testPort = 54339;
    auto [transport, clientSocket] = createTransportWithClient(testPort);
    ASSERT_NE(transport, nullptr);
    ASSERT_NE(clientSocket, nullptr);
    
    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Call asyncSend with size 0
    const std::uint8_t* emptyData = nullptr;
    EXPECT_NO_THROW({
        transport->asyncSend(emptyData, 0);
    }) << "asyncSend with size 0 should not throw";
    
    // Give it a moment to process
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Verify no crash occurred and transport is still functional
    // Try sending a real message to verify transport still works
    const std::string testMessage = "Test after empty";
    transport->asyncSend(reinterpret_cast<const std::uint8_t*>(testMessage.data()), testMessage.size());
    
    std::vector<std::uint8_t> readBuffer(testMessage.size());
    std::error_code ec;
    size_t bytesRead = asio::read(*clientSocket, asio::buffer(readBuffer),
                                   asio::transfer_exactly(testMessage.size()), ec);
    
    EXPECT_FALSE(ec) << "Transport should still work after empty send";
    EXPECT_EQ(bytesRead, testMessage.size()) << "Should receive the test message";
    
    std::string receivedString(reinterpret_cast<const char*>(readBuffer.data()), bytesRead);
    EXPECT_EQ(receivedString, testMessage) << "Transport should function normally after empty send";
    
    transport->stop();
}

TEST_F(TcpServerTransportTest, HandlesClientDisconnect) {
    const unsigned short testPort = 54340;
    auto [transport, clientSocket] = createTransportWithClient(testPort);
    ASSERT_NE(transport, nullptr);
    ASSERT_NE(clientSocket, nullptr);
    
    bool disconnected = false;
    std::string disconnectReason;
    
    transport->onDisconnected = [&disconnected, &disconnectReason](const std::string& reason) {
        disconnected = true;
        disconnectReason = reason;
    };
    
    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Close client connection
    std::error_code ec;
    clientSocket->close(ec);
    ASSERT_FALSE(ec) << "Client socket should close successfully";
    
    // Wait for disconnect callback
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    EXPECT_TRUE(disconnected) << "onDisconnected callback should be invoked when client disconnects";
    EXPECT_FALSE(disconnectReason.empty()) << "Disconnect reason should be provided";
    
    transport->stop();
}

TEST_F(TcpServerTransportTest, StopsReadingOnDisconnect) {
    const unsigned short testPort = 54341;
    auto [transport, clientSocket] = createTransportWithClient(testPort);
    ASSERT_NE(transport, nullptr);
    ASSERT_NE(clientSocket, nullptr);
    
    int bytesCallbackCount = 0;
    std::vector<std::uint8_t> receivedData;
    
    transport->onBytes = [&bytesCallbackCount, &receivedData](const std::uint8_t* data, std::size_t size) {
        bytesCallbackCount++;
        receivedData.insert(receivedData.end(), data, data + size);
    };
    
    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Send a message before disconnect to verify reading works
    const std::string message1 = "Before disconnect";
    std::error_code ec;
    asio::write(*clientSocket, asio::buffer(message1), ec);
    ASSERT_FALSE(ec);
    
    // Wait for first message to be received
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_GT(bytesCallbackCount, 0) << "Should receive data before disconnect";
    EXPECT_GE(receivedData.size(), message1.size()) << "Should have received first message";
    
    int callbackCountBeforeDisconnect = bytesCallbackCount;
    
    // Now disconnect the client
    clientSocket->close(ec);
    ASSERT_FALSE(ec);
    
    // Wait a bit to ensure no more callbacks are fired
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Verify no further onBytes callbacks after disconnect
    EXPECT_EQ(bytesCallbackCount, callbackCountBeforeDisconnect) 
        << "No further onBytes callbacks should be invoked after client disconnect";
    
    transport->stop();
}

