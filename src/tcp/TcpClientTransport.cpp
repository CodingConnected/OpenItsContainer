#include "TcpClientTransport.hpp"
#include "TlsHardening.hpp"

#include <sstream>
#include <chrono>
#include <stdexcept>
#include <string>
#include <ctime>

#include <asio/ip/tcp.hpp>
#include <asio/ip/address.hpp>
#include <asio/post.hpp>
#include <asio/connect.hpp>
#include <asio/ssl.hpp>
#include <asio/write.hpp>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/asn1.h>
#include <openssl/bio.h>

namespace tlcfi::tcp {

TcpClientTransport::TcpClientTransport(asio::io_context& io, std::string host, std::string port,
                                       const TlsConfig& tlsConfig, std::size_t maxPendingWriteBytes)
	: io_(io)
	, resolver_(io)
	, socket_(io)
	, host_(std::move(host))
	, port_(std::move(port))
	, maxPendingWriteBytes_(maxPendingWriteBytes) {
	if (maxPendingWriteBytes_ == 0) {
		throw std::invalid_argument("maxPendingWriteBytes must be greater than zero");
	}
	verifyPeer_ = tlsConfig.enabled && tlsConfig.verifyPeer;
	expectedPeerName_ = tlsConfig.expectedPeerName;
	if (tlsConfig.enabled) {
		sslContext_ = std::make_unique<asio::ssl::context>(asio::ssl::context::tls_client);

		// Apply the shared TLS hardening policy (min version TLS 1.2 + cipher suites).
		applyTlsHardening(*sslContext_);

		// Set verify mode first, before loading CA certificates
		// This ensures verification is properly configured
		if (tlsConfig.verifyPeer) {
			// Load CA certificate if provided
			if (!tlsConfig.caFile.empty()) {
				sslContext_->load_verify_file(tlsConfig.caFile);
			} else {
				// Use default verify paths (system CA certificates)
				sslContext_->set_default_verify_paths();
			}
		} else {
			// When verifyPeer is false, don't load CA certificates
			// This prevents OpenSSL from doing any verification
		}
		
		// Set verify mode
		if (tlsConfig.verifyPeer) {
			sslContext_->set_verify_mode(asio::ssl::verify_peer);
			
			// Set up verification callback for detailed error reporting
			sslContext_->set_verify_callback([this](bool preverified, asio::ssl::verify_context& ctx) {
				// Get the certificate being verified
				X509_STORE_CTX* cts = ctx.native_handle();
				int verifyError = X509_STORE_CTX_get_error(cts);
				
				// Store verification error for detailed reporting in handshake handler
				if (verifyError != X509_V_OK) {
					lastVerifyError_ = verifyError;
					
					X509* cert = X509_STORE_CTX_get_current_cert(cts);
					if (cert) {
						char subject[256];
						X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject));
						lastVerifyCertSubject_ = subject;
					}
				}
				
				return preverified;
			});
		} else {
			sslContext_->set_verify_mode(asio::ssl::verify_none);
		}
		
		// Create SSL stream (will be initialized after socket connection)
		sslStream_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(socket_, *sslContext_);
	}
}

void TcpClientTransport::start() {
	stopping_ = false;
	connected_ = false;
	{
		std::lock_guard<std::mutex> lock(writeMutex_);
		writeQueue_.clear();
		pendingWriteBytes_ = 0;
		writeInProgress_ = false;
		writeQueueOverflowed_ = false;
	}
	doConnect();
}

void TcpClientTransport::stop() {
	if (stopping_.exchange(true)) return;

	connected_ = false;

	{
		std::lock_guard<std::mutex> lock(writeMutex_);
		writeQueue_.clear();
		pendingWriteBytes_ = 0;
		writeInProgress_ = false;
	}

	// asio sockets are not thread-safe: cancelling/closing the socket on the
	// caller's thread races an in-flight async_read_some / async_write on the
	// io thread. Post the cancel+close onto the socket's executor so all
	// socket access stays on the io thread (mirrors TcpServerTransport::stop()).
	auto self = shared_from_this();
	asio::post(socket_.get_executor(), [self]() {
		std::error_code ec;
		self->socket_.cancel(ec);
		if (self->socket_.is_open()) {
			self->socket_.close(ec);
		}
	});

	if (onDisconnected) onDisconnected("stop");
}

