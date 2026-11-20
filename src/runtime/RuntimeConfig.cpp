#include "runtime/RuntimeApp.hpp"

#include <asio/ip/address.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

const char* USAGE =
    "Usage: <controller> [--tlcfi <file>] [--config <path>] [--json-log <file>] "
    "[--cif-serial-port <port>] [--cif-serial-bind <address>] "
    "[--cif-logging-port <port>] [--cif-logging-bind <address>] "
    "[--cif-file-log-dir <dir>] [--cif-file-log-system <name>] [--cif-file-log-max-files <n>] "
    "[--parm-state-dir <dir>] [--parm-state-interval-ms <int>] "
    "[--log-file <file>] [--log-level <level>] "
    "[--console-log-level <level>] [--file-log-level <level>] "
    "[--log-file-max-size-mb <n>] [--log-file-max-backups <n>] "
    "[--json-log-max-size-mb <n>] [--json-log-max-backups <n>]\n"
    "  --tlcfi <file>                  TLC-FI connection config (default: tlcfi.json)\n"
    "  --config <path>                 controller-specific config (e.g. actuated intersection JSON)\n"
    "  --cif-serial-bind <address>     listen address for --cif-serial-port (default 127.0.0.1)\n"
    "  --cif-logging-bind <address>    listen address for --cif-logging-port (default 127.0.0.1)\n"
    "  --cif-file-log-max-files <n>    max .vlg files to keep in --cif-file-log-dir (0=unlimited, default 0)\n"
    "  --parm-state-dir <dir>          directory for CIF_PARM1 persistence file (default '.', empty disables)\n"
    "  --parm-state-interval-ms <n>    min interval between CIF_PARM1 file writes (default 1000, floor 50)\n"
    "  --log-file <file>               write application log (plain text) to this file in addition to console\n"
    "  --log-level <level>             set both console and file log level (trace|debug|info|warning|error|fatal)\n"
    "  --console-log-level <level>     set console log level independently\n"
    "  --file-log-level <level>        set file log level independently\n"
    "  --log-file-max-size-mb <n>      rotate app log file at this size in MB (0=unlimited, default 1)\n"
    "  --log-file-max-backups <n>      number of app log backup files to keep (default 5)\n"
    "  --json-log-max-size-mb <n>      rotate JSON log file at this size in MB (0=unlimited, default 1)\n"
    "  --json-log-max-backups <n>      number of JSON log backup files to keep (default 5)\n";

tlcfi::fi::LogLevel parseLogLevel(const std::string& s) {
    using L = tlcfi::fi::LogLevel;
    if (s == "trace")   return L::Trace;
    if (s == "debug")   return L::Debug;
    if (s == "info")    return L::Info;
    if (s == "warning") return L::Warning;
    if (s == "error")   return L::Error;
    if (s == "fatal")   return L::Fatal;
    throw std::runtime_error("Unknown log level '" + s + "' (expected trace|debug|info|warning|error|fatal)");
}

bool isValidBindAddress(const std::string& address) {
    asio::error_code error;
    asio::ip::make_address(address, error);
    return !error;
}

bool validateBindAddressOrPrint(const char* flag, const std::string& address) {
    if (isValidBindAddress(address)) {
        return true;
    }
    std::cerr << "Error: " << flag << " requires a valid IP address, got '" << address << "'\n"
              << USAGE;
    return false;
}

void applyBindAddressFromJson(const nlohmann::json& j,
                              const char* key,
                              std::string& bindAddress) {
    if (!j.contains(key)) {
        return;
    }
    const auto value = j.at(key).get<std::string>();
    if (!isValidBindAddress(value)) {
        throw std::runtime_error(
            std::string("tlcfi.json: '") + key + "' must be a valid IP address");
    }
    bindAddress = value;
}

} // namespace

