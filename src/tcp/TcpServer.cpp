#include "TcpServer.hpp"
#include "TlsHardening.hpp"

#include <asio/error.hpp>
#include <asio/post.hpp>
#include <asio/ssl.hpp>
#include <openssl/ssl.h>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace tlcfi::tcp {

TcpServer::TcpServer(asio::io_context& io, unsigned short port, const TlsConfig& tlsConfig,
	                 std::size_t maxConnections)
	: io_(io), acceptor_(io),
	  maxConnections_(maxConnections == 0 ? kDefaultMaxConnections : maxConnections) {

	const asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), port);

	// Windows: the kernel's ephemeral-port picker can race with Hyper-V/WSL/WinNAT
	// port-exclusion ranges (see `netsh interface ipv4 show excludedportrange`),
	// causing bind(0) to return WSAEACCES on a port that's actually reserved.
	// Retry a few times — the OS picks a different port on each attempt. Only
	// retry for ephemeral binds; for a configured port, WSAEACCES means "port is
	// reserved or held", which is operator-visible and must surface immediately.
	constexpr int kBindRetries = 5;
	std::error_code ec;
	for (int attempt = 0;; ++attempt) {
		acceptor_.open(endpoint.protocol(), ec);
		if (ec) {
			throw std::system_error(ec, "TcpServer acceptor open failed");
		}
		acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
		if (ec) {
			throw std::system_error(ec, "TcpServer set_option(reuse_address) failed");
		}

		acceptor_.bind(endpoint, ec);
		if (!ec) break;

		const bool isEphemeral = (port == 0);
		const bool isAccessDenied = (ec == asio::error::access_denied);
		if (!isEphemeral || !isAccessDenied || attempt >= kBindRetries - 1) {
			throw std::system_error(ec, "TcpServer bind failed");
		}

		// Windows leaves a failed-bind acceptor unusable: close and try again.
		std::error_code ignore;
		acceptor_.close(ignore);
	}

	acceptor_.listen(asio::socket_base::max_listen_connections, ec);
	if (ec) {
		throw std::system_error(ec, "TcpServer listen failed");
	}

	if (tlsConfig.enabled) {
		sslContext_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tls_server);

		// Apply the shared TLS hardening policy (min version TLS 1.2 + cipher suites).
		applyTlsHardening(*sslContext_);

		// Load certificate and private key
		if (tlsConfig.certFile.empty() || tlsConfig.keyFile.empty()) {
			throw std::runtime_error("TLS enabled but certificate or key file not specified");
		}
		
		sslContext_->use_certificate_file(tlsConfig.certFile, asio::ssl::context::pem);
		sslContext_->use_private_key_file(tlsConfig.keyFile, asio::ssl::context::pem);
		
		// Load CA certificate if provided
		if (!tlsConfig.caFile.empty()) {
			sslContext_->load_verify_file(tlsConfig.caFile);
		}
		
		// Set verify mode
		if (tlsConfig.verifyPeer) {
			sslContext_->set_verify_mode(asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert);
		} else {
			sslContext_->set_verify_mode(asio::ssl::verify_none);
			// Explicitly disable client certificate requests when verifyPeer is false
			// This prevents OpenSSL from requesting client certificates during handshake
			SSL_CTX_set_verify(sslContext_->native_handle(), SSL_VERIFY_NONE, nullptr);
			// Clear any client CA list to prevent certificate requests
			SSL_CTX_set_client_CA_list(sslContext_->native_handle(), nullptr);
		}
	}
}

void TcpServer::start() {
	stopping_ = false;
	doAccept();
}

void TcpServer::stop() {
	// Idempotent: only the first call performs the shutdown work.
	if (stopping_.exchange(true)) return;

	// asio acceptors are not thread-safe, so the acceptor must be closed on
	// the io thread rather than on the caller's thread, where it would race
	// an in-flight async_accept. doAccept() holds a shared_ptr to this server
	// for the duration of each accept, so a pending accept keeps the server
	// alive; capturing `self` here likewise keeps it alive until the close
	// runs. weak_from_this() is null only if stop() is somehow called with no
	// shared owner — in that case ~TcpServer closes the acceptor instead.
	if (auto self = weak_from_this().lock()) {
		asio::post(io_, [self]() {
			std::error_code ec;
			self->acceptor_.close(ec);
		});
	}

	stopTransports();
}

void TcpServer::stopTransports() {
	// Collect all active transports and clear the set. Detach each transport's
	// onClosed hook first so a close racing with shutdown cannot call back into
	// a half-destroyed server.
	std::vector<std::shared_ptr<TcpServerTransport>> transportsToStop;
	{
		std::lock_guard<std::mutex> lock(transportsMutex_);
		for (const auto& transport : activeTransports_) {
			transport->clearOnClosed();
			transportsToStop.push_back(transport);
		}
		activeTransports_.clear();
	}

	// Stop all transports (without holding the lock to avoid deadlocks)
	for (auto& transport : transportsToStop) {
		transport->stop();
	}
}

void TcpServer::doAccept() {
	auto socket = std::make_shared<asio::ip::tcp::socket>(io_);
	// Capture a shared_ptr to this server: while an async_accept is in flight
	// the server cannot be destroyed, so `this` is always valid in the handler
	// and the acceptor is never destroyed mid-operation.
	auto self = shared_from_this();
	acceptor_.async_accept(*socket, [this, self, socket](std::error_code ec){
		if (ec) {
			// A failed accept operation consumes the outstanding accept. Keep
			// serving after transient network errors; shutdown closes the
			// acceptor and reports operation_aborted, which must not be re-armed.
			if (!stopping_ && ec != asio::error::operation_aborted && acceptor_.is_open()) {
				doAccept();
			}
			return;
		}

		// Enforce the connection limit: refuse excess connections rather than
		// letting a peer exhaust file descriptors and memory (slowloris /
		// connection-flood DoS).
		{
			std::lock_guard<std::mutex> lock(transportsMutex_);
			if (activeTransports_.size() >= maxConnections_) {
				std::error_code closeEc;
				socket->close(closeEc);
				if (!stopping_) doAccept();
				return;
			}
		}

		// Create transport with optional SSL context
		std::shared_ptr<TcpServerTransport> transport;
		if (sslContext_) {
			transport = std::make_shared<TcpServerTransport>(std::move(*socket), *sslContext_);
		} else {
			transport = std::make_shared<TcpServerTransport>(std::move(*socket));
		}

		// Drop our reference once the connection closes, so closed connections
		// do not accumulate in activeTransports_ for the life of the server.
		std::weak_ptr<TcpServerTransport> weakTransport = transport;
		transport->setOnClosed([this, weakTransport]() {
			if (auto t = weakTransport.lock()) {
				std::lock_guard<std::mutex> lock(transportsMutex_);
				activeTransports_.erase(t);
			}
		});

		// Track the transport
		{
			std::lock_guard<std::mutex> lock(transportsMutex_);
			activeTransports_.insert(transport);
		}

		if (onAccept) onAccept(transport);
		if (!stopping_) doAccept();
	});
}

TcpServer::~TcpServer() {
	stopping_ = true;
	// Reaching the destructor means no shared_ptr owns this server, so no
	// async_accept handler is pending (a pending one holds a shared_ptr that
	// keeps the server alive). The acceptor therefore has no operation in
	// flight and can be closed directly on this thread.
	std::error_code ec;
	acceptor_.close(ec);
	stopTransports();
}

} // namespace tlcfi::tcp


