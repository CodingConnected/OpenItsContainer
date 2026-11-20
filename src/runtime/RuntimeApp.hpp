#pragma once

// --- Standard Library Includes ---
#include <functional>
#include <string>

// --- Project Includes ---
#include "cif/CifBridge.hpp"
#include "cif/ptp/PtpConfig.hpp"
#include "fi/LogLevel.hpp"
#include "tcp/TlsConfig.hpp"

// --- Standard Library Includes (additional) ---
#include <vector>

// --- Namespace Declaration ---
namespace tlcfi::runtime {

/**
 * @brief Parsed command-line options for the container runtime.
 *
 * These describe the connection to the TLC-FI Facilities and the optional
 * diagnostic servers. They are protocol/transport concerns only — the runtime
 * is controller-agnostic and never interprets @ref configPath.
 */
struct RuntimeOptions {
    // --- TLC-FI connection (loaded from the tlcfi.json connection file) ---
    std::string host = "localhost";          ///< TLC-FI host to connect to.
    std::string port = "11501";              ///< TLC-FI port.
    std::string username = "Controller1";    ///< Registration username.
    std::string password = "Password1";      ///< Registration password.
    std::string uri = "app://Controller1";   ///< Registration URI.
    tcp::TlsConfig tls;                       ///< Optional hardened TLS client settings.

    // --- Per-run diagnostic toggles (command-line flags) ---
    std::string jsonLogPath;                  ///< If set, received JSON-RPC is traced to this file.
    unsigned short cifSerialPort = 0;            ///< If non-zero, expose CIF IBER/UBER over TCP.
    std::string cifSerialBindAddress = "127.0.0.1"; ///< Listen address for @ref cifSerialPort (loopback default).
    bool cifSerialEcho = false;                  ///< If true, server echoes each character back and sends IAC WILL ECHO on connect.
    std::string cifSerialWelcome;                ///< If non-empty, sent to each client immediately on connection.
    unsigned short cifLoggingPort = 0;           ///< If non-zero, expose CIF MON IBER/UBER over TCP.
    std::string cifLoggingBindAddress = "127.0.0.1"; ///< Listen address for @ref cifLoggingPort (loopback default).
    std::string cifFileLogDir;                   ///< If set, enable CIF_FILE_UBER file logging here.
    std::string cifFileLogSystemName;            ///< Override <SYSTEM> filename prefix (default: username).

    /// Directory for the CIF_PARM1 persistence file. Empty disables the
    /// feature; default "." writes "<system>_cifparm1.json" in the working
    /// directory. The filename's <system> is the effective system name (the
    /// same value the file VLOG writer uses).
    std::string parmStateDir = ".";

    /// Minimum interval between disk writes for the CIF_PARM1 persistence
    /// file. Floor of 50 ms enforced at the Param1Store level. The writer
    /// rate-caps even a misbehaving controller's per-cycle parameter mutation,
    /// so a high-frequency CIF_PARM1WIJZ source still produces at most one
    /// write per interval.
    int parmStateIntervalMs = 1000;

    /// Path to the TLC-FI connection config file (default "tlcfi.json").
    std::string tlcfiConfigPath = "tlcfi.json";

    /// Opaque path passed through for controllers that load their own config
    /// file (e.g. the actuated example's intersection JSON via --config).
    /// The runtime never reads this; a @ref PreInitHook may use it.
    std::string configPath;

    // --- Application log settings ---

    /// If set, application log messages are also written (plain text, no ANSI) to this file.
    std::string logFilePath;

    /// Minimum log level printed to the console (default: Info).
    fi::LogLevel consoleLogLevel = fi::LogLevel::Info;

    /// Minimum log level written to @ref logFilePath (default: Info).
    fi::LogLevel fileLogLevel = fi::LogLevel::Info;

    // --- Rolling log file limits ---

    /// Max size in MB for @ref jsonLogPath before rotation (0 = unlimited; default 1 MB).
    int jsonLogMaxSizeMb = 1;
    /// Number of JSON log backup files to keep (default 5).
    int jsonLogMaxBackups = 5;

    /// Max size in MB for @ref logFilePath before rotation (0 = unlimited; default 1 MB).
    int logFileMaxSizeMb = 1;
    /// Number of application log backup files to keep (default 5).
    int logFileMaxBackups = 5;

    /// Maximum number of CIF VLOG (.vlg) files to keep in @ref cifFileLogDir (0 = unlimited, default 0).
    /// The oldest files are removed whenever a new file is finalized and the count exceeds this limit.
    int cifFileLogMaxFiles = 0;

    /// Controller-side items the operator has declared absent in the TLC.
    /// Populated from the tlcfi.json "controllerOnlyItems" array; forwarded
    /// to CifHost before init() so the bridge skips them during mapping.
    ::cif::IgnoredItems controllerOnlyItems;

    /// Explicit TLC-FI ↔ CIF name couples for items whose names differ on the
    /// two sides. Populated from the tlcfi.json "manuallyCoupledItems" array;
    /// forwarded to CifHost before init(). Manual couples win over identity
    /// matches and both names of every couple must be found or startup aborts.
    ::cif::ManualCouples manuallyCoupledItems;

