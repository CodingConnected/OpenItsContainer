#include <gtest/gtest.h>
#include "tcp/TcpClientTransport.hpp"
#include "tcp/TcpServer.hpp"
#include "tls_test_helpers.hpp"
#include "../test_helpers.hpp"

#include <asio/io_context.hpp>
#include <asio/ssl.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/address.hpp>
#include <openssl/ssl.h>
#include <functional>
#include <system_error>
#include <memory>
#include <thread>
#include <chrono>
#include <vector>
#include <atomic>

namespace {

// Performs a synchronous TLS client handshake against 127.0.0.1:<port> using a
// context that the caller can weaken via `customize` (e.g. cap the protocol
// version or offer only a legacy cipher). Returns the handshake error_code,
// which is truthy when the server's hardened policy refuses the connection.
std::error_code attemptRawTlsHandshake(
        unsigned short port,
        const std::function<void(asio::ssl::context&)>& customize) {
    asio::io_context io;
    asio::ssl::context ctx(asio::ssl::context::tls_client);
    ctx.set_verify_mode(asio::ssl::verify_none);  // isolate the version/cipher policy under test
    customize(ctx);

    asio::ssl::stream<asio::ip::tcp::socket> stream(io, ctx);
    std::error_code ec;
    stream.next_layer().connect(
        asio::ip::tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
    if (ec) return ec;

    stream.handshake(asio::ssl::stream_base::client, ec);
    return ec;
}

} // namespace

class TlsIntegrationTest : public tls_test::TlsTestFixture {
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

TEST_F(TlsIntegrationTest, FullTlsConnectionBidirectionalDataTransfer) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    
    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    std::vector<std::uint8_t> serverReceived;
    std::vector<std::uint8_t> clientReceived;
    test_helpers::AsyncWaiter serverDataWaiter;
    test_helpers::AsyncWaiter clientDataWaiter;
    test_helpers::AsyncWaiter acceptWaiter;
    test_helpers::AsyncWaiter clientConnectedWaiter;
    test_helpers::AsyncWaiter serverConnectedWaiter;

    server_->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        // Set up the server's onBytes callback BEFORE starting
        transport->onBytes = [&](const std::uint8_t* data, std::size_t size) {
            serverReceived.insert(serverReceived.end(), data, data + size);
            if (serverReceived.size() >= 5) {
                serverDataWaiter.notify();
            }
        };
        // Signal when the server side has finished its TLS handshake. The
        // server must not send before this fires: asyncSend would race the
        // in-flight handshake on the SSL stream and the write would be lost.
        // (Under TLS 1.3 the client reaches onConnected before the server, so
        // gating the server send on the client's handshake is not enough.)
        transport->onConnected = [&]() {
            serverConnectedWaiter.notify();
        };
        transport->start();
        acceptWaiter.notify();
    };
    
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    auto client = tls_test::createTlsClient(*io_, "127.0.0.1", std::to_string(server_->port()), *certs_, false);
    
    client->onBytes = [&](const std::uint8_t* data, std::size_t size) {
        clientReceived.insert(clientReceived.end(), data, data + size);
        if (clientReceived.size() >= 5) {
            clientDataWaiter.notify();
        }
    };
    
    client->onConnected = [&]() {
        // Send data from client to server
        std::vector<std::uint8_t> clientData{1, 2, 3, 4, 5};
        client->asyncSend(clientData.data(), clientData.size());
        clientConnectedWaiter.notify();
    };
    
    client->start();
    
    // Wait for server to accept and both sides to complete the TLS handshake.
    EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_TRUE(clientConnectedWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_TRUE(serverConnectedWaiter.wait_for(std::chrono::milliseconds(2000)));

    // Now that both sides are ready, send data from server to client
    if (serverTransport) {
        std::vector<std::uint8_t> serverData{10, 20, 30, 40, 50};
        serverTransport->asyncSend(serverData.data(), serverData.size());
    }
    
    EXPECT_TRUE(serverDataWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_TRUE(clientDataWaiter.wait_for(std::chrono::milliseconds(2000)));
    
    EXPECT_EQ(serverReceived, (std::vector<std::uint8_t>{1, 2, 3, 4, 5}));
    EXPECT_EQ(clientReceived, (std::vector<std::uint8_t>{10, 20, 30, 40, 50}));
    
    client->stop();
    server_->stop();
}

TEST_F(TlsIntegrationTest, LargeDataTransferOverTls) {
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
    
    const size_t largeSize = 10000;
    std::vector<std::uint8_t> largeData(largeSize);
    for (size_t i = 0; i < largeSize; ++i) {
        largeData[i] = static_cast<std::uint8_t>(i % 256);
    }
    
    std::vector<std::uint8_t> receivedData;
    test_helpers::AsyncWaiter dataWaiter;
    
    auto client = tls_test::createTlsClient(*io_, "127.0.0.1", std::to_string(server_->port()), *certs_, false);
    client->onBytes = [&](const std::uint8_t* data, std::size_t size) {
        receivedData.insert(receivedData.end(), data, data + size);
        if (receivedData.size() >= largeSize) {
            dataWaiter.notify();
        }
    };
    
    client->start();
    
    EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(2000)));
    
    if (serverTransport) {
        serverTransport->asyncSend(largeData.data(), largeData.size());
    }
    
