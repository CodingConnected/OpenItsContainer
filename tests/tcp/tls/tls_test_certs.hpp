#pragma once

#include <string>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace tls_test {

/**
 * @brief Generates self-signed certificates for TLS testing.
 * 
 * This helper class creates temporary certificate files for use in tests.
 * Certificates are cleaned up when the object is destroyed.
 */
class TestCertificates {
public:
    /**
     * @brief Constructs test certificates and generates files.
     * 
     * @param baseDir Directory where certificate files will be created (default: system temp)
     * @throws std::runtime_error if certificate generation fails
     */
    explicit TestCertificates(const std::string& baseDir = "");

    /**
     * @brief Destructor - cleans up temporary certificate files.
     */
    ~TestCertificates();

    // Non-copyable, movable
    TestCertificates(const TestCertificates&) = delete;
    TestCertificates& operator=(const TestCertificates&) = delete;
    TestCertificates(TestCertificates&&) = default;
    TestCertificates& operator=(TestCertificates&&) = default;

    /**
     * @brief Returns path to server certificate file.
     */
    const std::string& serverCertFile() const { return serverCertFile_; }

    /**
     * @brief Returns path to server private key file.
     */
    const std::string& serverKeyFile() const { return serverKeyFile_; }

    /**
     * @brief Returns path to CA certificate file.
     */
    const std::string& caCertFile() const { return caCertFile_; }

    /**
     * @brief Returns path to client certificate file (for mutual TLS).
     */
    const std::string& clientCertFile() const { return clientCertFile_; }

    /**
     * @brief Returns path to client private key file (for mutual TLS).
     */
    const std::string& clientKeyFile() const { return clientKeyFile_; }

private:
    void generateCertificates();
    void cleanup();
    
    std::string baseDir_;
    std::string serverCertFile_;
    std::string serverKeyFile_;
    std::string caCertFile_;
    std::string clientCertFile_;
    std::string clientKeyFile_;
    bool cleanupOnDestroy_;
};

/**
 * @brief Generates a simple self-signed certificate using OpenSSL command-line.
 * 
 * This is a fallback method that uses the openssl command if available.
 * For production tests, consider using OpenSSL APIs directly.
 */
bool generateSelfSignedCert(const std::string& certFile, const std::string& keyFile,
                            const std::string& subject = "/CN=test-server");

/**
 * @brief Generates a CA certificate.
 */
bool generateCACert(const std::string& certFile, const std::string& keyFile,
                     const std::string& subject = "/CN=test-ca");

} // namespace tls_test



