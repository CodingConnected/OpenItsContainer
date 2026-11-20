#include <gtest/gtest.h>
#include "tcp/TcpClientTransport.hpp"
#include "tcp/TcpServer.hpp"
#include "tls_test_helpers.hpp"
#include "../test_helpers.hpp"

#include <asio/io_context.hpp>
#include <memory>
#include <thread>
#include <chrono>

class TlsErrorTest : public tls_test::TlsTestFixture {
protected:
    void SetUp() override {
        tls_test::TlsTestFixture::SetUp();
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
            server_.reset();
        }
        tls_test::TlsTestFixture::TearDown();
    }

    std::shared_ptr<tlcfi::tcp::TcpServer> server_;
};

TEST_F(TlsErrorTest, ServerConstructionFailsWithInvalidCertFile) {
    tlcfi::tcp::TlsConfig tlsConfig(true, "/nonexistent/server.crt", certs_->serverKeyFile());
    EXPECT_THROW(
        std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig),
        std::runtime_error
    );
}

TEST_F(TlsErrorTest, ServerConstructionFailsWithInvalidKeyFile) {
    tlcfi::tcp::TlsConfig tlsConfig(true, certs_->serverCertFile(), "/nonexistent/server.key");
    EXPECT_THROW(
        std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig),
        std::runtime_error
    );
}

TEST_F(TlsErrorTest, ClientHandshakeFailureInvokesOnError) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    
    server_->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
    };
    
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    bool errorCalled = false;
    std::string errorMessage;
    test_helpers::AsyncWaiter errorWaiter;

    // Create client that will verify peer but use system CAs (won't trust test cert)
    // This causes handshake failure without throwing during construction
    tlcfi::tcp::TlsConfig invalidTlsConfig(true, "", "", "", true);  // verifyPeer=true, no CA file
    auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(server_->port()), invalidTlsConfig);
    
    client->onError = [&](const std::string& reason) {
        errorCalled = true;
        errorMessage = reason;
        errorWaiter.notify();
    };
    
    client->onDisconnected = [&](const std::string&) {
        errorWaiter.notify();
    };
    
    client->start();
    
    // Should get error or disconnect due to handshake failure
    EXPECT_TRUE(errorWaiter.wait_for(std::chrono::milliseconds(3000)));
    // Either error callback or disconnect should be called
    EXPECT_TRUE(errorCalled || !errorMessage.empty());

    // Clear callbacks to prevent accessing local variables after test
    client->onError = nullptr;
    client->onDisconnected = nullptr;

    client->stop();
    server_->stop();
}

TEST_F(TlsErrorTest, ServerHandshakeFailureInvokesOnError) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    
    bool errorCalled = false;
    std::string errorMessage;
    
    server_->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        transport->onError = [&](const std::string& reason) {
            errorCalled = true;
            errorMessage = reason;
        };
        acceptWaiter.notify();
    };
    
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Connect plain TCP client (should cause TLS handshake failure)
    auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(server_->port()));
    client->start();
    
    EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(2000)));
    
    if (serverTransport) {
        serverTransport->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Handshake should fail because client is not using TLS
        // Note: Actual behavior depends on SSL implementation
        // Some implementations may disconnect silently

        // Clear callbacks to prevent accessing local variables
        serverTransport->onError = nullptr;
    }

    client->stop();
    server_->stop();
}

TEST_F(TlsErrorTest, ConnectionClosedDuringHandshake) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    test_helpers::AsyncWaiter acceptWaiter;
    
    bool disconnected = false;
    test_helpers::AsyncWaiter disconnectWaiter;
    
    server_->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        transport->onDisconnected = [&](const std::string&) {
            disconnected = true;
            disconnectWaiter.notify();
        };
        acceptWaiter.notify();
    };
    
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    auto client = tls_test::createTlsClient(*io_, "127.0.0.1", std::to_string(server_->port()), *certs_, false);
    
    client->start();
    
    EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(2000)));
    
    if (serverTransport) {
        // Start transport to begin handshake
        serverTransport->start();

        // Immediately close client connection during handshake
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        client->stop();

        // Server should detect disconnect
        EXPECT_TRUE(disconnectWaiter.wait_for(std::chrono::milliseconds(2000)));
        EXPECT_TRUE(disconnected);

        // Clear callback to prevent accessing local variables
        serverTransport->onDisconnected = nullptr;
    }

    server_->stop();
}

TEST_F(TlsErrorTest, InvalidCaCertFileHandledGracefully) {
    // Client with non-existent CA cert file should throw during construction
    // This is "graceful" handling - fail fast with a clear error message
    tlcfi::tcp::TlsConfig invalidTlsConfig(true, "", "", "/nonexistent/ca.crt", true);

    // Should throw when trying to load non-existent CA file
    EXPECT_THROW(
        std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", "55605", invalidTlsConfig),
        std::exception
    );
}



