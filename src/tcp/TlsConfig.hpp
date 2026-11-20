#pragma once

#include <string>

namespace tlcfi::tcp {

/**
 * @brief Configuration for TLS/SSL encryption.
 * 
 * This structure holds all TLS-related configuration options for both
 * server and client transports. When enabled is false, TLS is not used
 * and plain TCP connections are established.
 */
struct TlsConfig {
	/**
	 * @brief Whether TLS encryption is enabled.
	 * 
	 * When false, all other TLS settings are ignored and plain TCP is used.
	 */
	bool enabled = false;

	/**
	 * @brief Path to the server certificate file (PEM format).
	 * 
	 * Required for server-side TLS. Must be a valid X.509 certificate
	 * in PEM format. For client-side TLS, this is typically not used.
	 */
	std::string certFile;

	/**
	 * @brief Path to the private key file (PEM format).
	 * 
	 * Required for server-side TLS. Must correspond to the certificate
	 * in certFile. For client-side TLS, this is typically not used.
	 */
	std::string keyFile;

	/**
	 * @brief Path to the CA certificate file for peer verification (PEM format).
	 * 
	 * Optional. If provided and verifyPeer is true, this file contains
	 * trusted CA certificates used to verify the peer's certificate.
	 * If empty, system default CA certificates are used.
	 */
	std::string caFile;

	/**
	 * @brief Whether to verify the peer's certificate.
	 * 
	 * When true, the peer's certificate is verified against the CA
	 * certificates. For servers, this verifies client certificates.
	 * For clients, this verifies the server's certificate.
	 * 
	 * @note Setting this to false reduces security but may be needed
	 *        for testing or self-signed certificates.
	 */
	bool verifyPeer = true;

	/**
	 * @brief Optional override for the peer name checked during verification.
	 *
	 * Client-side only. When verifyPeer is true, the client verifies that the
	 * server certificate's identity (Subject Alternative Name, or Common Name
	 * as a fallback) matches the host it dialed. If this field is non-empty, it
	 * is used as the expected identity instead of the connect host — useful when
	 * connecting by IP address to a server whose certificate carries a DNS name.
	 *
	 * Leave empty (the default) to verify against the host passed to the
	 * transport. Ignored when verifyPeer is false.
	 */
	std::string expectedPeerName;

	/**
	 * @brief Constructs a TLS configuration with TLS disabled by default.
	 */
	TlsConfig() = default;

	/**
	 * @brief Constructs a TLS configuration with the specified settings.
	 * 
	 * @param enabled Whether TLS is enabled.
	 * @param certFile Path to certificate file (required if enabled for server).
	 * @param keyFile Path to private key file (required if enabled for server).
	 * @param caFile Path to CA certificate file (optional).
	 * @param verifyPeer Whether to verify peer certificates.
	 * @param expectedPeerName Optional override for the verified peer identity
	 *        (client-side; empty means verify against the dialed host).
	 */
	TlsConfig(bool enabled, std::string certFile, std::string keyFile,
	          std::string caFile = "", bool verifyPeer = true,
	          std::string expectedPeerName = "")
		: enabled(enabled)
		, certFile(std::move(certFile))
		, keyFile(std::move(keyFile))
		, caFile(std::move(caFile))
		, verifyPeer(verifyPeer)
		, expectedPeerName(std::move(expectedPeerName))
	{}
};

} // namespace tlcfi::tcp



