#include <gtest/gtest.h>
#include "tcp/TcpClientTransport.hpp"
#include "tcp/TcpServer.hpp"
#include "tls_test_helpers.hpp"
#include "../test_helpers.hpp"

#include <asio/io_context.hpp>
#include <memory>
#include <thread>
#include <chrono>
#include <vector>

class TcpClientTransportTlsTest : public tls_test::TlsTestFixture {
protected:
    void SetUp() override {
        tls_test::TlsTestFixture::SetUp();
    }

    void TearDown() override {
        // Destroy server_ before the base TearDown destroys io_context.
        // ~TcpServer() touches acceptor_, which references io_; if io_ is
        // destroyed first the close races a dangling executor (SEH 0xC0000005).
        if (server_) {
            server_->stop();
            server_.reset();
        }
        tls_test::TlsTestFixture::TearDown();
    }

    std::shared_ptr<tlcfi::tcp::TcpServer> server_;
};

TEST_F(TcpClientTransportTlsTest, NameReturnsTlsClientWhenTlsEnabled) {
    auto client = tls_test::createTlsClient(*io_, "127.0.0.1", "55400", *certs_, false);
    EXPECT_EQ(client->name(), "tls-client");
}

TEST_F(TcpClientTransportTlsTest, NameReturnsTcpClientWhenTlsDisabled) {
    auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", "55401");
    EXPECT_EQ(client->name(), "tcp-client");
}

TEST_F(TcpClientTransportTlsTest, ConnectsToTlsServer) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    
    server_->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
    };
    
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    auto client = tls_test::createTlsClient(*io_, "127.0.0.1", std::to_string(server_->port()), *certs_, false);
    
    client->onConnected = [&]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    EXPECT_TRUE(connectWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_TRUE(connected);
    
    client->stop();
    server_->stop();
    
    // Give time for pending async operations to complete before io_context is destroyed
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(TcpClientTransportTlsTest, OnConnectedCalledAfterTlsHandshake) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    
    server_->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
    };
    
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    bool handshakeComplete = false;
    test_helpers::AsyncWaiter connectWaiter;
    auto client = tls_test::createTlsClient(*io_, "127.0.0.1", std::to_string(server_->port()), *certs_, false);
    client->onConnected = [&]() {
        handshakeComplete = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    // onConnected should be called after TLS handshake completes
    EXPECT_TRUE(connectWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_TRUE(handshakeComplete);
    
    client->stop();
    server_->stop();
}

TEST_F(TcpClientTransportTlsTest, SendsDataOverTls) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    server_->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        transport->start();
        acceptWaiter.notify();
    };
    
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    std::vector<std::uint8_t> receivedData;
    test_helpers::AsyncWaiter dataWaiter;
    
    auto client = tls_test::createTlsClient(*io_, "127.0.0.1", std::to_string(server_->port()), *certs_, false);
    client->onConnected = [&]() {
        std::vector<std::uint8_t> sendData{1, 2, 3, 4, 5};
        client->asyncSend(sendData.data(), sendData.size());
    };
    
    if (serverTransport) {
        serverTransport->onBytes = [&](const std::uint8_t* data, std::size_t size) {
            receivedData.insert(receivedData.end(), data, data + size);
            dataWaiter.notify();
        };
    }
    
    client->start();
    
    EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(1000)));
    if (serverTransport) {
        serverTransport->onBytes = [&](const std::uint8_t* data, std::size_t size) {
            receivedData.insert(receivedData.end(), data, data + size);
            dataWaiter.notify();
        };
    }
    
    EXPECT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_EQ(receivedData, (std::vector<std::uint8_t>{1, 2, 3, 4, 5}));
    
    client->stop();
    server_->stop();
}

TEST_F(TcpClientTransportTlsTest, ReceivesDataOverTls) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    test_helpers::AsyncWaiter serverConnectedWaiter;
    server_->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        // Don't send before the server's own handshake completes: asyncSend
        // would race the in-flight handshake on the SSL stream (the write is
        // silently lost). Under TLS 1.3 the client connects before the server
        // finishes, so waiting on the client side alone is not enough.
        transport->onConnected = [&]() { serverConnectedWaiter.notify(); };
        transport->start();
        acceptWaiter.notify();
    };

    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::vector<std::uint8_t> receivedData;
    test_helpers::AsyncWaiter dataWaiter;

    auto client = tls_test::createTlsClient(*io_, "127.0.0.1", std::to_string(server_->port()), *certs_, false);
    client->onBytes = [&](const std::uint8_t* data, std::size_t size) {
        receivedData.insert(receivedData.end(), data, data + size);
        dataWaiter.notify();
    };

    client->start();

    EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(1000)));
    EXPECT_TRUE(serverConnectedWaiter.wait_for(std::chrono::milliseconds(2000)));

    if (serverTransport) {
        std::vector<std::uint8_t> sendData{10, 20, 30};
        serverTransport->asyncSend(sendData.data(), sendData.size());
    }
    
    EXPECT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_EQ(receivedData, (std::vector<std::uint8_t>{10, 20, 30}));
    
    client->stop();
    server_->stop();
}

TEST_F(TcpClientTransportTlsTest, StopShutsDownSslGracefully) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    
    server_->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
    };
    
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    bool disconnected = false;
    test_helpers::AsyncWaiter disconnectWaiter;
    
    auto client = tls_test::createTlsClient(*io_, "127.0.0.1", std::to_string(server_->port()), *certs_, false);
    client->onConnected = [&]() {
        client->stop();
    };
    client->onDisconnected = [&](const std::string&) {
        disconnected = true;
        disconnectWaiter.notify();
    };
    
    client->start();
    
    EXPECT_TRUE(disconnectWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_TRUE(disconnected);
    
    server_->stop();
}

TEST_F(TcpClientTransportTlsTest, CanManuallyReconnectWithTlsAfterDisconnect) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    
    int connectCount = 0;
    test_helpers::AsyncWaiter connectWaiter;
    
    server_->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
    };
    
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    auto client = tls_test::createTlsClient(*io_, "127.0.0.1", std::to_string(server_->port()), *certs_, false);
    client->onConnected = [&]() {
        connectCount++;
        connectWaiter.notify();
    };
    
    // First connection
    client->start();
    EXPECT_TRUE(connectWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_EQ(connectCount, 1) << "Should connect initially";
    
    // Stop client
    client->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Manually reconnect by calling start() again
    connectWaiter.reset();
    client->start();
    
    // Should reconnect and complete TLS handshake again
    EXPECT_TRUE(connectWaiter.wait_for(std::chrono::milliseconds(3000)));
    EXPECT_EQ(connectCount, 2) << "Should reconnect when start() is called again";
    
    client->stop();
    server_->stop();
}

TEST_F(TcpClientTransportTlsTest, WorksWithVerifyPeerDisabled) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    
    server_->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
    };
    
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    
    // Client with verifyPeer = false
    auto clientTlsConfig = tls_test::createClientTlsConfig(*certs_, false);
    auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(server_->port()), clientTlsConfig);
    client->onConnected = [&]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    EXPECT_TRUE(connectWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_TRUE(connected);
    
    client->stop();
    server_->stop();
}



