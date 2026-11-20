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

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        io_ = std::make_unique<asio::io_context>();
        runner_ = std::make_unique<test_helpers::IoContextRunner>(*io_);
    }

    void TearDown() override {
        runner_.reset();
        io_.reset();
    }

    // Creates a server on an OS-assigned ephemeral port (0); the actual
    // bound port is available via server->port(). Avoids fixed-port
    // collisions / TIME_WAIT flakiness across tests.
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

    // Helper to wait for client connection
    bool waitForConnection(std::shared_ptr<tlcfi::tcp::TcpClientTransport> client, std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
        bool connected = false;
        test_helpers::AsyncWaiter connectWaiter;
        
        client->onConnected = [&connected, &connectWaiter]() {
            connected = true;
            connectWaiter.notify();
        };
        
        client->start();
        return connectWaiter.wait_for(timeout) && connected;
    }

    std::unique_ptr<asio::io_context> io_;
    std::unique_ptr<test_helpers::IoContextRunner> runner_;
};

// 4.1 Full Communication Flow

TEST_F(IntegrationTest, ClientServerBidirectionalCommunication) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&serverTransport, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        acceptWaiter.notify();
    };
    
    auto client = createClientTransport("127.0.0.1", testPort);
    ASSERT_TRUE(waitForConnection(client)) << "Client should connect";
    
    bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    ASSERT_TRUE(accepted) << "Server should accept connection";
    ASSERT_NE(serverTransport, nullptr) << "Server transport should be created";
    
    serverTransport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Set up data reception on both sides
    std::vector<std::uint8_t> clientReceivedData;
    std::vector<std::uint8_t> serverReceivedData;
    test_helpers::AsyncWaiter clientBytesWaiter;
    test_helpers::AsyncWaiter serverBytesWaiter;
    
    client->onBytes = [&clientReceivedData, &clientBytesWaiter](const std::uint8_t* data, std::size_t size) {
        clientReceivedData.insert(clientReceivedData.end(), data, data + size);
        clientBytesWaiter.notify();
    };
    
    serverTransport->onBytes = [&serverReceivedData, &serverBytesWaiter](const std::uint8_t* data, std::size_t size) {
        serverReceivedData.insert(serverReceivedData.end(), data, data + size);
        serverBytesWaiter.notify();
    };

    // Client sends message to server
    const std::string clientMessage = "Hello from client!";
    client->asyncSend(reinterpret_cast<const std::uint8_t*>(clientMessage.data()), clientMessage.size());
    
    bool serverReceived = serverBytesWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(serverReceived) << "Server should receive client message";
    EXPECT_EQ(serverReceivedData.size(), clientMessage.size()) << "Server should receive correct data size";
    EXPECT_TRUE(std::equal(serverReceivedData.begin(), serverReceivedData.end(),
                          clientMessage.begin(), clientMessage.end()))
        << "Server should receive correct data";
    
    // Server sends message to client
    const std::string serverMessage = "Hello from server!";
    serverTransport->asyncSend(reinterpret_cast<const std::uint8_t*>(serverMessage.data()), serverMessage.size());
    
    bool clientReceived = clientBytesWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(clientReceived) << "Client should receive server message";
    EXPECT_EQ(clientReceivedData.size(), serverMessage.size()) << "Client should receive correct data size";
    EXPECT_TRUE(std::equal(clientReceivedData.begin(), clientReceivedData.end(),
                          serverMessage.begin(), serverMessage.end()))
        << "Client should receive correct data";
    
    client->stop();
    serverTransport->stop();
    server->stop();
}

