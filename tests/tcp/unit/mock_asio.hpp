#pragma once

#include <asio/buffer.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/steady_timer.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

namespace test_helpers::tcp::unit {

/**
 * @brief Mock socket that simulates ASIO socket behavior with in-memory buffers.
 * 
 * This mock provides the same interface as asio::ip::tcp::socket for testing purposes.
 * It uses in-memory buffers to simulate network communication without actual sockets.
 */
class MockSocket {
public:
	explicit MockSocket(asio::io_context& io) : io_(io), connected_(false), closed_(false) {}

	/**
	 * @brief Simulates async_read_some operation.
	 * 
	 * Reads data from the read buffer and invokes the completion handler.
	 * If no data is available, the handler is invoked with an error or empty read.
	 */
	template<typename MutableBufferSequence, typename ReadHandler>
	void async_read_some(const MutableBufferSequence& buffers, ReadHandler&& handler) {
		asio::post(io_, [this, buffers, handler = std::forward<ReadHandler>(handler)]() mutable {
			std::lock_guard<std::mutex> lock(mutex_);
			
			if (closed_ || !connected_) {
				std::error_code ec = closed_ ? std::make_error_code(std::errc::connection_aborted) 
				                            : std::make_error_code(std::errc::not_connected);
				handler(ec, 0);
				return;
			}

			if (readError_) {
				auto err = readError_;
				readError_ = std::error_code{};
				handler(err, 0);
				return;
			}

			if (readBuffer_.empty()) {
				// No data available - simulate would-block or EOF
				std::error_code ec = simulateReadBlock_ ? std::make_error_code(std::errc::operation_would_block)
				                                        : std::make_error_code(std::errc::broken_pipe);
				handler(ec, 0);
				return;
			}

			// Copy data from read buffer to the provided buffer
			std::size_t totalRead = 0;
			auto bufIt = asio::buffer_sequence_begin(buffers);
			auto bufEnd = asio::buffer_sequence_end(buffers);
			
			for (; bufIt != bufEnd && !readBuffer_.empty(); ++bufIt) {
				auto buf = *bufIt;
				std::size_t bufSize = asio::buffer_size(buf);
				void* bufData = asio::buffer_cast<void*>(buf);
				
				std::size_t toRead = std::min(bufSize, readBuffer_.size());
				std::memcpy(bufData, readBuffer_.data(), toRead);
				readBuffer_.erase(readBuffer_.begin(), readBuffer_.begin() + toRead);
				totalRead += toRead;
			}

			handler(std::error_code{}, totalRead);
		});
	}

	/**
	 * @brief Simulates async_write operation.
	 * 
	 * Writes data to the write buffer and invokes the completion handler.
	 */
	template<typename ConstBufferSequence, typename WriteHandler>
	void async_write(const ConstBufferSequence& buffers, WriteHandler&& handler) {
		asio::post(io_, [this, buffers, handler = std::forward<WriteHandler>(handler)]() mutable {
			std::lock_guard<std::mutex> lock(mutex_);
			
			if (closed_ || !connected_) {
				std::error_code ec = closed_ ? std::make_error_code(std::errc::connection_aborted)
				                            : std::make_error_code(std::errc::not_connected);
				handler(ec, 0);
				return;
			}

			if (writeError_) {
				auto err = writeError_;
				writeError_ = std::error_code{};
				handler(err, 0);
				return;
			}

			// Copy data from buffers to write buffer
			std::size_t totalWritten = 0;
			auto bufIt = asio::buffer_sequence_begin(buffers);
			auto bufEnd = asio::buffer_sequence_end(buffers);
			
			for (; bufIt != bufEnd; ++bufIt) {
				auto buf = *bufIt;
				std::size_t bufSize = asio::buffer_size(buf);
				const void* bufData = asio::buffer_cast<const void*>(buf);
				
				writeBuffer_.insert(writeBuffer_.end(),
				                   static_cast<const std::uint8_t*>(bufData),
				                   static_cast<const std::uint8_t*>(bufData) + bufSize);
				totalWritten += bufSize;
			}

			handler(std::error_code{}, totalWritten);
		});
	}

