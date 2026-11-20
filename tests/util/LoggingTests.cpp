#include <gtest/gtest.h>

#include "util/Logging.hpp"
#include "util/RollingFileStream.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace {

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p);
    return {std::istreambuf_iterator<char>(f), {}};
}

bool fileExists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

class TempLogFile {
public:
    TempLogFile() {
        std::mt19937_64 rng(std::random_device{}());
        path_ = std::filesystem::temp_directory_path()
                / ("logging_test_" + std::to_string(rng()) + ".log");
    }
    ~TempLogFile() {
        std::error_code ec;
        const std::string stem = path_.string();
        std::filesystem::remove(path_, ec);
        for (int i = 1; i <= 10; ++i) {
            std::filesystem::remove(
                std::filesystem::path(stem + "." + std::to_string(i)), ec);
        }
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

} // namespace

// =============================================================================
// makeTimestampedLogger — level filtering and output format
// =============================================================================

TEST(TimestampedLogger, MessagesAtOrAboveFileLevelAreWritten) {
    std::ostringstream out;
    auto log = tlcfi::makeTimestampedLogger(
        tlcfi::fi::LogLevel::Fatal,    // suppress console output during tests
        tlcfi::fi::LogLevel::Warning,
        &out);

    log(tlcfi::fi::LogLevel::Warning, "warn-msg");
    log(tlcfi::fi::LogLevel::Error,   "error-msg");
    log(tlcfi::fi::LogLevel::Fatal,   "fatal-msg");

    const auto content = out.str();
    EXPECT_NE(content.find("warn-msg"),  std::string::npos);
    EXPECT_NE(content.find("error-msg"), std::string::npos);
    EXPECT_NE(content.find("fatal-msg"), std::string::npos);
}

TEST(TimestampedLogger, MessagesBelowFileLevelAreNotWritten) {
    std::ostringstream out;
    auto log = tlcfi::makeTimestampedLogger(
        tlcfi::fi::LogLevel::Fatal,
        tlcfi::fi::LogLevel::Warning,
        &out);

    log(tlcfi::fi::LogLevel::Trace, "trace-msg");
    log(tlcfi::fi::LogLevel::Debug, "debug-msg");
    log(tlcfi::fi::LogLevel::Info,  "info-msg");

    EXPECT_TRUE(out.str().empty());
}

TEST(TimestampedLogger, NullFileOutDoesNotCrash) {
    auto log = tlcfi::makeTimestampedLogger(
        tlcfi::fi::LogLevel::Fatal,
        tlcfi::fi::LogLevel::Trace,
        nullptr);
    EXPECT_NO_THROW(log(tlcfi::fi::LogLevel::Info, "hello"));
}

TEST(TimestampedLogger, OutputContainsLevelTag) {
    using L = tlcfi::fi::LogLevel;
    std::ostringstream out;
    auto log = tlcfi::makeTimestampedLogger(L::Fatal, L::Trace, &out);

    log(L::Trace,   "t"); EXPECT_NE(out.str().find("[TRACE]"),   std::string::npos);
    log(L::Debug,   "d"); EXPECT_NE(out.str().find("[DEBUG]"),   std::string::npos);
    log(L::Info,    "i"); EXPECT_NE(out.str().find("[INFO]"),    std::string::npos);
    log(L::Warning, "w"); EXPECT_NE(out.str().find("[WARNING]"), std::string::npos);
    log(L::Error,   "e"); EXPECT_NE(out.str().find("[ERROR]"),   std::string::npos);
    log(L::Fatal,   "f"); EXPECT_NE(out.str().find("[FATAL]"),   std::string::npos);
}

TEST(TimestampedLogger, OutputContainsMessage) {
    std::ostringstream out;
    auto log = tlcfi::makeTimestampedLogger(
        tlcfi::fi::LogLevel::Fatal, tlcfi::fi::LogLevel::Trace, &out);

    log(tlcfi::fi::LogLevel::Info, "unique-marker-xyz");
    EXPECT_NE(out.str().find("unique-marker-xyz"), std::string::npos);
}

TEST(TimestampedLogger, TraceThreshold_AllLevelsWritten) {
    std::ostringstream out;
    auto log = tlcfi::makeTimestampedLogger(
        tlcfi::fi::LogLevel::Fatal, tlcfi::fi::LogLevel::Trace, &out);

    using L = tlcfi::fi::LogLevel;
    log(L::Trace,   "trace-msg");
    log(L::Debug,   "debug-msg");
    log(L::Info,    "info-msg");
    log(L::Warning, "warn-msg");
    log(L::Error,   "error-msg");
    log(L::Fatal,   "fatal-msg");

    const auto content = out.str();
    EXPECT_NE(content.find("trace-msg"), std::string::npos);
    EXPECT_NE(content.find("debug-msg"), std::string::npos);
    EXPECT_NE(content.find("info-msg"),  std::string::npos);
    EXPECT_NE(content.find("warn-msg"),  std::string::npos);
    EXPECT_NE(content.find("error-msg"), std::string::npos);
    EXPECT_NE(content.find("fatal-msg"), std::string::npos);
}

TEST(TimestampedLogger, FatalThreshold_OnlyFatalWritten) {
    std::ostringstream out;
    auto log = tlcfi::makeTimestampedLogger(
        tlcfi::fi::LogLevel::Fatal, tlcfi::fi::LogLevel::Fatal, &out);

    using L = tlcfi::fi::LogLevel;
    log(L::Trace,   "trace-msg");
    log(L::Debug,   "debug-msg");
    log(L::Info,    "info-msg");
    log(L::Warning, "warn-msg");
    log(L::Error,   "error-msg");
    EXPECT_TRUE(out.str().empty()) << "Nothing below Fatal should appear in the file sink";

    log(L::Fatal, "fatal-msg");
    EXPECT_NE(out.str().find("fatal-msg"), std::string::npos);
}

// =============================================================================
// RollingFileStream — file creation, rotation, backup management
// =============================================================================

TEST(RollingFileStream, OpenCreatesFile) {
    TempLogFile tmp;
    tlcfi::RollingFileStream stream;
    ASSERT_TRUE(stream.open(tmp.path(), 1024, 5));
    EXPECT_TRUE(stream.is_open());
    EXPECT_TRUE(fileExists(tmp.path()));
}

TEST(RollingFileStream, WritesBelowLimitDoNotRotate) {
    TempLogFile tmp;
    tlcfi::RollingFileStream stream;
    ASSERT_TRUE(stream.open(tmp.path(), 1024, 5));

    stream << "hello";
    stream.flush();
    stream.close();

    EXPECT_FALSE(fileExists(std::filesystem::path(tmp.path().string() + ".1")));
}

TEST(RollingFileStream, RotatesWhenSizeLimitExceeded) {
    TempLogFile tmp;
    tlcfi::RollingFileStream stream;
    ASSERT_TRUE(stream.open(tmp.path(), 5, 5));    // 5-byte limit

    stream << "1234567890";                         // 10 bytes — exceeds limit on first write
    stream.flush();
    stream.close();

    EXPECT_TRUE(fileExists(std::filesystem::path(tmp.path().string() + ".1")));
}

TEST(RollingFileStream, ZeroMaxSize_NeverRotates) {
    TempLogFile tmp;
    tlcfi::RollingFileStream stream;
    ASSERT_TRUE(stream.open(tmp.path(), 0, 5));    // 0 = unlimited

    for (int i = 0; i < 100; ++i)
        stream << "0123456789";
    stream.flush();
    stream.close();

    EXPECT_FALSE(fileExists(std::filesystem::path(tmp.path().string() + ".1")));
}

TEST(RollingFileStream, OldestBackupRemovedWhenMaxBackupsExceeded) {
    TempLogFile tmp;
    const std::string base = tmp.path().string();
    tlcfi::RollingFileStream stream;
    ASSERT_TRUE(stream.open(tmp.path(), 5, 2));    // 5-byte limit, keep 2 backups

    // Each write of 6 bytes (> 5 limit) triggers a rotation before writing.
    // After 3 rotations with maxBackups=2: .1 and .2 exist, .3 never exists.
    stream << "123456";
    stream << "123456";
    stream << "123456";
    stream.flush();
    stream.close();

    EXPECT_TRUE(fileExists(std::filesystem::path(base + ".1")));
    EXPECT_TRUE(fileExists(std::filesystem::path(base + ".2")));
    EXPECT_FALSE(fileExists(std::filesystem::path(base + ".3")));
}

TEST(RollingFileStream, CurrentFileContainsOnlyDataWrittenAfterRotation) {
    TempLogFile tmp;
    tlcfi::RollingFileStream stream;
    ASSERT_TRUE(stream.open(tmp.path(), 5, 3));

    stream << "123456";   // triggers rotation (6 > 5); "123456" written to fresh file
    stream << "NEW";      // triggers another rotation (6+3 > 5); "NEW" written to fresh file
    stream.flush();
    stream.close();

    EXPECT_EQ(readFile(tmp.path()), "NEW");
}