TEST_F(IntegrationTest, MultipleClientsCommunicateWithServer) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    std::vector<std::shared_ptr<tlcfi::tcp::TcpServerTransport>> serverTransports;
    std::atomic<int> acceptCount{0};
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&serverTransports, &acceptCount, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransports.push_back(transport);
        acceptCount++;
        acceptWaiter.notify();
    };
    
    // Connect first client
    auto client1 = createClientTransport("127.0.0.1", testPort);
    ASSERT_TRUE(waitForConnection(client1)) << "Client 1 should connect";
    
    bool accepted1 = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    ASSERT_TRUE(accepted1) << "Server should accept client 1";
    ASSERT_EQ(serverTransports.size(), 1) << "Should have 1 server transport";
    
    serverTransports[0]->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Small delay to ensure server has set up next async accept
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Reset accept waiter
    acceptWaiter.reset();

    // Connect second client
    auto client2 = createClientTransport("127.0.0.1", testPort);
    ASSERT_TRUE(waitForConnection(client2)) << "Client 2 should connect";
    
    bool accepted2 = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    ASSERT_TRUE(accepted2) << "Server should accept client 2";
    ASSERT_EQ(serverTransports.size(), 2) << "Should have 2 server transports";
    
    serverTransports[1]->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Set up data reception
    std::vector<std::uint8_t> client1ReceivedData;
    std::vector<std::uint8_t> client2ReceivedData;
    std::vector<std::uint8_t> server1ReceivedData;
    std::vector<std::uint8_t> server2ReceivedData;
    
    test_helpers::AsyncWaiter client1BytesWaiter;
    test_helpers::AsyncWaiter client2BytesWaiter;
    test_helpers::AsyncWaiter server1BytesWaiter;
    test_helpers::AsyncWaiter server2BytesWaiter;
    
    client1->onBytes = [&client1ReceivedData, &client1BytesWaiter](const std::uint8_t* data, std::size_t size) {
        client1ReceivedData.insert(client1ReceivedData.end(), data, data + size);
        client1BytesWaiter.notify();
    };
    
    client2->onBytes = [&client2ReceivedData, &client2BytesWaiter](const std::uint8_t* data, std::size_t size) {
        client2ReceivedData.insert(client2ReceivedData.end(), data, data + size);
        client2BytesWaiter.notify();
    };
    
    serverTransports[0]->onBytes = [&server1ReceivedData, &server1BytesWaiter](const std::uint8_t* data, std::size_t size) {
        server1ReceivedData.insert(server1ReceivedData.end(), data, data + size);
        server1BytesWaiter.notify();
    };
    
    serverTransports[1]->onBytes = [&server2ReceivedData, &server2BytesWaiter](const std::uint8_t* data, std::size_t size) {
        server2ReceivedData.insert(server2ReceivedData.end(), data, data + size);
        server2BytesWaiter.notify();
    };
    
    // Each client sends unique message
    const std::string message1 = "Message from client 1";
    const std::string message2 = "Message from client 2";
    
    client1->asyncSend(reinterpret_cast<const std::uint8_t*>(message1.data()), message1.size());
    client2->asyncSend(reinterpret_cast<const std::uint8_t*>(message2.data()), message2.size());
    
    // Wait for server to receive both messages
    bool server1Received = server1BytesWaiter.wait_for(std::chrono::milliseconds(1000));
    bool server2Received = server2BytesWaiter.wait_for(std::chrono::milliseconds(1000));
    
    EXPECT_TRUE(server1Received) << "Server should receive message from client 1";
    EXPECT_TRUE(server2Received) << "Server should receive message from client 2";
    EXPECT_EQ(server1ReceivedData.size(), message1.size()) << "Server 1 should receive correct data size";
    EXPECT_EQ(server2ReceivedData.size(), message2.size()) << "Server 2 should receive correct data size";
    EXPECT_TRUE(std::equal(server1ReceivedData.begin(), server1ReceivedData.end(),
                          message1.begin(), message1.end()))
        << "Server 1 should receive correct message";
    EXPECT_TRUE(std::equal(server2ReceivedData.begin(), server2ReceivedData.end(),
                          message2.begin(), message2.end()))
        << "Server 2 should receive correct message";
    
    // Server sends response to each client
    const std::string response1 = "Response to client 1";
    const std::string response2 = "Response to client 2";
    
    // Reset waiters
    client1BytesWaiter.reset();
    client2BytesWaiter.reset();
    
    // Send responses
    serverTransports[0]->asyncSend(reinterpret_cast<const std::uint8_t*>(response1.data()), response1.size());
    serverTransports[1]->asyncSend(reinterpret_cast<const std::uint8_t*>(response2.data()), response2.size());
    
    // Wait for clients to receive responses
    bool client1Received = client1BytesWaiter.wait_for(std::chrono::milliseconds(1000));
    bool client2Received = client2BytesWaiter.wait_for(std::chrono::milliseconds(1000));
    
    EXPECT_TRUE(client1Received) << "Client 1 should receive response";
    EXPECT_TRUE(client2Received) << "Client 2 should receive response";
    EXPECT_EQ(client1ReceivedData.size(), response1.size()) << "Client 1 should receive correct response size";
    EXPECT_EQ(client2ReceivedData.size(), response2.size()) << "Client 2 should receive correct response size";
    EXPECT_TRUE(std::equal(client1ReceivedData.begin(), client1ReceivedData.end(),
                          response1.begin(), response1.end()))
        << "Client 1 should receive correct response";
    EXPECT_TRUE(std::equal(client2ReceivedData.begin(), client2ReceivedData.end(),
                          response2.begin(), response2.end()))
        << "Client 2 should receive correct response";
    
    client1->stop();
    client2->stop();
    serverTransports[0]->stop();
    serverTransports[1]->stop();
    server->stop();
}

// 4.2 Stress Tests

TEST_F(IntegrationTest, RapidConnectDisconnectCycles) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    std::atomic<int> acceptCount{0};
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&acceptCount, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        acceptCount++;
        acceptWaiter.notify();
        // Don't start transport, just accept and let it close
    };
    
    // Perform 15 rapid connect/disconnect cycles
    const int cycles = 15;
    for (int i = 0; i < cycles; ++i) {
        auto client = createClientTransport("127.0.0.1", testPort);
        
        bool connected = false;
        test_helpers::AsyncWaiter connectWaiter;
        client->onConnected = [&connected, &connectWaiter]() {
            connected = true;
            connectWaiter.notify();
        };
        
        client->start();
        
        // Wait for connection (with timeout)
        connectWaiter.wait_for(std::chrono::milliseconds(1000));
        
        // Wait for server to accept
        acceptWaiter.wait_for(std::chrono::milliseconds(500));
        acceptWaiter.reset();
        
        // Disconnect client
        client->stop();
        
        // Small delay between cycles
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Reset accept waiter
    acceptWaiter.reset();
    
    // Verify server continues accepting
    auto finalClient = createClientTransport("127.0.0.1", testPort);
    ASSERT_TRUE(waitForConnection(finalClient)) << "Server should still accept connections after cycles";
    
    bool finalAccepted = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    EXPECT_TRUE(finalAccepted) << "Server should accept final connection";
    EXPECT_GE(acceptCount.load(), cycles) << "Server should have accepted at least " << cycles << " connections";
    
    finalClient->stop();
    server->stop();
}

