#include "runtime/RuntimeApp.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

class TempConfig {
public:
    explicit TempConfig(const std::string& contents)
        : path_(std::filesystem::temp_directory_path() / "tlcfi_runtime_tls_test.json") {
        std::ofstream file(path_);
        file << contents;
    }

    ~TempConfig() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

} // namespace

// =============================================================================
// TLS settings
// =============================================================================

TEST(RuntimeConfig, LoadsVerifiedTlsClientSettings) {
    TempConfig config(R"({
        "host": "192.0.2.10",
        "tls": {
            "enabled": true,
            "caFile": "deployment-ca.pem",
            "verifyPeer": true,
            "expectedPeerName": "tlc.example.test"
        }
    })");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.host, "192.0.2.10");
    EXPECT_TRUE(options.tls.enabled);
    EXPECT_EQ(options.tls.caFile, "deployment-ca.pem");
    EXPECT_TRUE(options.tls.verifyPeer);
    EXPECT_EQ(options.tls.expectedPeerName, "tlc.example.test");
}

TEST(RuntimeConfig, TlsVerificationDefaultsToEnabled) {
    TempConfig config(R"({"tls": {"enabled": true}})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_TRUE(options.tls.enabled);
    EXPECT_TRUE(options.tls.verifyPeer);
}

TEST(RuntimeConfig, RejectsNonObjectTlsConfiguration) {
    TempConfig config(R"({"tls": true})");

    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(
        tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
        std::runtime_error);
}

// =============================================================================
// CIF serial / logging ports
// =============================================================================

TEST(RuntimeConfig, LoadsCifSerialAndLoggingBindAddresses) {
    TempConfig config(R"({
        "cifSerialPort": 9001,
        "cifSerialBindAddress": "0.0.0.0",
        "cifLoggingPort": 9002,
        "cifLoggingBindAddress": "192.0.2.5"
    })");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.cifSerialPort, 9001);
    EXPECT_EQ(options.cifSerialBindAddress, "0.0.0.0");
    EXPECT_EQ(options.cifLoggingPort, 9002);
    EXPECT_EQ(options.cifLoggingBindAddress, "192.0.2.5");
}

TEST(RuntimeConfig, RejectsInvalidCifSerialBindAddressInJson) {
    TempConfig config(R"({"cifSerialBindAddress": "not-an-ip"})");

    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(
        tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
        std::runtime_error);
}

TEST(RuntimeConfig, ParseArgsAcceptsCifSerialBindOverride) {
    const char* argv[] = {
        "controller",
        "--cif-serial-port", "9001",
        "--cif-serial-bind", "0.0.0.0",
    };

    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(5, const_cast<char**>(argv), options));
    EXPECT_EQ(options.cifSerialPort, 9001);
    EXPECT_EQ(options.cifSerialBindAddress, "0.0.0.0");
}

TEST(RuntimeConfig, ParseArgsRejectsInvalidCifSerialBind) {
    const char* argv[] = {
        "controller",
        "--cif-serial-bind", "not-an-ip",
    };

    tlcfi::runtime::RuntimeOptions options;
    EXPECT_FALSE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
}

// =============================================================================
// Application log file path
// =============================================================================

TEST(RuntimeConfig, LogFilePathLoadedFromJson) {
    TempConfig config(R"({"logFile": "/var/log/app.log"})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.logFilePath, "/var/log/app.log");
}

TEST(RuntimeConfig, ParseArgs_LogFile) {
    const char* argv[] = { "controller", "--log-file", "app.log" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.logFilePath, "app.log");
}

// =============================================================================
// Log levels
// =============================================================================

TEST(RuntimeConfig, LogLevelSetsBothConsolAndFileLevelFromJson) {
    TempConfig config(R"({"logLevel": "warning"})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.consoleLogLevel, tlcfi::fi::LogLevel::Warning);
    EXPECT_EQ(options.fileLogLevel,    tlcfi::fi::LogLevel::Warning);
}

TEST(RuntimeConfig, IndividualLogLevelsOverrideLogLevelFromJson) {
    TempConfig config(R"({
        "logLevel": "error",
        "consoleLogLevel": "debug",
        "fileLogLevel": "info"
    })");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.consoleLogLevel, tlcfi::fi::LogLevel::Debug);
    EXPECT_EQ(options.fileLogLevel,    tlcfi::fi::LogLevel::Info);
}

TEST(RuntimeConfig, ConsoleAndFileLevelsSetIndependentlyFromJson) {
    TempConfig config(R"({
        "consoleLogLevel": "trace",
        "fileLogLevel": "error"
    })");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.consoleLogLevel, tlcfi::fi::LogLevel::Trace);
    EXPECT_EQ(options.fileLogLevel,    tlcfi::fi::LogLevel::Error);
}