    /// PTP (Point-to-Point) links for exchanging koppelsignalen with peer VRIs.
    /// Populated from the tlcfi.json "ptpLinks" array. Consumed lazily by the
    /// PTP transport (via @ref activeOptions) only when a PTP-enabled controller
    /// actually calls the driver symbols; empty by default (no PTP).
    std::vector<::cif::ptp::PtpLinkConfig> ptpLinks;
};

/**
 * @brief Hook invoked once, immediately before the hosted controller is
 *        initialised (i.e. before applicatieprogramma(CIF_INIT)).
 *
 * Use it to perform any controller-specific setup that must happen before
 * init() — for example, an example/actuated controller loading its
 * intersection configuration. External controllers normally pass nothing.
 *
 * @param opts The fully parsed runtime options (e.g. opts.configPath).
 */
using PreInitHook = std::function<void(const RuntimeOptions& opts)>;

/**
 * @brief Parse argv into RuntimeOptions.
 *
 * Recognised flags: --tlcfi <path> (connection config file), --config <path>
 * (controller-specific config, opaque to the runtime), --json-log/-j,
 * --cif-serial-port, --cif-serial-bind, --cif-logging-port, --cif-logging-bind,
 * --cif-file-log-dir, --cif-file-log-system, --log-file, --log-level,
 * --console-log-level, --file-log-level. There are no positional arguments;
 * the TLC-FI connection is read from the tlcfi.json file (see @ref loadTlcfiConfig).
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param[out] opts Parsed options on success.
 * @return true on success; false if a usage error was printed.
 */
bool parseArgs(int argc, char* argv[], RuntimeOptions& opts);

/**
 * @brief Overlay runtime settings from the tlcfi.json file onto opts.
 *
 * Reads (all optional):
 *  - Connection: host, port (string or number), username, password, uri.
 *  - TLS object: enabled, caFile, verifyPeer, expectedPeerName.
 *  - CIF streaming / diagnostics: cifSerialPort, cifSerialBindAddress,
 *    cifSerialEcho, cifSerialWelcome,
 *    cifLoggingPort, cifLoggingBindAddress, cifFileLogDir, cifFileLogSystem, jsonLog.
 *
 * Absent keys keep their existing value, so the built-in defaults survive a
 * partial file. A missing file is non-fatal: defaults are used and a warning
 * is printed. This file makes a fully argument-free startup possible; any
 * command-line flag still overrides the corresponding JSON value.
 *
 * @param path Path to the config file (e.g. "tlcfi.json").
 * @param[in,out] opts Options to overlay onto.
 */
void loadTlcfiConfig(const std::string& path, RuntimeOptions& opts);

/**
 * @brief The runtime options of the currently running container.
 *
 * Set once by @ref runContainer after configuration is parsed, and valid for
 * the remainder of the process. Provided so subsystems that are created lazily,
 * deep inside the control loop and without an injection point — notably the
 * optional PTP transport, which reads @ref RuntimeOptions::ptpLinks — can pull
 * their configuration. This is a one-directional accessor (callers depend on
 * the runtime, never the reverse), which keeps the optional PTP translation
 * unit from being force-linked into a controller that does not use it.
 *
 * Before @ref runContainer sets it, a reference to a default-constructed
 * (empty) RuntimeOptions is returned, so callers never see a dangling or null
 * reference.
 *
 * @return The active runtime options (or empty defaults before startup).
 */
const RuntimeOptions& activeOptions();

/**
 * @brief A parameterless action run once at container startup.
 *
 * Registered pre-main by optional subsystems that must be brought up when the
 * container starts, independently of control state (currently the PTP
 * transport). See @ref registerStartupHook.
 */
using StartupHook = std::function<void()>;

/**
 * @brief Register a hook to run once at container startup.
 *
 * This is the linkage-safe inversion point for optional subsystems: an
 * always-linked runtime must never name a symbol in an optional translation
 * unit (doing so would force-link it into every controller). Instead the
 * optional TU — e.g. the PTP transport's PtpTransport.cpp, which is linked only
 * when a controller references its driver symbols — registers a hook from a
 * file-scope initializer. @ref runContainer runs all registered hooks once,
 * immediately after configuration is published via @ref activeOptions, so a
 * hook may safely read the active options.
 *
 * Registrations from controllers that do not link the optional TU never happen
 * (the initializer is dropped with the TU), so this stays zero-cost.
 *
 * @param hook The action to run at startup (ignored if empty).
 */
void registerStartupHook(StartupHook hook);

/**
 * @brief Run the TLC-FI container: connect, host the CVN-C controller, and
 *        drive its control loop until interrupted.
 *
 * Owns the full generic lifecycle — argument parsing, FiControllerApp,
 * CifBridge, CifHost, FiAppHost retry loop, logging, signal handling. The
 * hosted controller is whatever CVN-C controller is linked into the final
 * binary (it provides the CIF_* buffers, applicatieprogramma, and
 * getCifItemName). This function does not know or care which controller that
 * is — that is the SDK boundary.
 *
 * @param argc Argument count (forwarded to parseArgs).
 * @param argv Argument vector.
 * @param preInit Optional controller-specific setup run just before init().
 * @return Process exit code (0 on clean shutdown, non-zero on error).
 */
int runContainer(int argc, char* argv[], const PreInitHook& preInit = {});

} // namespace tlcfi::runtime