TEST_F(IntegrationTest, HighMessageThroughput) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&serverTransport, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        acceptWaiter.notify();
    };
    
    auto client = createClientTransport("127.0.0.1", testPort);
    ASSERT_TRUE(waitForConnection(client)) << "Client should connect";
    
    bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    ASSERT_TRUE(accepted);
    ASSERT_NE(serverTransport, nullptr);
    
    serverTransport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::vector<std::uint8_t> allReceivedData;
    std::atomic<int> messageCount{0};
    test_helpers::AsyncWaiter bytesWaiter;
    
    serverTransport->onBytes = [&allReceivedData, &messageCount, &bytesWaiter](const std::uint8_t* data, std::size_t size) {
        allReceivedData.insert(allReceivedData.end(), data, data + size);
        messageCount++;
        bytesWaiter.notify();
    };
    
    // Send 150 messages rapidly
    const int numMessages = 150;
    const std::string baseMessage = "Message ";
    std::string expectedData;
    
    for (int i = 0; i < numMessages; ++i) {
        std::string msg = baseMessage + std::to_string(i) + "\n";
        client->asyncSend(reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size());
        expectedData += msg;
    }
    
    // Wait for all messages to be received
    int attempts = 0;
    while (allReceivedData.size() < expectedData.size() && attempts < 200) {
        if (bytesWaiter.wait_for(std::chrono::milliseconds(100))) {
            bytesWaiter.reset();
        }
        attempts++;
    }
    
    // Give a bit more time for any remaining data
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    EXPECT_EQ(allReceivedData.size(), expectedData.size()) << "All messages should be received";
    EXPECT_TRUE(std::equal(allReceivedData.begin(), allReceivedData.end(),
                          expectedData.begin(), expectedData.end()))
        << "All received data should match sent data exactly";
    
    client->stop();
    serverTransport->stop();
    server->stop();
}

TEST_F(IntegrationTest, LargeDataTransfer) {
    auto server = createServer();
    const unsigned short testPort = server->port();
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    
    server->onAccept = [&serverTransport, &acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        acceptWaiter.notify();
    };
    
    auto client = createClientTransport("127.0.0.1", testPort);
    ASSERT_TRUE(waitForConnection(client)) << "Client should connect";
    
    bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(1000));
    ASSERT_TRUE(accepted);
    ASSERT_NE(serverTransport, nullptr);
    
    serverTransport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::vector<std::uint8_t> allReceivedData;
    int chunkCount = 0;
    test_helpers::AsyncWaiter bytesWaiter;
    
    serverTransport->onBytes = [&allReceivedData, &chunkCount, &bytesWaiter](const std::uint8_t* data, std::size_t size) {
        allReceivedData.insert(allReceivedData.end(), data, data + size);
        chunkCount++;
        bytesWaiter.notify();
    };
    
    // Send 1MB message
    const size_t largeMessageSize = 1024 * 1024; // 1MB
    std::vector<std::uint8_t> largeMessage(largeMessageSize);
    for (size_t i = 0; i < largeMessageSize; ++i) {
        largeMessage[i] = static_cast<std::uint8_t>(i % 256);
    }
    
    client->asyncSend(largeMessage.data(), largeMessageSize);
    
    // Wait for all chunks to be received
    int attempts = 0;
    while (allReceivedData.size() < largeMessageSize && attempts < 500) {
        if (bytesWaiter.wait_for(std::chrono::milliseconds(200))) {
            bytesWaiter.reset();
        }
        attempts++;
    }
    
    // Give more time for any remaining chunks
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    
    EXPECT_EQ(allReceivedData.size(), largeMessageSize) << "Complete 1MB message should be received";
    
    // Verify data integrity by checking first, middle, and last bytes
    EXPECT_EQ(allReceivedData[0], largeMessage[0]) << "First byte should match";
    EXPECT_EQ(allReceivedData[largeMessageSize / 2], largeMessage[largeMessageSize / 2]) << "Middle byte should match";
    EXPECT_EQ(allReceivedData[largeMessageSize - 1], largeMessage[largeMessageSize - 1]) << "Last byte should match";
    
    // Verify complete data integrity
    EXPECT_TRUE(std::equal(allReceivedData.begin(), allReceivedData.end(),
                          largeMessage.begin(), largeMessage.end()))
        << "Complete message should match sent data exactly with no corruption";
    
    client->stop();
    serverTransport->stop();
    server->stop();
}

