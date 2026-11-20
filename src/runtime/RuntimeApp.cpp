#include "runtime/RuntimeApp.hpp"

#include "fi/FiControllerApp.hpp"
#include "fi/FiAppHost.hpp"
#include "cif/CifBridge.hpp"
#include "cif/CifHost.hpp"
#include "util/Logging.hpp"
#include "util/RollingFileStream.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {

// Container-wide run flag, cleared by the signal handler.
std::atomic<bool> g_running{true};

// Optional JSON-RPC trace log file (enabled via --json-log / jsonLog).
tlcfi::RollingFileStream g_jsonLogFile;

// Optional plain-text application log file (enabled via logFile / --log-file).
tlcfi::RollingFileStream g_appLogFile;

void signalHandler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down...\n";
    g_running.store(false);
}

} // namespace

namespace tlcfi::runtime {

// activeOptions() lives in its own translation unit (RuntimeActiveOptions.cpp)
// so that pulling it does NOT drag runContainer's heavy dependency graph (which
// needs a controller's CIF_* symbols). setActiveOptions is its internal setter,
// called here once configuration is parsed.
void setActiveOptions(const RuntimeOptions* opts);

// Runs every hook registered via registerStartupHook(). Defined in the same
// isolated TU as setActiveOptions/activeOptions so referencing it here does not
// drag any optional subsystem into the runtime. Called once, immediately after
// setActiveOptions, to bring up startup-time subsystems (the PTP transport).
void runStartupHooks();

int runContainer(int argc, char* argv[], const PreInitHook& preInit) {
    std::cout.flush();
    std::cerr.flush();

    // Declared outside the try block so the catch paths can log fatal errors
    // to the file if the logger was successfully created.
    ::tlcfi::fi::LogCallback logger;

    try {
        RuntimeOptions opts;
        // Pre-scan for --tlcfi so the JSON loads before flag overrides are
        // applied (precedence: built-in defaults < tlcfi.json < flags).
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == "--tlcfi") {
                opts.tlcfiConfigPath = argv[i + 1];
            }
        }
        loadTlcfiConfig(opts.tlcfiConfigPath, opts);
        if (!parseArgs(argc, argv, opts)) {
            return 1;
        }

        // Publish the parsed options so lazily-created subsystems (the optional
        // PTP transport) can pull their configuration via activeOptions().
        // opts lives for the whole of runContainer, i.e. the process run.
        setActiveOptions(&opts);

        // Bring up startup-time subsystems now that configuration is live. This
        // is where the optional PTP transport (if a PTP-enabled controller is
        // linked) is constructed eagerly, so its link is up from container
        // startup — a peer can connect at any time, independently of control
        // state, and the link then lives for the process (see PtpManager).
        runStartupHooks();

