#include "tls_test_certs.hpp"

#include <cstdlib>
#include <ctime>
#include <random>
#include <sstream>
#include <system_error>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace tls_test {

namespace {
    std::string getTempDir() {
#ifdef _WIN32
        char tempPath[MAX_PATH];
        if (GetTempPathA(MAX_PATH, tempPath) == 0) {
            return std::string(".");
        }
        return std::string(tempPath);
#else
        const char* tmpdir = std::getenv("TMPDIR");
        if (tmpdir) return std::string(tmpdir);
        tmpdir = std::getenv("TMP");
        if (tmpdir) return std::string(tmpdir);
        tmpdir = std::getenv("TEMP");
        if (tmpdir) return std::string(tmpdir);
        return std::string("/tmp");
#endif
    }

    std::string generateUniqueFilename(const std::string& prefix, const std::string& suffix) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(10000, 99999);
        
        std::ostringstream oss;
        oss << prefix << "_" << dis(gen) << suffix;
        return oss.str();
    }

    bool fileExists(const std::string& path) {
        std::ifstream file(path);
        return file.good();
    }

    std::string getOpenSSLCommand() {
        // Try common openssl command locations
        const char* openssl = std::getenv("OPENSSL");
        if (openssl && fileExists(openssl)) {
            return std::string(openssl);
        }
        
        // Try system PATH
        if (system("openssl version >nul 2>&1") == 0 || system("openssl version >/dev/null 2>&1") == 0) {
            return std::string("openssl");
        }
        
        return std::string();
    }
}

TestCertificates::TestCertificates(const std::string& baseDir)
    : baseDir_(baseDir.empty() ? getTempDir() : baseDir)
    , cleanupOnDestroy_(true) {
    
    // Ensure base directory exists
    try {
        std::filesystem::create_directories(baseDir_);
    } catch (const std::exception&) {
        throw std::runtime_error("Failed to create certificate directory: " + baseDir_);
    }

    // Generate unique filenames
    std::string prefix = "tls_test_" + std::to_string(std::time(nullptr));
    serverCertFile_ = (std::filesystem::path(baseDir_) / generateUniqueFilename(prefix + "_server", ".crt")).string();
    serverKeyFile_ = (std::filesystem::path(baseDir_) / generateUniqueFilename(prefix + "_server", ".key")).string();
    caCertFile_ = (std::filesystem::path(baseDir_) / generateUniqueFilename(prefix + "_ca", ".crt")).string();
    clientCertFile_ = (std::filesystem::path(baseDir_) / generateUniqueFilename(prefix + "_client", ".crt")).string();
    clientKeyFile_ = (std::filesystem::path(baseDir_) / generateUniqueFilename(prefix + "_client", ".key")).string();

    generateCertificates();
}

TestCertificates::~TestCertificates() {
    if (cleanupOnDestroy_) {
        cleanup();
    }
}

