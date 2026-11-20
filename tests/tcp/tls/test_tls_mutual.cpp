#include <gtest/gtest.h>
#include "tcp/TcpClientTransport.hpp"
#include "tcp/TcpServer.hpp"
#include "tls_test_helpers.hpp"
#include "../test_helpers.hpp"

#include <asio/io_context.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

// TLS negative tests (remediation plan Phase 4.6):
//   - a server that requires a client certificate (verifyPeer = true ->
//     verify_peer | verify_fail_if_no_peer_cert) must reject a client that
//     presents none;
//   - a client that verifies the server certificate against the wrong CA
//     must reject the handshake.
//
// Note on coverage: TcpClientTransport currently has no API to load a client
// certificate, so the "client presents a wrong cert" mutual-TLS case is not
// reachable. The reachable enforcement check is therefore "client presents no
// cert when the server requires one", which is what these tests exercise.

class TlsMutualTest : public tls_test::TlsTestFixture {
protected:
    void TearDown() override {
        if (server_) {
            server_->stop();
            server_.reset();
        }
        tls_test::TlsTestFixture::TearDown();
    }

    std::shared_ptr<tlcfi::tcp::TcpServer> server_;
};

// A server configured with verifyPeer = true demands a client certificate.
// The client cannot supply one, so the handshake must fail and the client
// must observe an error or a disconnect rather than a successful connect.
TEST_F(TlsMutualTest, ServerRequiringClientCertRejectsClientWithoutCert) {
    // verifyPeer = true -> verify_peer | verify_fail_if_no_peer_cert.
    auto serverTls = tls_test::createServerTlsConfig(*certs_, /*verifyPeer=*/true);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, serverTls);

    server_->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
    };
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Client trusts the test CA (so the server cert verifies fine) but has no
    // client certificate of its own to present.
    auto clientTls = tls_test::createClientTlsConfig(*certs_, /*verifyPeer=*/true);
    auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(
        *io_, "127.0.0.1", std::to_string(server_->port()), clientTls);

    std::atomic<bool> failed{false};  // an error or disconnect was observed
    test_helpers::AsyncWaiter failureWaiter;

    client->onConnected = [&]() {};  // see note below: may fire under TLS 1.3
    client->onError = [&](const std::string&) { failed = true; failureWaiter.notify(); };
    client->onDisconnected = [&](const std::string&) { failed = true; failureWaiter.notify(); };

    client->start();

    // The server requires a client certificate the client cannot supply, so the
    // connection must be rejected. Under TLS 1.2 the client's handshake fails
    // outright; under TLS 1.3 the client may briefly reach onConnected before
    // the server's post-handshake alert tears the connection down. The
    // version-independent property is that a disconnect/error arrives and no
    // usable session survives.
    EXPECT_TRUE(failureWaiter.wait_for(std::chrono::milliseconds(3000)))
        << "client must observe a handshake failure / disconnect";
    EXPECT_TRUE(failed.load())
        << "handshake must not yield a usable session when the server requires a client cert";

    client->onConnected = nullptr;
    client->onError = nullptr;
    client->onDisconnected = nullptr;
    client->stop();
    server_->stop();
}

// The server side of the same scenario: the accepted transport's handshake
// must fail (error or disconnect), never reach onConnected.
TEST_F(TlsMutualTest, ServerRequiringClientCertFailsServerHandshake) {
    auto serverTls = tls_test::createServerTlsConfig(*certs_, /*verifyPeer=*/true);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, serverTls);

    std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
    bool serverConnected = false;
    test_helpers::AsyncWaiter resultWaiter;

    server_->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        serverTransport = transport;
        transport->onConnected = [&]() {
            serverConnected = true;
            resultWaiter.notify();
        };
        transport->onError = [&](const std::string&) { resultWaiter.notify(); };
        transport->onDisconnected = [&](const std::string&) { resultWaiter.notify(); };
        transport->start();
    };
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto clientTls = tls_test::createClientTlsConfig(*certs_, /*verifyPeer=*/true);
    auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(
        *io_, "127.0.0.1", std::to_string(server_->port()), clientTls);
    client->start();

    EXPECT_TRUE(resultWaiter.wait_for(std::chrono::milliseconds(3000)));
    EXPECT_FALSE(serverConnected)
        << "server handshake must fail when the client presents no certificate";

    if (serverTransport) {
        serverTransport->onConnected = nullptr;
        serverTransport->onError = nullptr;
        serverTransport->onDisconnected = nullptr;
    }
    client->stop();
    server_->stop();
}

// A client that verifies the server certificate against an unrelated CA must
// reject the handshake: the server cert does not chain to that CA.
TEST_F(TlsMutualTest, ClientRejectsServerCertSignedByUnknownCa) {
    // Server uses the fixture's certificates / CA.
    auto serverTls = tls_test::createServerTlsConfig(*certs_, /*verifyPeer=*/false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, serverTls);

    server_->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
    };
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // A second, independent set of certificates: its CA never signed the
    // server's certificate.
    tls_test::TestCertificates foreignCerts;
    tlcfi::tcp::TlsConfig clientTls(
        true,                          // enabled
        "",                            // no client cert
        "",                            // no client key
        foreignCerts.caCertFile(),     // wrong CA
        true);                         // verifyPeer

    auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(
        *io_, "127.0.0.1", std::to_string(server_->port()), clientTls);

    bool connected = false;
    test_helpers::AsyncWaiter waiter;
    client->onConnected = [&]() {
        connected = true;
        waiter.notify();
    };
    client->onError = [&](const std::string&) { waiter.notify(); };
    client->onDisconnected = [&](const std::string&) { waiter.notify(); };

    client->start();

    EXPECT_TRUE(waiter.wait_for(std::chrono::milliseconds(3000)));
    EXPECT_FALSE(connected)
        << "handshake must fail when the server cert does not chain to the client's CA";

    client->onConnected = nullptr;
    client->onError = nullptr;
    client->onDisconnected = nullptr;
    client->stop();
    server_->stop();
}

// Baseline: with matching CA and a server that does not demand a client cert,
// the handshake succeeds. This guards the negative tests above against
// becoming false positives (e.g. a connect that fails for an unrelated reason).
TEST_F(TlsMutualTest, ClientWithMatchingCaCompletesHandshake) {
    auto serverTls = tls_test::createServerTlsConfig(*certs_, /*verifyPeer=*/false);
    server_ = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0, serverTls);

    server_->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
        transport->start();
    };
    server_->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto clientTls = tls_test::createClientTlsConfig(*certs_, /*verifyPeer=*/true);
    auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(
        *io_, "127.0.0.1", std::to_string(server_->port()), clientTls);

    bool connected = false;
    test_helpers::AsyncWaiter waiter;
    client->onConnected = [&]() {
        connected = true;
        waiter.notify();
    };
    client->onError = [&](const std::string&) { waiter.notify(); };

    client->start();

    EXPECT_TRUE(waiter.wait_for(std::chrono::milliseconds(3000)));
    EXPECT_TRUE(connected) << "a trusted server cert must complete the handshake";

    client->onConnected = nullptr;
    client->onError = nullptr;
    client->onDisconnected = nullptr;
    client->stop();
    server_->stop();
}