        // Open the application log file before creating the logger so it is
        // included from the very first log line.
        if (!opts.logFilePath.empty()) {
            std::error_code ec;
            auto parent = std::filesystem::path(opts.logFilePath).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ec);
            }
            const auto logMaxBytes =
                static_cast<std::uintmax_t>(opts.logFileMaxSizeMb) * 1024 * 1024;
            if (!g_appLogFile.open(opts.logFilePath, logMaxBytes, opts.logFileMaxBackups)) {
                std::cerr << "Warning: Failed to open application log file: "
                          << opts.logFilePath << "\n";
            }
        }

        // All output from this point on goes through the logger.
        std::ostream* logFileOut = g_appLogFile.is_open() ? &g_appLogFile : nullptr;
        logger = ::tlcfi::makeTimestampedLogger(
            opts.consoleLogLevel, opts.fileLogLevel, logFileOut);

        using L = ::tlcfi::fi::LogLevel;

        logger(L::Info, "=== TLC-FI Controller Container ===");
        logger(L::Info, "Connecting to " + opts.host + ":" + opts.port);
        logger(L::Info, std::string("Transport: ") + (opts.tls.enabled ? "TLS" : "plain TCP"));
        logger(L::Info, "Username: " + opts.username);
        logger(L::Info, "URI: " + opts.uri);
        if (!opts.cifFileLogDir.empty()) {
            const std::string sysName =
                opts.cifFileLogSystemName.empty() ? opts.username : opts.cifFileLogSystemName;
            logger(L::Info, "CIF file VLOG dir: " + opts.cifFileLogDir);
            logger(L::Info, "CIF file VLOG system: " + sysName);
            if (opts.cifFileLogMaxFiles > 0) {
                logger(L::Info, "CIF file VLOG max files: " + std::to_string(opts.cifFileLogMaxFiles));
            }
        }
        if (g_appLogFile.is_open()) {
            logger(L::Info, "Application log file: " + opts.logFilePath);
        } else if (!opts.logFilePath.empty()) {
            logger(L::Warning, "Failed to open application log file: " + opts.logFilePath);
        }

        // Set up signal handler for graceful shutdown
        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);

        // Create the controller application with the configured transport security.
        ::tlcfi::fi::FiControllerApp app(
            opts.host, opts.port, opts.username, opts.password, opts.uri, opts.tls);

        // Create the CIF bridge (translates TLC-FI <-> CIF arrays)
        cif::CifBridge bridge(app);

        // Bridge id<->index mappings are built automatically by CifHost::init()
        // from the controller's getCifItemName() contract (see cif/CifController.h),
        // then validated against the TLC's reported objects when meta is received.

        // Create the CIF host (runs the control loop calling applicatieprogramma)
        cif::CifHost cifHost(app, bridge);

        const bool fileLogEnabled = !opts.cifFileLogDir.empty();
        const std::string effectiveSystemName =
            opts.cifFileLogSystemName.empty() ? opts.username : opts.cifFileLogSystemName;
        bridge.configureFileLogging(fileLogEnabled, opts.cifFileLogDir, effectiveSystemName,
                                    opts.cifFileLogMaxFiles);

        // Configure CIF_PARM1 persistence. Same effective system name as the
        // file VLOG writer so a deployment's per-system files cluster together
        // by prefix. Empty parmStateDir disables the feature.
        if (!opts.parmStateDir.empty()) {
            const auto sanitized = cif::CifBridge::sanitizeSystemName(effectiveSystemName);
            const auto parmFile = std::filesystem::path(opts.parmStateDir)
                                  / (sanitized + "_cifparm1.json");
            cifHost.setParam1StoreFile(parmFile,
                                       std::chrono::milliseconds(opts.parmStateIntervalMs));
            logger(L::Info, "CIF_PARM1 persistence: " + parmFile.string() +
                   " (interval " + std::to_string(opts.parmStateIntervalMs) + " ms)");
        }

        // Set up JSON-RPC trace logging if requested.
        // The JsonRpcSession is created during connect() (inside appHost.run()),
        // so the trace callback is installed in the onConnected handler.
        if (!opts.jsonLogPath.empty()) {
            std::error_code ec;
            auto parent = std::filesystem::path(opts.jsonLogPath).parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent, ec);
            }
            const auto jsonMaxBytes =
                static_cast<std::uintmax_t>(opts.jsonLogMaxSizeMb) * 1024 * 1024;
            if (!g_jsonLogFile.open(opts.jsonLogPath, jsonMaxBytes, opts.jsonLogMaxBackups)) {
                logger(L::Warning, "Failed to open JSON log file: " + opts.jsonLogPath);
            } else {
                logger(L::Info, "JSON-RPC logging enabled, will log to: " + opts.jsonLogPath);
                logger(L::Info, "(Logging will start after connection is established)");

                app.onConnectedEvent = [&app, logger]() {
                    logger(::tlcfi::fi::LogLevel::Info,
                           "Connection established, starting JSON-RPC logging...");
                    auto rpcSession = app.getJsonRpcSession();
                    if (rpcSession) {
                        rpcSession->onTrace = [](const char* direction, const std::string& text) {
                            if (std::strcmp(direction, "recv") == 0 && g_jsonLogFile.is_open()) {
                                auto now = std::chrono::system_clock::now();
                                auto time = std::chrono::system_clock::to_time_t(now);
                                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now.time_since_epoch()).count() % 1000;

                                std::tm tm_buf;
#ifdef _WIN32
                                localtime_s(&tm_buf, &time);
#else
                                localtime_r(&time, &tm_buf);
#endif
                                std::ostringstream oss;
                                oss << "[" << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
                                    << "." << std::setfill('0') << std::setw(3) << ms
                                    << "] " << text << "\n";
                                g_jsonLogFile << oss.str();
                                g_jsonLogFile.flush();
                            }
                        };
                    } else {
                        logger(::tlcfi::fi::LogLevel::Warning,
                               "JsonRpcSession is null, cannot enable trace logging");
                    }
                };
            }
        }

        // Note: onControlInControl and onControlEndControl are set by
        // cifHost.init() — CifHost owns the loop lifecycle and the CIF_WPS
        // writes that go with each transition.

        // Configure the application host
        ::tlcfi::fi::FiAppHost::Config hostConfig;
        hostConfig.connectTimeout = std::chrono::milliseconds(5000);
        hostConfig.operationTimeout = std::chrono::milliseconds(5000);

        ::tlcfi::fi::FiAppHost appHost(app, hostConfig);

        appHost.onLog = logger;
        bridge.onLog = logger;
        cifHost.onLog = logger;

        // Enable message server if serial port was specified. Buffer sizes are
        // read at runtime from CIF_PB_MAX_IBER/UBER (and the MON_ variants
        // for the logging server) inside CifHost, so they always match the
        // linked controller's CIF.INC regardless of which controller is wired
        // in.
        if (opts.cifSerialPort > 0) {
            cifHost.setMessageServerConfig(opts.cifSerialPort, opts.cifSerialBindAddress,
                                           opts.cifSerialEcho, opts.cifSerialWelcome);
        }
        if (opts.cifLoggingPort > 0) {
            cifHost.setLoggingServerConfig(opts.cifLoggingPort, opts.cifLoggingBindAddress);
        }

        // Controller-specific setup (e.g. loading a config file) right before init().
        if (preInit) {
            preInit(opts);
        }

        // Forward operator-declared controller-only items so the bridge skips
        // them when building mappings. Must precede init() — the mapping
        // table is built inside it.
        cifHost.setIgnoredItems(opts.controllerOnlyItems);

        // Forward explicit TLC-FI ↔ CIF name couples. Also must precede init():
        // the couples are resolved (and validated) while the mapping table is
        // built inside it.
        cifHost.setManualCouples(opts.manuallyCoupledItems);

        // Initialize CifHost (installs hooks, calls applicatieprogramma(CIF_INIT))
        cifHost.init();

        // Run the application with automatic retry and reconnection
        appHost.run(g_running);

        // Clean up
        cifHost.stopLoop();

        if (g_jsonLogFile.is_open()) {
            g_jsonLogFile.close();
        }
        if (g_appLogFile.is_open()) {
            g_appLogFile.close();
        }
        return 0;
    } catch (const std::exception& ex) {
        if (logger) logger(::tlcfi::fi::LogLevel::Fatal,
                           std::string("Unhandled exception: ") + ex.what());
        std::cerr << "\nFATAL ERROR: Unhandled exception: " << ex.what() << "\n";
        std::cerr.flush();
        if (g_jsonLogFile.is_open()) {
            g_jsonLogFile.close();
        }
        if (g_appLogFile.is_open()) {
            g_appLogFile.close();
        }
        return 1;
    } catch (...) {
        if (logger) logger(::tlcfi::fi::LogLevel::Fatal, "Unknown exception occurred");
        std::cerr << "\nFATAL ERROR: Unknown exception occurred\n";
        std::cerr.flush();
        if (g_jsonLogFile.is_open()) {
            g_jsonLogFile.close();
        }
        if (g_appLogFile.is_open()) {
            g_appLogFile.close();
        }
        return 1;
    }
}

} // namespace tlcfi::runtime