void TcpClientTransport::asyncSend(const std::uint8_t* data, std::size_t size) {
	bool startWrite = false;
	bool queueOverflow = false;
	{
		std::lock_guard<std::mutex> lock(writeMutex_);
		if (!connected_ || stopping_) {
			throw std::runtime_error("Cannot send: not connected or stopping");
		}
		if (writeQueueOverflowed_) {
			return;
		}
		if (size > maxPendingWriteBytes_ - pendingWriteBytes_) {
			writeQueueOverflowed_ = true;
			queueOverflow = true;
		} else {
			writeQueue_.emplace_back(data, data + size);
			pendingWriteBytes_ += size;
		}
		if (!queueOverflow && !writeInProgress_) {
			writeInProgress_ = true;
			startWrite = true;
		}
	}
	if (queueOverflow) {
		asio::post(io_, [self = shared_from_this()]() {
			if (self->onError) {
				self->onError("write queue limit exceeded");
			}
			self->stop();
		});
		return;
	}
	if (startWrite) {
		asio::post(io_, [self = shared_from_this()](){ self->doWrite(); });
	}
}

void TcpClientTransport::doConnect() {
	auto self = shared_from_this();
		resolver_.async_resolve(host_, port_, [this, self](std::error_code ec, asio::ip::tcp::resolver::results_type results){
		if (ec) {
			if (stopping_) return;
			if (onError) onError("resolve: " + ec.message());
			return;
		}
		asio::async_connect(socket_, results, [this, self](std::error_code ec, const asio::ip::tcp::endpoint& ep){
			if (ec) {
				if (stopping_) return;
				connected_ = false;
				if (onError) onError("connect: " + ec.message());
				return;
			}
			(void)ep;
			
			// Perform TLS handshake if enabled, otherwise start reading
			if (sslStream_) {
				doHandshake(); // this will also call onConnected if successful
			} else {
				connected_ = true;
				if (onConnected) onConnected();
				doRead();
			}
		});
	});
}

void TcpClientTransport::doRead() {
	auto self = shared_from_this();
	
	if (sslStream_) {
		// TLS read
		sslStream_->async_read_some(asio::buffer(readBuf_), [this, self](std::error_code ec, std::size_t n){
			if (stopping_ || !sslStream_) return;  // Check early - sslStream_ may be reset during stop()
			
			if (ec) {
				connected_ = false;
				if (!stopping_) {
					// Check for SSL shutdown errors - these are normal when peer closes gracefully
					if (ec == asio::ssl::error::stream_truncated) {
						// SSL stream was truncated (peer didn't send proper shutdown)
						if (onDisconnected) onDisconnected(ec.message());
					} else if (ec == asio::error::eof) {
						// Peer closed connection - this is normal
						if (onDisconnected) onDisconnected(ec.message());
					} else if (ec.category() == asio::error::get_ssl_category()) {
						// SSL-specific error
						if (onDisconnected) onDisconnected(ec.message());
					} else if (ec != asio::error::operation_aborted) {
						// Other errors
						if (onDisconnected) onDisconnected(ec.message());
					}
					// Connection lost - report via callback, application should handle reconnection
				}
				return;
			}
			if (onBytes) onBytes(readBuf_.data(), n);
			if (!stopping_) doRead();
		});
	} else {
		// Plain TCP read
		socket_.async_read_some(asio::buffer(readBuf_), [this, self](std::error_code ec, std::size_t n){
			if (stopping_) return;  // Check early
			
			if (ec) {
				connected_ = false;
				if (!stopping_) {
					if (onDisconnected) onDisconnected(ec.message());
					// Connection lost - report via callback, application should handle reconnection
				}
				return;
			}
			if (onBytes) onBytes(readBuf_.data(), n);
			if (!stopping_) doRead();
		});
	}
}