	/**
	 * @brief Simulates async_connect operation.
	 * 
	 * Sets connected state and invokes the completion handler.
	 */
	template<typename ConnectHandler>
	void async_connect(const asio::ip::tcp::resolver::results_type&, ConnectHandler&& handler) {
		asio::post(io_, [this, handler = std::forward<ConnectHandler>(handler)]() mutable {
			std::lock_guard<std::mutex> lock(mutex_);
			
			if (connectError_) {
				auto err = connectError_;
				connectError_ = std::error_code{};
				handler(err, asio::ip::tcp::endpoint{});
				return;
			}

			connected_ = true;
			closed_ = false;
			handler(std::error_code{}, asio::ip::tcp::endpoint{});
		});
	}

	/**
	 * @brief Cancels pending operations.
	 */
	void cancel(std::error_code& ec) {
		std::lock_guard<std::mutex> lock(mutex_);
		ec = std::error_code{};
		// In real socket, this cancels pending operations
		// For mock, we just clear the flag
	}

	/**
	 * @brief Closes the socket.
	 */
	void close(std::error_code& ec) {
		std::lock_guard<std::mutex> lock(mutex_);
		closed_ = true;
		connected_ = false;
		ec = std::error_code{};
	}

	void close() {
		std::error_code ec;
		close(ec);
	}

	// Test control methods
	void simulateConnect() {
		std::lock_guard<std::mutex> lock(mutex_);
		connected_ = true;
		closed_ = false;
	}

	void simulateDisconnect() {
		std::lock_guard<std::mutex> lock(mutex_);
		connected_ = false;
		closed_ = true;
	}

	void injectReadData(const std::vector<std::uint8_t>& data) {
		std::lock_guard<std::mutex> lock(mutex_);
		readBuffer_.insert(readBuffer_.end(), data.begin(), data.end());
	}

	void injectReadError(const std::error_code& ec) {
		std::lock_guard<std::mutex> lock(mutex_);
		readError_ = ec;
	}

	void injectWriteError(const std::error_code& ec) {
		std::lock_guard<std::mutex> lock(mutex_);
		writeError_ = ec;
	}

	void injectConnectError(const std::error_code& ec) {
		std::lock_guard<std::mutex> lock(mutex_);
		connectError_ = ec;
	}

	void setSimulateReadBlock(bool block) {
		std::lock_guard<std::mutex> lock(mutex_);
		simulateReadBlock_ = block;
	}

	std::vector<std::uint8_t> getWrittenData() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return writeBuffer_;
	}

	void clearWrittenData() {
		std::lock_guard<std::mutex> lock(mutex_);
		writeBuffer_.clear();
	}

	bool isConnected() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return connected_ && !closed_;
	}

	bool isClosed() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return closed_;
	}

private:
	asio::io_context& io_;
	mutable std::mutex mutex_;
	std::vector<std::uint8_t> readBuffer_;
	std::vector<std::uint8_t> writeBuffer_;
	bool connected_;
	bool closed_;
	std::error_code readError_;
	std::error_code writeError_;
	std::error_code connectError_;
	bool simulateReadBlock_{false};
};

/**
 * @brief Mock resolver that simulates DNS resolution.
 */
class MockResolver {
public:
	explicit MockResolver(asio::io_context& io) : io_(io) {}

	using results_type = asio::ip::tcp::resolver::results_type;