    EXPECT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(5000)));
    EXPECT_GE(receivedData.size(), largeSize);
    if (receivedData.size() >= largeSize) {
        for (size_t i = 0; i < largeSize; ++i) {
            EXPECT_EQ(receivedData[i], largeData[i]) << "Mismatch at index " << i;
        }
    }
    
    client->stop();
    server_->stop();
}

TEST_F(TlsIntegrationTest, MultipleConcurrentTlsConnections) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    
    std::atomic<int> connectionCount{0};
    test_helpers::AsyncWaiter allConnectedWaiter;
    
    server_->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
        int count = ++connectionCount;
        if (count >= 5) {
            allConnectedWaiter.notify();
        }
    };
    
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    std::vector<std::shared_ptr<tlcfi::tcp::TcpClientTransport>> clients;
    std::atomic<int> connectedClients{0};
    
    for (int i = 0; i < 5; ++i) {
        auto client = tls_test::createTlsClient(*io_, "127.0.0.1", std::to_string(server_->port()), *certs_, false);
        client->onConnected = [&]() {
            int count = ++connectedClients;
            if (count >= 5) {
                allConnectedWaiter.notify();
            }
        };
        clients.push_back(client);
        client->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    EXPECT_TRUE(allConnectedWaiter.wait_for(std::chrono::milliseconds(5000)));
    EXPECT_GE(connectionCount.load(), 5);
    EXPECT_GE(connectedClients.load(), 5);
    
    for (auto& client : clients) {
        client->stop();
    }
    server_->stop();
}

TEST_F(TlsIntegrationTest, TlsConnectionWithCertificateVerificationEnabled) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    
    server_->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
    };
    
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    bool connected = false;
    test_helpers::AsyncWaiter connectWaiter;
    
    // Client with verifyPeer = true (should work with valid cert)
    auto clientTlsConfig = tls_test::createClientTlsConfig(*certs_, true);
    auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(server_->port()), clientTlsConfig);
    client->onConnected = [&]() {
        connected = true;
        connectWaiter.notify();
    };
    
    client->start();
    
    // Should connect successfully with valid certificate
    EXPECT_TRUE(connectWaiter.wait_for(std::chrono::milliseconds(2000)));
    EXPECT_TRUE(connected);

    client->stop();
    server_->stop();
}

TEST_F(TlsIntegrationTest, HandshakeFailsWhenExpectedPeerNameDoesNotMatchCertificate) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);

    server_->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
    };

    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Client trusts the CA and verifies the peer, but expects an identity the
    // server certificate does not carry. The cert chains to the trusted CA, so
    // without identity checking this would succeed — the hostname mismatch must
    // make the handshake fail. This is the man-in-the-middle defense (5.3).
    tlcfi::tcp::TlsConfig clientTlsConfig(
        true,                    // enabled
        "", "",                  // no client cert/key
        certs_->caCertFile(),    // trust the test CA
        true,                    // verifyPeer
        "wrong.example.com");    // expected name absent from the cert SAN

    auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(
        *io_, "127.0.0.1", std::to_string(server_->port()), clientTlsConfig);

    bool connected = false;
    std::string errorMessage;
    test_helpers::AsyncWaiter resultWaiter;
    client->onConnected = [&]() {
        connected = true;
        resultWaiter.notify();
    };
    client->onError = [&](const std::string& msg) {
        errorMessage = msg;
        resultWaiter.notify();
    };

    client->start();

    // We must observe a failure, not a successful connection.
    EXPECT_TRUE(resultWaiter.wait_for(std::chrono::milliseconds(3000)));
    EXPECT_FALSE(connected) << "Handshake must not succeed when the peer name does not match the certificate";
    EXPECT_NE(errorMessage.find("Verification error"), std::string::npos)
        << "Expected a certificate verification failure. Got: " << errorMessage;

    client->stop();
    server_->stop();
}

TEST_F(TlsIntegrationTest, TlsConnectionWithCertificateVerificationDisabled) {
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

TEST_F(TlsIntegrationTest, ServerRejectsClientBelowTls12) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    server_->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
    };
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Client capped at TLS 1.1, below the server's TLS 1.2 floor (5.1).
    std::error_code ec = attemptRawTlsHandshake(server_->port(), [](asio::ssl::context& ctx) {
        SSL_CTX_set_max_proto_version(ctx.native_handle(), TLS1_1_VERSION);
    });
    EXPECT_TRUE(static_cast<bool>(ec))
        << "Handshake must fail: client max TLS 1.1 is below the server's TLS 1.2 floor";

    server_->stop();
}

TEST_F(TlsIntegrationTest, ServerRejectsClientOfferingOnlyLegacyCipher) {
    auto tlsConfig = tls_test::createServerTlsConfig(*certs_, false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, tlsConfig);
    server_->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
    };
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Client pinned to TLS 1.2 offering only a non-PFS CBC suite (5.2). The
    // server's hardened policy offers only ECDHE AEAD suites, so there is no
    // common cipher and the handshake must fail.
    std::error_code ec = attemptRawTlsHandshake(server_->port(), [](asio::ssl::context& ctx) {
        SSL_CTX_set_max_proto_version(ctx.native_handle(), TLS1_2_VERSION);
        EXPECT_EQ(SSL_CTX_set_cipher_list(ctx.native_handle(), "AES128-SHA"), 1)
            << "test precondition: client should accept the legacy cipher locally";
    });
    EXPECT_TRUE(static_cast<bool>(ec))
        << "Handshake must fail: no cipher overlap between legacy-only client and hardened server";

    server_->stop();
}