void TcpClientTransport::doWrite() {
	// Check if we're stopping before doing anything - prevents accessing queue after stop()
	if (stopping_) {
		return;
	}
	
	std::shared_ptr<std::vector<std::uint8_t>> data = std::make_shared<std::vector<std::uint8_t>>();
	{
		std::lock_guard<std::mutex> lock(writeMutex_);
		// Check again after acquiring lock - stop() might have been called
		if (stopping_ || writeQueue_.empty()) {
			writeInProgress_ = false;
			return;
		}
		// Move and pop immediately to avoid iterator invalidation issues
		*data = std::move(writeQueue_.front());
		writeQueue_.pop_front();
	}
	auto self = shared_from_this();
	
	// Check again after acquiring data - stop() might have been called
	if (stopping_) {
		return;
	}
	
	if (!stopping_ && sslStream_) {
		// TLS write - use sslStream_ directly to avoid dangling pointer if reset during stop()
		asio::async_write(*sslStream_, asio::buffer(*data), [this, self, data](std::error_code ec, std::size_t){
			// Check if we're stopping before accessing members
			if (stopping_ || !sslStream_) return;
			onWriteComplete(ec, data->size());
		});
	} else {
		// Plain TCP write
		asio::async_write(socket_, asio::buffer(*data), [this, self, data](std::error_code ec, std::size_t){
			// Check if we're stopping before accessing members
			if (stopping_) return;
			onWriteComplete(ec, data->size());
		});
	}
}

void TcpClientTransport::onWriteComplete(const std::error_code& error, std::size_t completedBytes) {
	if (stopping_) return;

	if (error) {
		connected_ = false;
		{
			std::lock_guard<std::mutex> lock(writeMutex_);
			writeQueue_.clear();
			pendingWriteBytes_ = 0;
			writeInProgress_ = false;
		}
		if (onError) onError("write: " + error.message());
		return;
	}

	bool hasMore = false;
	{
		std::lock_guard<std::mutex> lock(writeMutex_);
		if (stopping_) {
			writeInProgress_ = false;
			return;
		}
		pendingWriteBytes_ -= completedBytes;
		hasMore = !writeQueue_.empty();
		if (!hasMore) {
			writeInProgress_ = false;
		}
	}
	if (hasMore) {
		doWrite();
	}
}