TEST(RuntimeConfig, InvalidLogLevelInJsonThrows) {
    TempConfig config(R"({"logLevel": "verbose"})");

    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(
        tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
        std::runtime_error);
}

TEST(RuntimeConfig, ParseArgs_LogLevel_SetsBothLevels) {
    const char* argv[] = { "controller", "--log-level", "warning" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.consoleLogLevel, tlcfi::fi::LogLevel::Warning);
    EXPECT_EQ(options.fileLogLevel,    tlcfi::fi::LogLevel::Warning);
}

TEST(RuntimeConfig, ParseArgs_LogLevel_Invalid_ReturnsFalse) {
    const char* argv[] = { "controller", "--log-level", "verbose" };
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_FALSE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
}

TEST(RuntimeConfig, ParseArgs_ConsoleLogLevel) {
    const char* argv[] = { "controller", "--console-log-level", "debug" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.consoleLogLevel, tlcfi::fi::LogLevel::Debug);
}

TEST(RuntimeConfig, ParseArgs_FileLogLevel) {
    const char* argv[] = { "controller", "--file-log-level", "error" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.fileLogLevel, tlcfi::fi::LogLevel::Error);
}

// =============================================================================
// App log rotation limits
// =============================================================================

TEST(RuntimeConfig, LogFileMaxSizeMbLoadedFromJson) {
    TempConfig config(R"({"logFileMaxSizeMb": 10})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.logFileMaxSizeMb, 10);
}

TEST(RuntimeConfig, LogFileMaxBackupsLoadedFromJson) {
    TempConfig config(R"({"logFileMaxBackups": 3})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.logFileMaxBackups, 3);
}

TEST(RuntimeConfig, ParseArgs_LogFileMaxSizeMb) {
    const char* argv[] = { "controller", "--log-file-max-size-mb", "10" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.logFileMaxSizeMb, 10);
}

TEST(RuntimeConfig, ParseArgs_LogFileMaxBackups) {
    const char* argv[] = { "controller", "--log-file-max-backups", "3" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.logFileMaxBackups, 3);
}

// =============================================================================
// JSON log
// =============================================================================

TEST(RuntimeConfig, JsonLogPathLoadedFromJson) {
    TempConfig config(R"({"jsonLog": "/var/log/trace.json"})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.jsonLogPath, "/var/log/trace.json");
}

TEST(RuntimeConfig, JsonLogMaxSizeMbLoadedFromJson) {
    TempConfig config(R"({"jsonLogMaxSizeMb": 5})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.jsonLogMaxSizeMb, 5);
}

TEST(RuntimeConfig, JsonLogMaxBackupsLoadedFromJson) {
    TempConfig config(R"({"jsonLogMaxBackups": 2})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.jsonLogMaxBackups, 2);
}

TEST(RuntimeConfig, ParseArgs_JsonLog) {
    const char* argv[] = { "controller", "--json-log", "trace.json" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.jsonLogPath, "trace.json");
}

TEST(RuntimeConfig, ParseArgs_JsonLog_ShortAlias) {
    const char* argv[] = { "controller", "-j", "trace.json" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.jsonLogPath, "trace.json");
}

TEST(RuntimeConfig, ParseArgs_JsonLogMaxSizeMb) {
    const char* argv[] = { "controller", "--json-log-max-size-mb", "5" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.jsonLogMaxSizeMb, 5);
}

TEST(RuntimeConfig, ParseArgs_JsonLogMaxBackups) {
    const char* argv[] = { "controller", "--json-log-max-backups", "2" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.jsonLogMaxBackups, 2);
}

// =============================================================================
// CIF file logging (VLOG)
// =============================================================================

TEST(RuntimeConfig, CifFileLogDirLoadedFromJson) {
    TempConfig config(R"({"cifFileLogDir": "/var/log/cif"})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.cifFileLogDir, "/var/log/cif");
}

TEST(RuntimeConfig, CifFileLogSystemLoadedFromJson) {
    TempConfig config(R"({"cifFileLogSystem": "SYS1"})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.cifFileLogSystemName, "SYS1");
}

TEST(RuntimeConfig, CifFileLogMaxFilesLoadedFromJson) {
    TempConfig config(R"({"cifFileLogMaxFiles": 10})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.cifFileLogMaxFiles, 10);
}

TEST(RuntimeConfig, CifFileLogMaxFiles_Zero_Means_Unlimited) {
    TempConfig config(R"({"cifFileLogMaxFiles": 0})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.cifFileLogMaxFiles, 0);
}

TEST(RuntimeConfig, ParseArgs_CifFileLogDir) {
    const char* argv[] = { "controller", "--cif-file-log-dir", "/var/log/cif" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.cifFileLogDir, "/var/log/cif");
}

TEST(RuntimeConfig, ParseArgs_CifFileLogSystem) {
    const char* argv[] = { "controller", "--cif-file-log-system", "SYS1" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.cifFileLogSystemName, "SYS1");
}

TEST(RuntimeConfig, ParseArgs_CifFileLogMaxFiles) {
    const char* argv[] = { "controller", "--cif-file-log-max-files", "10" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.cifFileLogMaxFiles, 10);
}

// =============================================================================
// Parm state persistence
// =============================================================================

TEST(RuntimeConfig, ParmStateDirLoadedFromJson) {
    TempConfig config(R"({"parmStateDir": "/tmp/state"})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.parmStateDir, "/tmp/state");
}

TEST(RuntimeConfig, ParmStateDirEmpty_DisablesFeature) {
    TempConfig config(R"({"parmStateDir": ""})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_TRUE(options.parmStateDir.empty());
}

TEST(RuntimeConfig, ParmStateIntervalMsLoadedFromJson) {
    TempConfig config(R"({"parmStateIntervalMs": 500})");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    EXPECT_EQ(options.parmStateIntervalMs, 500);
}

TEST(RuntimeConfig, ParmStateIntervalMs_BelowMinimum_Throws) {
    TempConfig config(R"({"parmStateIntervalMs": 49})");

    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(
        tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
        std::runtime_error);
}

TEST(RuntimeConfig, ParseArgs_ParmStateDir) {
    const char* argv[] = { "controller", "--parm-state-dir", "/tmp/state" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.parmStateDir, "/tmp/state");
}

TEST(RuntimeConfig, ParseArgs_ParmStateIntervalMs) {
    const char* argv[] = { "controller", "--parm-state-interval-ms", "500" };
    tlcfi::runtime::RuntimeOptions options;
    ASSERT_TRUE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
    EXPECT_EQ(options.parmStateIntervalMs, 500);
}

TEST(RuntimeConfig, ParseArgs_ParmStateIntervalMs_TooSmall_ReturnsFalse) {
    const char* argv[] = { "controller", "--parm-state-interval-ms", "49" };
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_FALSE(tlcfi::runtime::parseArgs(3, const_cast<char**>(argv), options));
}

// =============================================================================
// manuallyCoupledItems
// =============================================================================

TEST(RuntimeConfig, LoadsManuallyCoupledItemsPerType) {
    TempConfig config(R"({
        "manuallyCoupledItems": [
            { "tlcfiName": "us01", "cifName": "US001", "type": "signalGroup" },
            { "tlcfiName": "o1",   "cifName": "OUT1",  "type": "output" },
            { "tlcfiName": "d1",   "cifName": "DET1",  "type": "detector" },
            { "tlcfiName": "i1",   "cifName": "IN1",   "type": "input" }
        ]
    })");

    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    ASSERT_EQ(options.manuallyCoupledItems.signalGroups.size(), 1U);
    EXPECT_EQ(options.manuallyCoupledItems.signalGroups[0].tlcfiName, "us01");
    EXPECT_EQ(options.manuallyCoupledItems.signalGroups[0].cifName, "US001");
    ASSERT_EQ(options.manuallyCoupledItems.outputs.size(), 1U);
    EXPECT_EQ(options.manuallyCoupledItems.outputs[0].cifName, "OUT1");
    ASSERT_EQ(options.manuallyCoupledItems.detectors.size(), 1U);
    EXPECT_EQ(options.manuallyCoupledItems.detectors[0].cifName, "DET1");
    ASSERT_EQ(options.manuallyCoupledItems.inputs.size(), 1U);
    EXPECT_EQ(options.manuallyCoupledItems.inputs[0].cifName, "IN1");
}

TEST(RuntimeConfig, RejectsManuallyCoupledItemsNotArray) {
    TempConfig config(R"({"manuallyCoupledItems": {"tlcfiName": "a"}})");
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
                 std::runtime_error);
}

TEST(RuntimeConfig, RejectsManuallyCoupledItemMissingFields) {
    TempConfig config(R"({"manuallyCoupledItems": [{"tlcfiName": "a", "type": "input"}]})");
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
                 std::runtime_error);
}

TEST(RuntimeConfig, RejectsManuallyCoupledItemEmptyName) {
    TempConfig config(R"({"manuallyCoupledItems": [{"tlcfiName": "", "cifName": "X", "type": "input"}]})");
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
                 std::runtime_error);
}

TEST(RuntimeConfig, RejectsManuallyCoupledItemUnknownType) {
    TempConfig config(R"({"manuallyCoupledItems": [{"tlcfiName": "a", "cifName": "X", "type": "widget"}]})");
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
                 std::runtime_error);
}

TEST(RuntimeConfig, RejectsDuplicateCifNameWithinType) {
    TempConfig config(R"({
        "manuallyCoupledItems": [
            { "tlcfiName": "a", "cifName": "X", "type": "signalGroup" },
            { "tlcfiName": "b", "cifName": "X", "type": "signalGroup" }
        ]
    })");
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
                 std::runtime_error);
}

TEST(RuntimeConfig, RejectsDuplicateTlcfiNameWithinType) {
    TempConfig config(R"({
        "manuallyCoupledItems": [
            { "tlcfiName": "a", "cifName": "X", "type": "signalGroup" },
            { "tlcfiName": "a", "cifName": "Y", "type": "signalGroup" }
        ]
    })");
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
                 std::runtime_error);
}

TEST(RuntimeConfig, AllowsSameNameAcrossDifferentTypes) {
    TempConfig config(R"({
        "manuallyCoupledItems": [
            { "tlcfiName": "x", "cifName": "X", "type": "signalGroup" },
            { "tlcfiName": "x", "cifName": "X", "type": "detector" }
        ]
    })");
    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);
    EXPECT_EQ(options.manuallyCoupledItems.signalGroups.size(), 1U);
    EXPECT_EQ(options.manuallyCoupledItems.detectors.size(), 1U);
}