	template<typename ResolveHandler>
	void async_resolve(const std::string& host, const std::string& port, ResolveHandler&& handler) {
		asio::post(io_, [this, handler = std::forward<ResolveHandler>(handler)]() mutable {
			std::lock_guard<std::mutex> lock(mutex_);
			
			if (resolveError_) {
				auto err = resolveError_;
				resolveError_ = std::error_code{};
				results_type results;
				handler(err, results);
				return;
			}

			// Create mock endpoint results
			asio::ip::tcp::resolver::results_type results;
			asio::ip::tcp::endpoint ep(asio::ip::address::from_string("127.0.0.1"), 12345);
			results.push_back(ep);
			
			handler(std::error_code{}, results);
		});
	}

	void injectResolveError(const std::error_code& ec) {
		std::lock_guard<std::mutex> lock(mutex_);
		resolveError_ = ec;
	}

private:
	asio::io_context& io_;
	mutable std::mutex mutex_;
	std::error_code resolveError_;
};

/**
 * @brief Mock timer that executes callbacks immediately (no actual delay).
 */
class MockTimer {
public:
	explicit MockTimer(asio::io_context& io) : io_(io), cancelled_(false) {}

	template<typename Duration>
	void expires_after(const Duration&) {
		std::lock_guard<std::mutex> lock(mutex_);
		cancelled_ = false;
		// No-op for mock - we execute immediately
	}

	template<typename WaitHandler>
	void async_wait(WaitHandler&& handler) {
		asio::post(io_, [this, handler = std::forward<WaitHandler>(handler)]() mutable {
			std::lock_guard<std::mutex> lock(mutex_);
			
			if (cancelled_) {
				std::error_code ec = std::make_error_code(std::errc::operation_canceled);
				handler(ec);
				return;
			}

			if (timerError_) {
				auto err = timerError_;
				timerError_ = std::error_code{};
				handler(err);
				return;
			}

			// Execute immediately (no delay in mock)
			handler(std::error_code{});
		});
	}

	void cancel(std::error_code& ec) {
		std::lock_guard<std::mutex> lock(mutex_);
		cancelled_ = true;
		ec = std::error_code{};
	}

	void cancel() {
		std::error_code ec;
		cancel(ec);
	}

	void injectTimerError(const std::error_code& ec) {
		std::lock_guard<std::mutex> lock(mutex_);
		timerError_ = ec;
	}

private:
	asio::io_context& io_;
	mutable std::mutex mutex_;
	bool cancelled_;
	std::error_code timerError_;
};

/**
 * @brief Mock acceptor that simulates accepting connections.
 */
class MockAcceptor {
public:
	explicit MockAcceptor(asio::io_context& io, const asio::ip::tcp::endpoint&)
		: io_(io), closed_(false) {}

	template<typename AcceptHandler>
	void async_accept(asio::ip::tcp::socket& socket, AcceptHandler&& handler) {
		asio::post(io_, [this, &socket, handler = std::forward<AcceptHandler>(handler)]() mutable {
			std::lock_guard<std::mutex> lock(mutex_);
			
			if (closed_) {
				std::error_code ec = std::make_error_code(std::errc::operation_canceled);
				handler(ec);
				return;
			}

			if (acceptError_) {
				auto err = acceptError_;
				acceptError_ = std::error_code{};
				handler(err);
				return;
			}

			// Create a new mock socket for the accepted connection
			// Note: This is a simplified mock - in reality we'd need to handle socket creation differently
			// For now, we'll just signal success
			handler(std::error_code{});
		});
	}

	void close(std::error_code& ec) {
		std::lock_guard<std::mutex> lock(mutex_);
		closed_ = true;
		ec = std::error_code{};
	}

	void close() {
		std::error_code ec;
		close(ec);
	}

	void injectAcceptError(const std::error_code& ec) {
		std::lock_guard<std::mutex> lock(mutex_);
		acceptError_ = ec;
	}

	bool isClosed() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return closed_;
	}

private:
	asio::io_context& io_;
	mutable std::mutex mutex_;
	bool closed_;
	std::error_code acceptError_;
};

} // namespace test_helpers::tcp::unit