void TcpClientTransport::doHandshake() {
	auto self = shared_from_this();

	SSL* ssl = sslStream_->native_handle();

	// The identity we expect the server's certificate to carry: an explicit
	// override if configured, otherwise the host we dialed.
	const std::string& peerName = expectedPeerName_.empty() ? host_ : expectedPeerName_;

	// Distinguish an IP literal (e.g. "127.0.0.1") from a DNS name. SNI and
	// hostname matching use different code paths for each.
	std::error_code addrEc;
	asio::ip::make_address(peerName, addrEc);
	const bool isIpLiteral = !addrEc;

	// SNI carries the requested hostname so the server can select the right
	// certificate. It is only valid for DNS names — RFC 6066 forbids IP
	// literals in SNI — so skip it for IP addresses.
	if (!isIpLiteral) {
		SSL_set_tlsext_host_name(ssl, peerName.c_str());
	}

	// Identity verification: when verifying the peer, require that the
	// certificate actually belongs to the host we intended to reach. Without
	// this, any certificate that merely chains to a trusted CA is accepted,
	// leaving the connection open to man-in-the-middle attacks. OpenSSL
	// enforces the match during chain verification and reports a mismatch
	// (X509_V_ERR_HOSTNAME_MISMATCH / IP_ADDRESS_MISMATCH) through the verify
	// callback, which fails the handshake.
	if (verifyPeer_) {
		X509_VERIFY_PARAM* param = SSL_get0_param(ssl);
		X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
		const int rc = isIpLiteral
			? X509_VERIFY_PARAM_set1_ip_asc(param, peerName.c_str())
			: X509_VERIFY_PARAM_set1_host(param, peerName.c_str(), 0);
		if (rc != 1 && onError) {
			onError("TLS: failed to set expected peer identity '" + peerName + "'");
		}
	}

	sslStream_->async_handshake(asio::ssl::stream_base::client,
		[this, self](std::error_code ec) {
			if (ec) {
				if (!stopping_) {
					connected_ = false;
					
					// Build detailed error message
					std::ostringstream errorMsg;
					errorMsg << "TLS handshake failed: " << ec.message() << " (code: " << ec.value() << ")";
					
					// Add verification error details if available
					if (sslStream_) {
						long verifyResult = SSL_get_verify_result(sslStream_->native_handle());
						
						// Use stored verification error from callback if available
						if (lastVerifyError_ != 0) {
							errorMsg << "\n  Verification error: " << X509_verify_cert_error_string(lastVerifyError_);
							if (!lastVerifyCertSubject_.empty()) {
								errorMsg << "\n  Certificate: " << lastVerifyCertSubject_;
							}
						} else if (verifyResult != X509_V_OK) {
							errorMsg << "\n  Verification error: " << X509_verify_cert_error_string(verifyResult);
						}
						
						// Get peer certificate info if available
						X509* peerCert = SSL_get_peer_certificate(sslStream_->native_handle());
						if (peerCert) {
							char subject[256];
							X509_NAME_oneline(X509_get_subject_name(peerCert), subject, sizeof(subject));
							char issuer[256];
							X509_NAME_oneline(X509_get_issuer_name(peerCert), issuer, sizeof(issuer));
							errorMsg << "\n  Server cert subject: " << subject;
							errorMsg << "\n  Server cert issuer: " << issuer;
							
							// Simple expiration check
							ASN1_TIME* notAfter = X509_get_notAfter(peerCert);
							if (notAfter) {
								// Format expiration date
								BIO* bio = BIO_new(BIO_s_mem());
								if (bio && ASN1_TIME_print(bio, notAfter)) {
									char* data = nullptr;
									long len = BIO_get_mem_data(bio, &data);
									if (len > 0) {
										std::string expiryDate(data, len);
										errorMsg << "\n  Certificate expires: " << expiryDate;
										
										// Simple check: compare expiry date string (format: "MMM DD HH:MM:SS YYYY GMT")
										// If year is in the past, it's expired
										time_t now = time(nullptr);
										struct tm* nowTm = gmtime(&now);
										int currentYear = nowTm->tm_year + 1900;
										
										// Extract year from expiry date string (last 4 digits before " GMT")
										size_t gmtPos = expiryDate.find(" GMT");
										if (gmtPos != std::string::npos && gmtPos >= 4) {
											std::string yearStr = expiryDate.substr(gmtPos - 4, 4);
											int expiryYear = std::stoi(yearStr);
											if (expiryYear < currentYear) {
												errorMsg << " [EXPIRED]";
											} else if (expiryYear == currentYear) {
												// Could be expired this year, but simple check shows it's at least this year
												errorMsg << " [Check if expired this year]";
											}
										}
									}
									BIO_free(bio);
								} else if (bio) {
									BIO_free(bio);
								}
							}
							
							X509_free(peerCert);
						} else {
							errorMsg << "\n  No peer certificate received";
						}
					}
					
					if (onError) onError(errorMsg.str());
					if (onDisconnected) onDisconnected("TLS handshake failed");
					// TLS handshake failed - report via callback, application should handle reconnection
				}
				return;
			}
			
			// Handshake successful - connection is ready
			if (!stopping_) {
				connected_ = true;
				if (onConnected) onConnected();
				doRead();
			}
		});
}

std::string TcpClientTransport::name() const {
	return sslStream_ ? "tls-client" : "tcp-client";
}

} // namespace tlcfi::tcp