TEST(RuntimeConfig, RejectsNameInBothManualAndControllerOnly) {
    TempConfig config(R"({
        "controllerOnlyItems": [ { "name": "X", "type": "signalGroup" } ],
        "manuallyCoupledItems": [ { "tlcfiName": "a", "cifName": "X", "type": "signalGroup" } ]
    })");
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
                 std::runtime_error);
}

// =============================================================================
// PTP links (ptpLinks)
// =============================================================================

TEST(RuntimeConfig, AbsentPtpLinksLeavesEmpty) {
    TempConfig config(R"({"host": "localhost"})");
    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);
    EXPECT_TRUE(options.ptpLinks.empty());
}

TEST(RuntimeConfig, LoadsPtpLinks) {
    TempConfig config(R"({
        "ptpLinks": [
            { "linkId": 1, "mode": "connect", "peerIp": "10.0.0.2", "tcpPort": 7100 },
            { "linkId": 2, "mode": "listen",  "peerIp": "10.0.0.5" }
        ]
    })");
    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);

    ASSERT_EQ(options.ptpLinks.size(), 2U);
    EXPECT_EQ(options.ptpLinks[0].linkId, 1);
    EXPECT_EQ(options.ptpLinks[0].mode, cif::ptp::Mode::Connect);
    EXPECT_EQ(options.ptpLinks[0].peerIp, "10.0.0.2");
    EXPECT_EQ(options.ptpLinks[0].tcpPort, 7100);
    EXPECT_EQ(options.ptpLinks[1].linkId, 2);
    EXPECT_EQ(options.ptpLinks[1].mode, cif::ptp::Mode::Listen);
    // tcpPort omitted -> defaults to 7100.
    EXPECT_EQ(options.ptpLinks[1].tcpPort, cif::ptp::DEFAULT_TCP_PORT);
}

