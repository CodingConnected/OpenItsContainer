#pragma once

#include "tcp/TlsConfig.hpp"
#include "tcp/TcpServer.hpp"
#include "tcp/TcpClientTransport.hpp"
#include "tls_test_certs.hpp"
#include "../test_helpers.hpp"

#include <asio/io_context.hpp>
#include <asio/ssl.hpp>
#include <memory>
#include <string>

namespace tls_test {

/**
 * @brief Creates a TlsConfig for server-side TLS testing.
 */
inline tlcfi::tcp::TlsConfig createServerTlsConfig(const TestCertificates& certs, bool verifyPeer = false) {
    return tlcfi::tcp::TlsConfig(
        true,  // enabled
        certs.serverCertFile(),
        certs.serverKeyFile(),
        certs.caCertFile(),
        verifyPeer
    );
}

/**
 * @brief Creates a TlsConfig for client-side TLS testing.
 */
inline tlcfi::tcp::TlsConfig createClientTlsConfig(const TestCertificates& certs, bool verifyPeer = true) {
    return tlcfi::tcp::TlsConfig(
        true,  // enabled
        "",  // certFile not needed for client
        "",  // keyFile not needed for client
        certs.caCertFile(),
        verifyPeer
    );
}

/**
 * @brief Creates a TlsConfig with TLS disabled (plain TCP).
 */
inline tlcfi::tcp::TlsConfig createDisabledTlsConfig() {
    return tlcfi::tcp::TlsConfig();
}

/**
 * @brief Helper to create a TLS-enabled server.
 */
inline std::shared_ptr<tlcfi::tcp::TcpServer> createTlsServer(
    asio::io_context& io,
    unsigned short port,
    const TestCertificates& certs,
    bool verifyPeer = false) {
    
    auto tlsConfig = createServerTlsConfig(certs, verifyPeer);
    return std::make_shared<tlcfi::tcp::TcpServer>(io, port, tlsConfig);
}

/**
 * @brief Helper to create a TLS-enabled client transport.
 */
inline std::shared_ptr<tlcfi::tcp::TcpClientTransport> createTlsClient(
    asio::io_context& io,
    const std::string& host,
    const std::string& port,
    const TestCertificates& certs,
    bool verifyPeer = false) {
    
    auto tlsConfig = createClientTlsConfig(certs, verifyPeer);
    return std::make_shared<tlcfi::tcp::TcpClientTransport>(io, host, port, tlsConfig);
}

/**
 * @brief Helper to wait for TLS handshake completion.
 * 
 * Waits for both TCP connection and TLS handshake to complete.
 */
class TlsHandshakeWaiter {
public:
    explicit TlsHandshakeWaiter(std::shared_ptr<tlcfi::tcp::ITransport> transport)
        : transport_(transport)
        , handshakeComplete_(false) {
        
        transport_->onConnected = [this]() {
            handshakeComplete_ = true;
            waiter_.notify();
        };
        
        transport_->onError = [this](const std::string& reason) {
            errorMessage_ = reason;
            waiter_.notify();
        };
    }

    bool wait_for(std::chrono::milliseconds timeout) {
        return waiter_.wait_for(timeout);
    }

    bool isComplete() const { return handshakeComplete_; }
    const std::string& errorMessage() const { return errorMessage_; }

private:
    std::shared_ptr<tlcfi::tcp::ITransport> transport_;
    test_helpers::AsyncWaiter waiter_;
    bool handshakeComplete_;
    std::string errorMessage_;
};

/**
 * @brief Test fixture base class for TLS tests.
 * 
 * Provides common setup: io_context, certificates, and cleanup.
 */
class TlsTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        io_ = std::make_unique<asio::io_context>();
        runner_ = std::make_unique<test_helpers::IoContextRunner>(*io_);
        certs_ = std::make_unique<TestCertificates>();
    }

    void TearDown() override {
        runner_.reset();
        io_.reset();
        certs_.reset();
    }

    std::unique_ptr<asio::io_context> io_;
    std::unique_ptr<test_helpers::IoContextRunner> runner_;
    std::unique_ptr<TestCertificates> certs_;
};

} // namespace tls_test



