#pragma once

// --- Third-Party Includes ---
#include <asio/ssl.hpp>
#include <openssl/ssl.h>

// --- Standard Library Includes ---
#include <stdexcept>

namespace tlcfi::tcp {

/**
 * @brief Applies the project's TLS hardening policy to an SSL context.
 *
 * Centralizes the security policy shared by the client and server transports
 * so it cannot drift between the two:
 *  - applies standard workarounds and disables SSLv2/SSLv3;
 *  - floors the negotiated protocol at TLS 1.2 (TLS 1.3 is used automatically
 *    when both peers and the linked OpenSSL support it);
 *  - restricts the TLS 1.2 cipher list to forward-secret (ECDHE) AEAD suites
 *    only — no CBC, no static-RSA key exchange, no SHA-1. TLS 1.3 cipher
 *    suites are negotiated separately and left at OpenSSL's defaults, which
 *    are all AEAD.
 *
 * The context must already have been created with a version-flexible method
 * (asio::ssl::context::tls_client or tls_server) so that the floor, rather
 * than a hard-locked version, governs negotiation.
 *
 * @param ctx The SSL context to harden.
 * @throws std::runtime_error if the minimum version or cipher list cannot be set.
 */
inline void applyTlsHardening(asio::ssl::context& ctx) {
	// Forward-secret AEAD suites only. ECDHE-RSA-* covers RSA server
	// certificates; ECDHE-ECDSA-* covers ECDSA ones. Order expresses
	// preference (AES-256/ChaCha20 ahead of AES-128).
	static constexpr char kCipherList[] =
		"ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
		"ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
		"ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256";

	ctx.set_options(
		asio::ssl::context::default_workarounds |
		asio::ssl::context::no_sslv2 |
		asio::ssl::context::no_sslv3 |
		asio::ssl::context::single_dh_use);

	SSL_CTX* native = ctx.native_handle();

	if (SSL_CTX_set_min_proto_version(native, TLS1_2_VERSION) != 1) {
		throw std::runtime_error("TLS: failed to set minimum protocol version (TLS 1.2)");
	}

	if (SSL_CTX_set_cipher_list(native, kCipherList) != 1) {
		throw std::runtime_error("TLS: failed to apply cipher list");
	}
}

} // namespace tlcfi::tcp