TEST(RuntimeConfig, RejectsPtpLinkMissingRequiredField) {
    TempConfig config(R"({
        "ptpLinks": [ { "linkId": 1, "mode": "connect" } ]
    })");
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
                 std::runtime_error);
}

TEST(RuntimeConfig, RejectsPtpLinkInvalidMode) {
    TempConfig config(R"({
        "ptpLinks": [ { "linkId": 1, "mode": "master", "peerIp": "10.0.0.2" } ]
    })");
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
                 std::runtime_error);
}

TEST(RuntimeConfig, RejectsPtpLinkInvalidPeerIp) {
    TempConfig config(R"({
        "ptpLinks": [ { "linkId": 1, "mode": "connect", "peerIp": "not-an-ip" } ]
    })");
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
                 std::runtime_error);
}

TEST(RuntimeConfig, RejectsTooManyPtpLinks) {
    TempConfig config(R"({
        "ptpLinks": [
            { "linkId": 1, "mode": "connect", "peerIp": "10.0.0.1" },
            { "linkId": 2, "mode": "connect", "peerIp": "10.0.0.2" },
            { "linkId": 3, "mode": "connect", "peerIp": "10.0.0.3" },
            { "linkId": 4, "mode": "connect", "peerIp": "10.0.0.4" },
            { "linkId": 5, "mode": "connect", "peerIp": "10.0.0.5" }
        ]
    })");
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
                 std::runtime_error);
}

