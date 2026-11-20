#pragma once

#include <asio/connect.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace test_helpers {

/**
 * @brief Helper to get the local port bound by an acceptor.
 */
inline unsigned short get_local_port(asio::ip::tcp::acceptor& acceptor) {
    std::error_code ec;
    auto endpoint = acceptor.local_endpoint(ec);
    if (ec) {
        return 0;
    }
    return endpoint.port();
}

/**
 * @brief Helper to get the local port bound by a socket.
 */
inline unsigned short get_local_port(asio::ip::tcp::socket& socket) {
    std::error_code ec;
    auto endpoint = socket.local_endpoint(ec);
    if (ec) {
        return 0;
    }
    return endpoint.port();
}

/**
 * @brief Helper class to run io_context in a background thread.
 * Automatically stops and joins on destruction.
 */
class IoContextRunner {
public:
    explicit IoContextRunner(asio::io_context& io) 
        : io_(io), work_guard_(asio::make_work_guard(io)), thread_([this]() {
            io_.run();
        }) {}

    ~IoContextRunner() {
        work_guard_.reset();  // Release work guard to allow io_.run() to return
        io_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Non-copyable, non-movable
    IoContextRunner(const IoContextRunner&) = delete;
    IoContextRunner& operator=(const IoContextRunner&) = delete;
    IoContextRunner(IoContextRunner&&) = delete;
    IoContextRunner& operator=(IoContextRunner&&) = delete;

private:
    asio::io_context& io_;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
    std::thread thread_;
};

/**
 * @brief Helper to wait for a condition with timeout.
 * Useful for waiting for async callbacks in tests.
 */
class AsyncWaiter {
public:
    void notify() {
        std::lock_guard<std::mutex> lock(mutex_);
        notified_ = true;
        cv_.notify_one();
    }

    bool wait_for(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this] { return notified_; });
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        notified_ = false;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool notified_{false};
};

/**
 * @brief Helper to create a simple TCP client socket for testing.
 * Connects to the specified host and port.
 */
inline bool connect_client(asio::io_context& io, const std::string& host, unsigned short port) {
    asio::ip::tcp::socket socket(io);
    asio::ip::tcp::resolver resolver(io);
    
    std::error_code ec;
    auto endpoints = resolver.resolve(host, std::to_string(port), ec);
    if (ec) {
        return false;
    }
    
    asio::connect(socket, endpoints, ec);
    return !ec;
}

} // namespace test_helpers

