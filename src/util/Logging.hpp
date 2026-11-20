#pragma once

#include "fi/LogLevel.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <sstream>

namespace tlcfi {

/**
 * @brief Returns a LogCallback that writes timestamped, ANSI-colored lines to stdout
 *        and optionally mirrors output to a plain-text file stream.
 *
 * Console output is filtered to @p consoleLevel and above (ANSI-colored).
 * File output (if @p fileOut is non-null) is filtered to @p fileLevel and above
 * (plain text, no ANSI escape codes).
 *
 * Format: [YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] message
 *
 * Used as the default logger in RuntimeApp and the example applications.
 *
 * @param consoleLevel Minimum log level to print to stdout (default: Trace = everything).
 * @param fileLevel    Minimum log level to write to @p fileOut (default: Trace = everything).
 * @param fileOut      Optional output stream for plain-text log file (not owned, must outlive callback).
 */
inline fi::LogCallback makeTimestampedLogger(
    fi::LogLevel consoleLevel = fi::LogLevel::Trace,
    fi::LogLevel fileLevel    = fi::LogLevel::Trace,
    std::ostream* fileOut     = nullptr)
{
    return [consoleLevel, fileLevel, fileOut](fi::LogLevel level, const std::string& message) {
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
        std::stringstream ts;
        ts << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
           << "." << std::setfill('0') << std::setw(3) << ms;
        const std::string timestamp = ts.str();

        using L = fi::LogLevel;

        if (level >= consoleLevel) {
            std::cout << "\033[90m[" << timestamp << "] \033[0m";
            switch (level) {
                case L::Trace:   std::cout << "\033[38;5;245m[TRACE] "   << message << "\033[0m\n"; break;
                case L::Debug:   std::cout << "\033[38;5;255m[DEBUG] "   << message << "\033[0m\n"; break;
                case L::Info:    std::cout << "\033[38;5;32m[INFO] "     << message << "\033[0m\n"; break;
                case L::Warning: std::cout << "\033[38;5;226m[WARNING] " << message << "\033[0m\n"; break;
                case L::Error:   std::cout << "\033[38;5;196m[ERROR] "   << message << "\033[0m\n"; break;
                case L::Fatal:   std::cout << "\033[38;5;199m[FATAL] "   << message << "\033[0m\n"; break;
            }
        }

        if (fileOut && level >= fileLevel) {
            const char* levelStr = "UNKNOWN";
            switch (level) {
                case L::Trace:   levelStr = "TRACE";   break;
                case L::Debug:   levelStr = "DEBUG";   break;
                case L::Info:    levelStr = "INFO";    break;
                case L::Warning: levelStr = "WARNING"; break;
                case L::Error:   levelStr = "ERROR";   break;
                case L::Fatal:   levelStr = "FATAL";   break;
            }
            // Build the complete line before writing so the RollingStreamBuf
            // receives it as a single xsputn call — keeps log lines atomic under
            // concurrent writes from multiple threads.
            const std::string line =
                std::string("[") + timestamp + "] [" + levelStr + "] " + message + "\n";
            *fileOut << line;
            fileOut->flush();
        }
    };
}

} // namespace tlcfi
