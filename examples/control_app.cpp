#include "fi/FiControllerApp.hpp"
#include "fi/FiAppHost.hpp"
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
 * @brief Example TLC-FI Controller Application
 *
 * This example demonstrates how to use FiControllerApp with FiAppHost to create
 * a control application that connects to TLC-FI Facilities and manages the
 * control state machine.
 *
 * The controller app:
 * - Connects to the Facilities server
 * - Registers as a Control application
 * - Subscribes to the Session object for control state monitoring
 * - Automatically handles the control state machine:
 *   - NotConfigured: reads meta data and subscribes to objects
 *   - Offline: requests ReadyToControl
 *   - StartControl: acknowledges with InControl
 * - Prints all state updates to console
 * - Runs until interrupted (Ctrl+C)
 *
 * Usage:
 *   control_app [host] [port] [username] [password] [uri]
 *
 * Defaults:
 *   host: localhost
 *   port: 11501
 *   username: Controller1
 *   password: Password1
 *   uri: app://Controller1
 */
int main(int argc, char* argv[]) {
    // Flush output immediately to ensure messages appear even if program crashes
    std::cout.flush();
    std::cerr.flush();

    try {
        // Parse command line arguments
        std::string host = "localhost";
        std::string port = "11501";
        std::string username = "Controller1";
        std::string password = "Password1";
        std::string uri = "app://Controller1";

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

        // Check for unknown options
        while (argIdx < argc) {
            std::string arg = argv[argIdx];
            std::cerr << "Unknown option: " << arg << "\n";
            std::cerr << "Usage: control_app [host] [port] [username] [password] [uri]\n";
            std::cerr.flush();
            return 1;
        }

        // Print initial banner
        std::cout << "=== TLC-FI Controller Application Example ===\n";
        std::cout << "Connecting to " << host << ":" << port << "\n";
        std::cout << "Username: " << username << "\n";
        std::cout << "URI: " << uri << "\n";
        std::cout << "\n";
        std::cout.flush();

        // Set up signal handler for graceful shutdown
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);

        // Create the controller application (no TLS)
        tlcfi::fi::FiControllerApp app(host, port, username, password, uri);

        // Set up control state callbacks
        app.onControlInControl = [](const std::string& intersectionId) {
            std::cout << "\033[38;5;46m*** IN CONTROL of intersection: " << intersectionId << " ***\033[0m\n";
            std::cout.flush();
        };

        app.onControlEndControl = [](const std::string& intersectionId) {
            std::cout << "\033[38;5;196m*** END CONTROL of intersection: " << intersectionId << " ***\033[0m\n";
            std::cout.flush();
        };

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
