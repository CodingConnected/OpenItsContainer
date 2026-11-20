#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <ostream>
#include <string>

namespace tlcfi {

/**
 * @brief std::streambuf that rotates to a new file when a size limit is reached.
 *
 * On each write, if currentSize + incomingBytes > maxSizeBytes, the file is
 * rotated before the write: backup files are shifted (.1 -> .2, ...,
 * .(N-1) -> .N, current -> .1, oldest backup deleted), then a fresh file is
 * opened. Opening in append mode preserves content from a previous run.
 *
 * All operations are protected by an internal mutex; the class is thread-safe.
 */
class RollingStreamBuf : public std::streambuf {
public:
    /**
     * @param path         Log file path.
     * @param maxSizeBytes Rotate when file reaches this size (0 = unlimited).
     * @param maxBackups   Number of backup files to keep (.1 .. .N).
     * @return true if the file was opened successfully.
     */
    bool open(const std::filesystem::path& path,
              std::uintmax_t maxSizeBytes,
              int maxBackups)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path_         = path;
        maxSizeBytes_ = maxSizeBytes;
        maxBackups_   = maxBackups;

        std::error_code ec;
        currentSize_ = std::filesystem::exists(path_, ec)
            ? std::filesystem::file_size(path_, ec)
            : 0;

        file_.open(path_, std::ios::out | std::ios::app | std::ios::binary);
        return file_.is_open();
    }

    bool is_open() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return file_.is_open();
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
    }

protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.is_open() || n <= 0) return 0;
        checkRotate(static_cast<std::uintmax_t>(n));
        file_.write(s, n);
        currentSize_ += static_cast<std::uintmax_t>(n);
        return n;
    }

    int_type overflow(int_type c) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.is_open() || c == traits_type::eof()) return traits_type::eof();
        checkRotate(1);
        file_.put(static_cast<char>(c));
        ++currentSize_;
        return c;
    }

    int sync() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.is_open()) return -1;
        file_.flush();
        return file_.good() ? 0 : -1;
    }

private:
    // Caller must hold mutex_.
    void checkRotate(std::uintmax_t incoming) {
        if (maxSizeBytes_ > 0 && currentSize_ + incoming > maxSizeBytes_) {
            rotate();
        }
    }

    // Caller must hold mutex_.
    void rotate() {
        file_.flush();
        file_.close();

        std::error_code ec;
        // Remove oldest backup
        std::filesystem::remove(
            std::filesystem::path(path_.string() + "." + std::to_string(maxBackups_)), ec);

        // Shift backups: .(N-1) -> .N, ..., .1 -> .2
        for (int i = maxBackups_ - 1; i >= 1; --i) {
            auto from = std::filesystem::path(path_.string() + "." + std::to_string(i));
            auto to   = std::filesystem::path(path_.string() + "." + std::to_string(i + 1));
            if (std::filesystem::exists(from, ec)) {
                std::filesystem::rename(from, to, ec);
            }
        }

        // Rename current file to .1
        if (std::filesystem::exists(path_, ec)) {
            std::filesystem::rename(
                path_,
                std::filesystem::path(path_.string() + ".1"), ec);
        }

        file_.open(path_, std::ios::out | std::ios::trunc | std::ios::binary);
        currentSize_ = 0;
    }

    mutable std::mutex mutex_;
    std::filesystem::path path_;
    std::ofstream file_;
    std::uintmax_t maxSizeBytes_ = 0;
    int maxBackups_              = 5;
    std::uintmax_t currentSize_  = 0;
};

/**
 * @brief std::ostream backed by a RollingStreamBuf.
 *
 * Drop-in replacement for std::ofstream wherever rolling log files are needed.
 * Inherits from std::ostream so it can be passed as std::ostream* / std::ostream&
 * without any changes at call sites.
 */
class RollingFileStream : public std::ostream {
public:
    RollingFileStream() : std::ostream(&buf_) {}

    /**
     * @param path         Log file path.
     * @param maxSizeBytes Rotate when file reaches this size in bytes (0 = unlimited).
     * @param maxBackups   Number of backup files to keep (default 5).
     * @return true if the file was opened successfully.
     */
    bool open(const std::filesystem::path& path,
              std::uintmax_t maxSizeBytes = 1ULL * 1024 * 1024,
              int maxBackups = 5)
    {
        return buf_.open(path, maxSizeBytes, maxBackups);
    }

    bool is_open() const { return buf_.is_open(); }
    void close()         { buf_.close(); }

private:
    RollingStreamBuf buf_;
};

} // namespace tlcfi