TEST(RuntimeConfig, RejectsDuplicatePtpLinkId) {
    TempConfig config(R"({
        "ptpLinks": [
            { "linkId": 1, "mode": "connect", "peerIp": "10.0.0.1" },
            { "linkId": 1, "mode": "listen",  "peerIp": "10.0.0.2" }
        ]
    })");
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
                 std::runtime_error);
}

TEST(RuntimeConfig, AllowsArbitraryLinkId) {
    // linkId is engineer-chosen (= the controller's PTP->PORTNR), not a 1..4
    // slot; any value, including 0 or a large id, is accepted.
    TempConfig config(R"({
        "ptpLinks": [
            { "linkId": 0,    "mode": "connect", "peerIp": "10.0.0.1" },
            { "linkId": 7033, "mode": "connect", "peerIp": "10.0.0.2" }
        ]
    })");
    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);
    ASSERT_EQ(options.ptpLinks.size(), 2U);
    EXPECT_EQ(options.ptpLinks[0].linkId, 0);
    EXPECT_EQ(options.ptpLinks[1].linkId, 7033);
}

TEST(RuntimeConfig, RejectsAmbiguousListenDemux) {
    // Two listen links sharing peerIp + port cannot be demultiplexed.
    TempConfig config(R"({
        "ptpLinks": [
            { "linkId": 1, "mode": "listen", "peerIp": "10.0.0.5", "tcpPort": 7100 },
            { "linkId": 2, "mode": "listen", "peerIp": "10.0.0.5", "tcpPort": 7100 }
        ]
    })");
    tlcfi::runtime::RuntimeOptions options;
    EXPECT_THROW(tlcfi::runtime::loadTlcfiConfig(config.path().string(), options),
                 std::runtime_error);
}

TEST(RuntimeConfig, AllowsTwoListenLinksWithDistinctPeers) {
    // Same port, different peer IPs -> demuxable, allowed.
    TempConfig config(R"({
        "ptpLinks": [
            { "linkId": 1, "mode": "listen", "peerIp": "10.0.0.5", "tcpPort": 7100 },
            { "linkId": 2, "mode": "listen", "peerIp": "10.0.0.6", "tcpPort": 7100 }
        ]
    })");
    tlcfi::runtime::RuntimeOptions options;
    tlcfi::runtime::loadTlcfiConfig(config.path().string(), options);
    EXPECT_EQ(options.ptpLinks.size(), 2U);
}
