#include <gtest/gtest.h>
#include "tcp/TcpServer.hpp"
#include "tls_test_helpers.hpp"
#include "../test_helpers.hpp"

#include <asio/io_context.hpp>
#include <memory>
#include <thread>
#include <chrono>

class TcpServerTlsTest : public tls_test::TlsTestFixture {
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

TEST_F(TcpServerTlsTest, ConstructionWithTlsDisabled) {
    // Should work without TLS config (backward compatibility)
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
    ASSERT_NE(server_, nullptr);
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server_->stop();
}

TEST_F(TcpServerTlsTest, ConstructionWithTlsEnabled) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    ASSERT_NE(server_, nullptr);
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    server_->stop();
}

TEST_F(TcpServerTlsTest, ConstructionFailsWhenTlsEnabledButCertMissing) {
    tlcfi::tcp::TlsConfig tlsConfig(true, "", "nonexistent.key");
    EXPECT_THROW(
        std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig),
        std::runtime_error
    );
}

TEST_F(TcpServerTlsTest, ConstructionFailsWhenTlsEnabledButKeyMissing) {
    tlcfi::tcp::TlsConfig tlsConfig(true, "nonexistent.crt", "");
    EXPECT_THROW(
        std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig),
        std::runtime_error
    );
}

TEST_F(TcpServerTlsTest, AcceptsTlsConnections) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    
    bool accepted = false;
    test_helpers::AsyncWaiter acceptWaiter;
    server_->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        accepted = true;
        EXPECT_EQ(transport->name(), "tls-server-conn");
        transport->start();  // Start the transport to perform TLS handshake
        acceptWaiter.notify();
    };
    
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Connect TLS client
    auto client = tls_test::createTlsClient(*io_, "127.0.0.1", std::to_string(server_->port()), *certs_, false);
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    client->onConnected = [&]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_TRUE(accepted);
    EXPECT_TRUE(connectWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_TRUE(connected);
    
    client->stop();
    server_->stop();
}

TEST_F(TcpServerTlsTest, CreatesPlainTcpTransportWhenTlsDisabled) {
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
    
    bool accepted = false;
    test_helpers::AsyncWaiter acceptWaiter;
    server_->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        accepted = true;
        EXPECT_EQ(transport->name(), "tcp-server-conn");
        transport->start();  // Start the transport to perform TLS handshake
        acceptWaiter.notify();
    };
    
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Connect plain TCP client
    auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(server_->port()));
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    client->onConnected = [&]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(1000)));
    EXPECT_TRUE(accepted);
    EXPECT_TRUE(connectWaiter.wait_for(std::chrono::milliseconds(1000)));
    EXPECT_TRUE(connected);
    
    client->stop();
    server_->stop();
}



