#include "fi/FiAppHost.hpp"
#include "tcp/TlsConfig.hpp"
#include "util/Logging.hpp"

#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#endif

// Global flag for signal handling
std::atomic<bool> g_running{true};

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down...\n";
    g_running.store(false);
}

/**
 * @brief Example TLC-FI Consumer Monitor Application
 * 
 * This example demonstrates how to use FiConsumerApp to create a simple
 * read-only monitoring application that connects to TLC-FI Facilities and
 * subscribes to all object state updates.
 * 
 * The consumer app:
 * - Connects to the Facilities server
 * - Registers as a Monitor application (read-only, no control)
 * - Reads meta data to discover available objects
 * - Subscribes to all discovered objects (SignalGroups, Outputs, Detectors, Inputs)
 * - Prints all state updates to console
 * - Runs until interrupted (Ctrl+C)
 * 
 * Usage:
 *   consumer_app [host] [port] [username] [password] [uri] [--tls] [--ca-file <file>] [--no-verify]
 * 
 * Defaults:
 *   host: localhost
 *   port: 11501
 *   username: Consumer1
 *   password: Password1
 *   uri: app://Consumer1
 *   TLS: disabled
 * 
 * TLS Options:
 *   --tls              Enable TLS encryption
 *   --ca-file <file>   Path to CA certificate file (for server verification)
 *   --no-verify        Disable peer certificate verification (not recommended)
 */
int main(int argc, char* argv[]) {
    // Flush output immediately to ensure messages appear even if program crashes
    std::cout.flush();
    std::cerr.flush();
    
    try {
        // Parse command line arguments
        std::string host = "localhost";
        std::string port = "11501";
        std::string username = "Consumer1";
        std::string password = "Password1";
        std::string uri = "app://Consumer1";
        
        // TLS configuration
        bool useTls = false;
        std::string caFile;
        bool verifyPeer = true;
        
        // Parse positional arguments
        int argIdx = 1;
        if (argc > argIdx) {
            host = argv[argIdx++];
        }
        if (argc > argIdx) {
            port = argv[argIdx++];
        }
        if (argc > argIdx) {
            username = argv[argIdx++];
        }
        if (argc > argIdx) {
            password = argv[argIdx++];
        }
        if (argc > argIdx) {
            uri = argv[argIdx++];
        }
        
        // Parse TLS options
        while (argIdx < argc) {
            std::string arg = argv[argIdx];
            if (arg == "--tls") {
                useTls = true;
                argIdx++;
            } else if (arg == "--ca-file" && argIdx + 1 < argc) {
                caFile = argv[++argIdx];
                argIdx++;
            } else if (arg == "--no-verify") {
                verifyPeer = false;
                argIdx++;
            } else {
                std::cerr << "Unknown option: " << arg << "\n";
                std::cerr << "Usage: consumer_app [host] [port] [username] [password] [uri] [--tls] [--ca-file <file>] [--no-verify]\n";
                std::cerr.flush();
                return 1;
            }
        }
        
        // Create TLS configuration
        tlcfi::tcp::TlsConfig tlsConfig;
        if (useTls) {
            tlsConfig = tlcfi::tcp::TlsConfig(true, "", "", caFile, verifyPeer);
        }
    
        // Print initial banner
        std::cout << "=== TLC-FI Consumer Monitor Example ===\n";
        std::cout << "Connecting to " << host << ":" << port << "\n";
        std::cout << "Username: " << username << "\n";
        std::cout << "URI: " << uri << "\n";
        if (useTls) {
            std::cout << "TLS: Enabled";
            if (!caFile.empty()) {
                std::cout << " (CA: " << caFile << ")";
            }
            std::cout << " (verify: " << (verifyPeer ? "yes" : "no") << ")\n";
        } else {
            std::cout << "TLS: Disabled\n";
        }
        std::cout << "\n";
        std::cout.flush();
        
        // Set up signal handler for graceful shutdown
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);
        
        // Create the consumer application
        tlcfi::fi::FiConsumerApp app(host, port, username, password, uri, tlsConfig);
        
        // Configure the application host
        tlcfi::fi::FiAppHost::Config hostConfig;
        hostConfig.connectTimeout = std::chrono::milliseconds(5000);
        hostConfig.operationTimeout = std::chrono::milliseconds(5000);
        
        // Create the application host (handles retry loop and logging)
        tlcfi::fi::FiAppHost appHost(app, hostConfig);
        
        appHost.onLog = tlcfi::makeTimestampedLogger();
        
        // Run the application with automatic retry and reconnection
        appHost.run(g_running);
        
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "\nFATAL ERROR: Unhandled exception: " << ex.what() << "\n";
        std::cerr.flush();
        return 1;
    }
    catch (...) {
        std::cerr << "\nFATAL ERROR: Unknown exception occurred\n";
        std::cerr.flush();
        return 1;
    }
}