void TestCertificates::generateCertificates() {
    std::string openssl = getOpenSSLCommand();
    if (openssl.empty()) {
        // Fallback: create minimal PEM files for testing (will fail actual TLS but allows compilation)
        // In a real scenario, tests should require OpenSSL to be available
        std::ofstream caFile(caCertFile_);
        caFile << "-----BEGIN CERTIFICATE-----\n";
        caFile << "TEST CA CERTIFICATE (INVALID - FOR TESTING ONLY)\n";
        caFile << "-----END CERTIFICATE-----\n";
        
        std::ofstream serverCert(serverCertFile_);
        serverCert << "-----BEGIN CERTIFICATE-----\n";
        serverCert << "TEST SERVER CERTIFICATE (INVALID - FOR TESTING ONLY)\n";
        serverCert << "-----END CERTIFICATE-----\n";
        
        std::ofstream serverKey(serverKeyFile_);
        serverKey << "-----BEGIN PRIVATE KEY-----\n";
        serverKey << "TEST SERVER KEY (INVALID - FOR TESTING ONLY)\n";
        serverKey << "-----END PRIVATE KEY-----\n";
        
        std::ofstream clientCert(clientCertFile_);
        clientCert << "-----BEGIN CERTIFICATE-----\n";
        clientCert << "TEST CLIENT CERTIFICATE (INVALID - FOR TESTING ONLY)\n";
        clientCert << "-----END CERTIFICATE-----\n";
        
        std::ofstream clientKey(clientKeyFile_);
        clientKey << "-----BEGIN PRIVATE KEY-----\n";
        clientKey << "TEST CLIENT KEY (INVALID - FOR TESTING ONLY)\n";
        clientKey << "-----END PRIVATE KEY-----\n";
        
        return;
    }

    // Generate CA key and certificate
    std::string caKeyFile = caCertFile_ + ".key";
    std::string cmd = openssl + " req -x509 -newkey rsa:2048 -nodes -keyout \"" + caKeyFile +
                      "\" -out \"" + caCertFile_ + "\" -days 1 -subj \"/CN=test-ca\" -batch";
    if (system(cmd.c_str()) != 0) {
        throw std::runtime_error("Failed to generate CA certificate");
    }

    // Generate server key
    std::string serverKeyGen = openssl + " genrsa -out \"" + serverKeyFile_ + "\" 2048";
    if (system(serverKeyGen.c_str()) != 0) {
        cleanup();
        throw std::runtime_error("Failed to generate server key");
    }

    // Generate server certificate request
    std::string serverCsr = serverCertFile_ + ".csr";
    std::string serverReq = openssl + " req -new -key \"" + serverKeyFile_ + "\" -out \"" + serverCsr +
                            "\" -subj \"/CN=test-server\" -batch";
    if (system(serverReq.c_str()) != 0) {
        cleanup();
        throw std::runtime_error("Failed to generate server certificate request");
    }

    // Write an X.509 v3 extension file giving the server cert a Subject
    // Alternative Name. The client now verifies that the host it dialed
    // appears in the cert's SAN, so the loopback addresses the tests connect
    // to (127.0.0.1 / localhost) must be present here or handshakes fail.
    std::string serverExt = serverCertFile_ + ".ext";
    {
        std::ofstream ext(serverExt);
        ext << "[v3_ext]\n";
        ext << "basicConstraints=CA:FALSE\n";
        ext << "subjectAltName=DNS:localhost,DNS:test-server,IP:127.0.0.1\n";
    }

    // Sign server certificate with CA, applying the SAN extension above.
    std::string signServer = openssl + " x509 -req -in \"" + serverCsr + "\" -CA \"" + caCertFile_ +
                             "\" -CAkey \"" + caKeyFile + "\" -CAcreateserial -out \"" + serverCertFile_ +
                             "\" -days 1 -extfile \"" + serverExt + "\" -extensions v3_ext";
    if (system(signServer.c_str()) != 0) {
        std::filesystem::remove(serverExt);
        cleanup();
        throw std::runtime_error("Failed to sign server certificate");
    }
    std::filesystem::remove(serverExt);

    // Generate client key
    std::string clientKeyGen = openssl + " genrsa -out \"" + clientKeyFile_ + "\" 2048";
    if (system(clientKeyGen.c_str()) != 0) {
        cleanup();
        throw std::runtime_error("Failed to generate client key");
    }

    // Generate client certificate request
    std::string clientCsr = clientCertFile_ + ".csr";
    std::string clientReq = openssl + " req -new -key \"" + clientKeyFile_ + "\" -out \"" + clientCsr +
                             "\" -subj \"/CN=test-client\" -batch";
    if (system(clientReq.c_str()) != 0) {
        cleanup();
        throw std::runtime_error("Failed to generate client certificate request");
    }

    // Sign client certificate with CA
    std::string signClient = openssl + " x509 -req -in \"" + clientCsr + "\" -CA \"" + caCertFile_ +
                             "\" -CAkey \"" + caKeyFile + "\" -CAcreateserial -out \"" + clientCertFile_ +
                             "\" -days 1";
    if (system(signClient.c_str()) != 0) {
        cleanup();
        throw std::runtime_error("Failed to sign client certificate");
    }

    // Clean up temporary files
    std::filesystem::remove(serverCsr);
    std::filesystem::remove(clientCsr);
    std::filesystem::remove(caKeyFile);
    std::filesystem::remove(caCertFile_ + ".srl");
}

void TestCertificates::cleanup() {
    try {
        if (!serverCertFile_.empty()) std::filesystem::remove(serverCertFile_);
        if (!serverKeyFile_.empty()) std::filesystem::remove(serverKeyFile_);
        if (!caCertFile_.empty()) std::filesystem::remove(caCertFile_);
        if (!clientCertFile_.empty()) std::filesystem::remove(clientCertFile_);
        if (!clientKeyFile_.empty()) std::filesystem::remove(clientKeyFile_);
    } catch (...) {
        // Ignore cleanup errors
    }
}

bool generateSelfSignedCert(const std::string& certFile, const std::string& keyFile,
                            const std::string& subject) {
    std::string openssl = getOpenSSLCommand();
    if (openssl.empty()) {
        return false;
    }

    std::string cmd = openssl + " req -x509 -newkey rsa:2048 -nodes -keyout \"" + keyFile +
                      "\" -out \"" + certFile + "\" -days 1 -subj \"" + subject + "\" -batch";
    return system(cmd.c_str()) == 0;
}

bool generateCACert(const std::string& certFile, const std::string& keyFile,
                     const std::string& subject) {
    return generateSelfSignedCert(certFile, keyFile, subject);
}

} // namespace tls_test