namespace tlcfi::runtime {

bool parseArgs(int argc, char* argv[], RuntimeOptions& opts) {
    int argIdx = 1;
    auto requireValue = [&](const char* flag) -> bool {
        if (argIdx + 1 >= argc) {
            std::cerr << "Error: " << flag << " requires an argument\n" << USAGE;
            return false;
        }
        return true;
    };

    while (argIdx < argc) {
        std::string arg = argv[argIdx];
        if (arg == "--json-log" || arg == "-j") {
            if (!requireValue("--json-log")) return false;
            opts.jsonLogPath = argv[++argIdx];
            ++argIdx;
        } else if (arg == "--cif-serial-port") {
            if (!requireValue("--cif-serial-port")) return false;
            opts.cifSerialPort = static_cast<unsigned short>(std::stoi(argv[++argIdx]));
            ++argIdx;
        } else if (arg == "--cif-serial-bind") {
            if (!requireValue("--cif-serial-bind")) return false;
            opts.cifSerialBindAddress = argv[++argIdx];
            if (!validateBindAddressOrPrint("--cif-serial-bind", opts.cifSerialBindAddress)) return false;
            ++argIdx;
        } else if (arg == "--cif-logging-port") {
            if (!requireValue("--cif-logging-port")) return false;
            opts.cifLoggingPort = static_cast<unsigned short>(std::stoi(argv[++argIdx]));
            ++argIdx;
        } else if (arg == "--cif-logging-bind") {
            if (!requireValue("--cif-logging-bind")) return false;
            opts.cifLoggingBindAddress = argv[++argIdx];
            if (!validateBindAddressOrPrint("--cif-logging-bind", opts.cifLoggingBindAddress)) return false;
            ++argIdx;
        } else if (arg == "--cif-file-log-dir") {
            if (!requireValue("--cif-file-log-dir")) return false;
            opts.cifFileLogDir = argv[++argIdx];
            ++argIdx;
        } else if (arg == "--cif-file-log-system") {
            if (!requireValue("--cif-file-log-system")) return false;
            opts.cifFileLogSystemName = argv[++argIdx];
            ++argIdx;
        } else if (arg == "--tlcfi") {
            if (!requireValue("--tlcfi")) return false;
            opts.tlcfiConfigPath = argv[++argIdx];
            ++argIdx;
        } else if (arg == "--config") {
            if (!requireValue("--config")) return false;
            opts.configPath = argv[++argIdx];
            ++argIdx;
        } else if (arg == "--parm-state-dir") {
            if (!requireValue("--parm-state-dir")) return false;
            opts.parmStateDir = argv[++argIdx];
            ++argIdx;
        } else if (arg == "--parm-state-interval-ms") {
            if (!requireValue("--parm-state-interval-ms")) return false;
            const int v = std::stoi(argv[++argIdx]);
            if (v < 50) {
                std::cerr << "Error: --parm-state-interval-ms must be >= 50\n";
                return false;
            }
            opts.parmStateIntervalMs = v;
            ++argIdx;
        } else if (arg == "--log-file") {
            if (!requireValue("--log-file")) return false;
            opts.logFilePath = argv[++argIdx];
            ++argIdx;
        } else if (arg == "--log-level") {
            if (!requireValue("--log-level")) return false;
            try {
                const auto level = parseLogLevel(argv[++argIdx]);
                opts.consoleLogLevel = level;
                opts.fileLogLevel = level;
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n" << USAGE;
                return false;
            }
            ++argIdx;
        } else if (arg == "--console-log-level") {
            if (!requireValue("--console-log-level")) return false;
            try {
                opts.consoleLogLevel = parseLogLevel(argv[++argIdx]);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n" << USAGE;
                return false;
            }
            ++argIdx;
        } else if (arg == "--file-log-level") {
            if (!requireValue("--file-log-level")) return false;
            try {
                opts.fileLogLevel = parseLogLevel(argv[++argIdx]);
            } catch (const std::exception& e) {
                std::cerr << "Error: " << e.what() << "\n" << USAGE;
                return false;
            }
            ++argIdx;
        } else if (arg == "--log-file-max-size-mb") {
            if (!requireValue("--log-file-max-size-mb")) return false;
            opts.logFileMaxSizeMb = std::stoi(argv[++argIdx]);
            ++argIdx;
        } else if (arg == "--log-file-max-backups") {
            if (!requireValue("--log-file-max-backups")) return false;
            opts.logFileMaxBackups = std::stoi(argv[++argIdx]);
            ++argIdx;
        } else if (arg == "--json-log-max-size-mb") {
            if (!requireValue("--json-log-max-size-mb")) return false;
            opts.jsonLogMaxSizeMb = std::stoi(argv[++argIdx]);
            ++argIdx;
        } else if (arg == "--json-log-max-backups") {
            if (!requireValue("--json-log-max-backups")) return false;
            opts.jsonLogMaxBackups = std::stoi(argv[++argIdx]);
            ++argIdx;
        } else if (arg == "--cif-file-log-max-files") {
            if (!requireValue("--cif-file-log-max-files")) return false;
            opts.cifFileLogMaxFiles = std::stoi(argv[++argIdx]);
            ++argIdx;
        } else {
            std::cerr << "Unexpected argument: " << arg << "\n" << USAGE;
            return false;
        }
    }
    return true;
}

void loadTlcfiConfig(const std::string& path, RuntimeOptions& opts) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Warning: TLC-FI connection config '" << path
                  << "' not found; using defaults (" << opts.host << ":" << opts.port << ")\n";
        return;
    }

    nlohmann::json j;
    file >> j;

    if (j.contains("host"))     opts.host = j.at("host").get<std::string>();
    if (j.contains("username")) opts.username = j.at("username").get<std::string>();
    if (j.contains("password")) opts.password = j.at("password").get<std::string>();
    if (j.contains("uri"))      opts.uri = j.at("uri").get<std::string>();
    if (j.contains("port")) {
        const auto& p = j.at("port");
        opts.port = p.is_string() ? p.get<std::string>() : std::to_string(p.get<long long>());
    }
    if (j.contains("tls")) {
        const auto& tls = j.at("tls");
        if (!tls.is_object()) {
            throw std::runtime_error("tlcfi.json: 'tls' must be an object");
        }
        if (tls.contains("enabled"))          opts.tls.enabled = tls.at("enabled").get<bool>();
        if (tls.contains("caFile"))           opts.tls.caFile = tls.at("caFile").get<std::string>();
        if (tls.contains("verifyPeer"))       opts.tls.verifyPeer = tls.at("verifyPeer").get<bool>();
        if (tls.contains("expectedPeerName")) {
            opts.tls.expectedPeerName = tls.at("expectedPeerName").get<std::string>();
        }
    }

    if (j.contains("cifSerialPort"))    opts.cifSerialPort = static_cast<unsigned short>(j.at("cifSerialPort").get<int>());
    applyBindAddressFromJson(j, "cifSerialBindAddress", opts.cifSerialBindAddress);
    if (j.contains("cifSerialEcho"))    opts.cifSerialEcho = j.at("cifSerialEcho").get<bool>();
    if (j.contains("cifSerialWelcome")) opts.cifSerialWelcome = j.at("cifSerialWelcome").get<std::string>();
    if (j.contains("cifLoggingPort"))   opts.cifLoggingPort = static_cast<unsigned short>(j.at("cifLoggingPort").get<int>());
    applyBindAddressFromJson(j, "cifLoggingBindAddress", opts.cifLoggingBindAddress);
    if (j.contains("cifFileLogDir"))    opts.cifFileLogDir = j.at("cifFileLogDir").get<std::string>();
    if (j.contains("cifFileLogSystem")) opts.cifFileLogSystemName = j.at("cifFileLogSystem").get<std::string>();
    if (j.contains("jsonLog"))       opts.jsonLogPath = j.at("jsonLog").get<std::string>();
    if (j.contains("parmStateDir"))  opts.parmStateDir = j.at("parmStateDir").get<std::string>();
    if (j.contains("parmStateIntervalMs")) {
        const int v = j.at("parmStateIntervalMs").get<int>();
        if (v < 50) {
            throw std::runtime_error("tlcfi.json: parmStateIntervalMs must be >= 50");
        }
        opts.parmStateIntervalMs = v;
    }

    if (j.contains("logFile")) opts.logFilePath = j.at("logFile").get<std::string>();
    if (j.contains("logLevel")) {
        const auto level = parseLogLevel(j.at("logLevel").get<std::string>());
        opts.consoleLogLevel = level;
        opts.fileLogLevel = level;
    }
    if (j.contains("consoleLogLevel"))
        opts.consoleLogLevel = parseLogLevel(j.at("consoleLogLevel").get<std::string>());
    if (j.contains("fileLogLevel"))
        opts.fileLogLevel = parseLogLevel(j.at("fileLogLevel").get<std::string>());

    if (j.contains("jsonLogMaxSizeMb"))    opts.jsonLogMaxSizeMb    = j.at("jsonLogMaxSizeMb").get<int>();
    if (j.contains("jsonLogMaxBackups"))   opts.jsonLogMaxBackups   = j.at("jsonLogMaxBackups").get<int>();
    if (j.contains("logFileMaxSizeMb"))    opts.logFileMaxSizeMb    = j.at("logFileMaxSizeMb").get<int>();
    if (j.contains("logFileMaxBackups"))   opts.logFileMaxBackups   = j.at("logFileMaxBackups").get<int>();
    if (j.contains("cifFileLogMaxFiles"))  opts.cifFileLogMaxFiles  = j.at("cifFileLogMaxFiles").get<int>();

    if (j.contains("controllerOnlyItems")) {
        const auto& arr = j.at("controllerOnlyItems");
        if (!arr.is_array()) {
            throw std::runtime_error("tlcfi.json: 'controllerOnlyItems' must be an array");
        }
        for (const auto& entry : arr) {
            if (!entry.is_object() || !entry.contains("name") || !entry.contains("type")) {
                throw std::runtime_error(
                    "tlcfi.json: each controllerOnlyItems entry needs 'name' and 'type'");
            }
            const auto name = entry.at("name").get<std::string>();
            const auto type = entry.at("type").get<std::string>();
            if      (type == "signalGroup") opts.controllerOnlyItems.signalGroups.insert(name);
            else if (type == "output")      opts.controllerOnlyItems.outputs.insert(name);
            else if (type == "detector")    opts.controllerOnlyItems.detectors.insert(name);
            else if (type == "input")       opts.controllerOnlyItems.inputs.insert(name);
            else {
                throw std::runtime_error(
                    "tlcfi.json: controllerOnlyItems entry '" + name +
                    "' has unknown type '" + type +
                    "' (expected signalGroup|output|detector|input)");
            }
        }
    }

    if (j.contains("manuallyCoupledItems")) {
        const auto& arr = j.at("manuallyCoupledItems");
        if (!arr.is_array()) {
            throw std::runtime_error("tlcfi.json: 'manuallyCoupledItems' must be an array");
        }

        // Per-type (signalGroup, output, detector, input) duplicate tracking so
        // no cifName or tlcfiName is coupled twice within the same type.
        std::set<std::string> seenCif[4];
        std::set<std::string> seenTlcfi[4];
        auto typeIndex = [](const std::string& t) -> int {
            if (t == "signalGroup") return 0;
            if (t == "output")      return 1;
            if (t == "detector")    return 2;
            if (t == "input")       return 3;
            return -1;
        };

        for (const auto& entry : arr) {
            if (!entry.is_object() || !entry.contains("tlcfiName") ||
                !entry.contains("cifName") || !entry.contains("type")) {
                throw std::runtime_error(
                    "tlcfi.json: each manuallyCoupledItems entry needs 'tlcfiName', 'cifName' and 'type'");
            }
            const auto tlcfiName = entry.at("tlcfiName").get<std::string>();
            const auto cifName   = entry.at("cifName").get<std::string>();
            const auto type      = entry.at("type").get<std::string>();

            if (tlcfiName.empty() || cifName.empty()) {
                throw std::runtime_error(
                    "tlcfi.json: manuallyCoupledItems entries must have non-empty 'tlcfiName' and 'cifName'");
            }

            const int ti = typeIndex(type);
            if (ti < 0) {
                throw std::runtime_error(
                    "tlcfi.json: manuallyCoupledItems entry {tlcfiName=" + tlcfiName +
                    ", cifName=" + cifName + "} has unknown type '" + type +
                    "' (expected signalGroup|output|detector|input)");
            }

            if (!seenCif[ti].insert(cifName).second) {
                throw std::runtime_error(
                    "tlcfi.json: duplicate manuallyCoupledItems cifName '" + cifName +
                    "' for type '" + type + "'");
            }
            if (!seenTlcfi[ti].insert(tlcfiName).second) {
                throw std::runtime_error(
                    "tlcfi.json: duplicate manuallyCoupledItems tlcfiName '" + tlcfiName +
                    "' for type '" + type + "'");
            }

            std::vector<::cif::ManualCouple>* target = nullptr;
            const std::set<std::string>* ignored = nullptr;
            switch (ti) {
                case 0: target = &opts.manuallyCoupledItems.signalGroups;
                        ignored = &opts.controllerOnlyItems.signalGroups; break;
                case 1: target = &opts.manuallyCoupledItems.outputs;
                        ignored = &opts.controllerOnlyItems.outputs; break;
                case 2: target = &opts.manuallyCoupledItems.detectors;
                        ignored = &opts.controllerOnlyItems.detectors; break;
                case 3: target = &opts.manuallyCoupledItems.inputs;
                        ignored = &opts.controllerOnlyItems.inputs; break;
            }

            // A name cannot be both manually coupled and declared controller-only
            // for the same type — the two escape hatches contradict each other.
            if (ignored->count(cifName) != 0) {
                throw std::runtime_error(
                    "tlcfi.json: '" + cifName + "' is both a manuallyCoupledItems cifName and a "
                    "controllerOnlyItems entry for type '" + type + "'");
            }
            if (ignored->count(tlcfiName) != 0) {
                throw std::runtime_error(
                    "tlcfi.json: manuallyCoupledItems tlcfiName '" + tlcfiName +
                    "' is also a controllerOnlyItems entry for type '" + type + "'");
            }

            target->push_back(::cif::ManualCouple{tlcfiName, cifName});
        }
    }

    if (j.contains("ptpLinks")) {
        const auto& arr = j.at("ptpLinks");
        if (!arr.is_array()) {
            throw std::runtime_error("tlcfi.json: 'ptpLinks' must be an array");
        }
        for (const auto& entry : arr) {
            if (!entry.is_object() || !entry.contains("linkId") ||
                !entry.contains("mode") || !entry.contains("peerIp")) {
                throw std::runtime_error(
                    "tlcfi.json: each ptpLinks entry needs 'linkId', 'mode' and 'peerIp'");
            }
            ::cif::ptp::PtpLinkConfig link;
            link.linkId = entry.at("linkId").get<int>();
            // parseMode throws std::runtime_error on an invalid mode string.
            link.mode = ::cif::ptp::parseMode(entry.at("mode").get<std::string>());
            link.peerIp = entry.at("peerIp").get<std::string>();
            if (entry.contains("tcpPort")) {
                link.tcpPort = static_cast<std::uint16_t>(entry.at("tcpPort").get<int>());
            }
            if (!isValidBindAddress(link.peerIp)) {
                throw std::runtime_error(
                    "tlcfi.json: ptpLinks linkId " + std::to_string(link.linkId) +
                    " 'peerIp' must be a valid IP address, got '" + link.peerIp + "'");
            }
            opts.ptpLinks.push_back(std::move(link));
        }
        // Structural rules (count <= 4, linkId uniqueness, peerIp present).
        ::cif::ptp::validatePtpLinks(opts.ptpLinks);
    }
}

} // namespace tlcfi::runtime
