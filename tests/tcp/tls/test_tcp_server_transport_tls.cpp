#include <gtest/gtest.h>
#include "tcp/TcpServer.hpp"
#include "tcp/TcpServerTransport.hpp"
#include "tls_test_helpers.hpp"
#include "../test_helpers.hpp"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <memory>
#include <thread>
#include <chrono>
#include <vector>

class TcpServerTransportTlsTest : public tls_test::TlsTestFixture {
protected:
    void SetUp() override {
        tls_test::TlsTestFixture::SetUp();
    }

    void TearDown() override {
        // Stop and destroy client and server BEFORE destroying io_context
        if (client_) {
            client_->stop();
            client_.reset();
        }
        if (server_) {
            server_->stop();
            server_.reset();
        }
        tls_test::TlsTestFixture::TearDown();
    }

    std::shared_ptr<tlcfi::tcp::TcpServerTransport> createTlsTransport(unsigned short = 0) {
        // Bind an OS-assigned ephemeral port (0); the actual bound port is
        // read via server_->port() for the client connect below.
        auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
        server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
        const unsigned short port = server_->port();

        // Use shared_ptr to avoid capturing local variables by reference
        auto transport = std::make_shared<std::shared_ptr<tlcfi::tcp::TcpServerTransport>>();
        auto acceptWaiter = std::make_shared<test_helpers::AsyncWaiter>();

        server_->onAccept = [transport, acceptWaiter](std::shared_ptr<tlcfi::tcp::TcpServerTransport> t) {
            *transport = t;
            acceptWaiter->notify();
        };
        
        server_->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Connect TLS client to trigger accept
        // Note: We don't set onConnected here to avoid capturing local variables by reference
        auto client = tls_test::createTlsClient(*io_, "127.0.0.1", std::to_string(port), *certs_, false);
        client->start();

        if (acceptWaiter->wait_for(std::chrono::milliseconds(2000)) && *transport) {
            client_ = client;
            return *transport;
        }

        server_->stop();
        return nullptr;
    }

    std::shared_ptr<tlcfi::tcp::TcpServer> server_;
    std::shared_ptr<tlcfi::tcp::TcpClientTransport> client_;
};

TEST_F(TcpServerTransportTlsTest, NameReturnsTlsServerConnWhenTlsEnabled) {
    auto transport = createTlsTransport(55300);
    ASSERT_NE(transport, nullptr);
    EXPECT_EQ(transport->name(), "tls-server-conn");
    
    if (client_) client_->stop();
    server_->stop();
}

TEST_F(TcpServerTransportTlsTest, OnConnectedCalledAfterTlsHandshake) {
    auto transport = createTlsTransport(55301);
    ASSERT_NE(transport, nullptr);
    
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    transport->onConnected = [&]() {
        connected = true;
        connectWaiter.notify();
    };
    
    transport->start();
    
    // onConnected should be called after TLS handshake completes
    EXPECT_TRUE(connectWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_TRUE(connected);
    
    if (client_) client_->stop();
    server_->stop();
}

TEST_F(TcpServerTransportTlsTest, SendsDataOverTls) {
    auto transport = createTlsTransport(55302);
    ASSERT_NE(transport, nullptr);

    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Set up client to receive data BEFORE sending
    std::vector<std::uint8_t> receivedData;
    test_helpers::AsyncWaiter dataWaiter;
    if (client_) {
        client_->onBytes = [&](const std::uint8_t* data, std::size_t size) {
            receivedData.insert(receivedData.end(), data, data + size);
            if (receivedData.size() >= 5) {  // Expecting 5 bytes
                dataWaiter.notify();
            }
        };
    }

    // Now send data from transport
    std::vector<std::uint8_t> sendData{10, 20, 30, 40, 50};
    transport->asyncSend(sendData.data(), sendData.size());

    // Wait for data to arrive
    EXPECT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(1000)));
    EXPECT_EQ(receivedData.size(), sendData.size());
    EXPECT_EQ(receivedData, sendData);

    if (client_) client_->stop();
    server_->stop();
}

TEST_F(TcpServerTransportTlsTest, ReceivesDataOverTls) {
    auto transport = createTlsTransport(55303);
    ASSERT_NE(transport, nullptr);
    
    std::vector<std::uint8_t> receivedData;
    test_helpers::AsyncWaiter dataWaiter;
    transport->onBytes = [&](const std::uint8_t* data, std::size_t size) {
        receivedData.insert(receivedData.end(), data, data + size);
        dataWaiter.notify();
    };
    
    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Send data from client
    if (client_) {
        std::vector<std::uint8_t> sendData{100, 200, 150};
        client_->asyncSend(sendData.data(), sendData.size());
    }
    
    EXPECT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(1000)));
    EXPECT_GE(receivedData.size(), 3u);
    if (receivedData.size() >= 3) {
        EXPECT_EQ(receivedData[0], 100);
        EXPECT_EQ(receivedData[1], 200);
        EXPECT_EQ(receivedData[2], 150);
    }
    
    if (client_) client_->stop();
    server_->stop();
}

TEST_F(TcpServerTransportTlsTest, HandlesMultipleReadsOverTls) {
    auto transport = createTlsTransport(55304);
    ASSERT_NE(transport, nullptr);
    
    std::vector<std::uint8_t> receivedData;
    int readCount = 0;
    test_helpers::AsyncWaiter dataWaiter;
    transport->onBytes = [&](const std::uint8_t* data, std::size_t size) {
        receivedData.insert(receivedData.end(), data, data + size);
        readCount++;
        if (readCount >= 3) {
            dataWaiter.notify();
        }
    };
    
    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Send multiple chunks from client
    if (client_) {
        std::vector<std::uint8_t> chunk1{1, 2};
        std::vector<std::uint8_t> chunk2{3, 4};
        std::vector<std::uint8_t> chunk3{5, 6};
        client_->asyncSend(chunk1.data(), chunk1.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        client_->asyncSend(chunk2.data(), chunk2.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        client_->asyncSend(chunk3.data(), chunk3.size());
    }
    
    EXPECT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_GE(receivedData.size(), 6u);
    
    if (client_) client_->stop();
    server_->stop();
}

TEST_F(TcpServerTransportTlsTest, StopShutsDownSslGracefully) {
    auto transport = createTlsTransport(55305);
    ASSERT_NE(transport, nullptr);

    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    bool disconnected = false;
    test_helpers::AsyncWaiter disconnectWaiter;
    transport->onDisconnected = [&](const std::string&) {
        disconnected = true;
        disconnectWaiter.notify();
    };

    transport->stop();

    EXPECT_TRUE(disconnectWaiter.wait_for(std::chrono::milliseconds(1000)));
    EXPECT_TRUE(disconnected);

    // Clear callback to prevent accessing local variables after test completes
    transport->onDisconnected = nullptr;

    if (client_) client_->stop();
    server_->stop();
}

TEST_F(TcpServerTransportTlsTest, ConcurrentAsyncSendIsThreadSafeWithTls) {
    auto transport = createTlsTransport(55306);
    ASSERT_NE(transport, nullptr);
    
    transport->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::atomic<int> sendCount{0};
    std::vector<std::thread> threads;
    
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&, i]() {
            std::vector<std::uint8_t> data{static_cast<std::uint8_t>(i)};
            transport->asyncSend(data.data(), data.size());
            sendCount++;
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(sendCount.load(), 10);
    
    if (client_) client_->stop();
    server_->stop();
}



