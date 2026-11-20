#pragma once

#include "ITransport.hpp"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <system_error>

namespace tlcfi::tcp {

/**
 * @brief TCP transport implementation for server-side connections.
 * 
 * This class provides asynchronous bidirectional communication over a TCP socket.
 * It implements the ITransport interface and manages read/write operations with
 * proper queueing for writes to ensure thread-safety.
 * 
 * @note This class must be managed via std::shared_ptr due to enable_shared_from_this.
 *       The transport must be started with start() after construction.
 */
class TcpServerTransport : public ITransport, public std::enable_shared_from_this<TcpServerTransport> {
public:
	static constexpr std::size_t DEFAULT_MAX_PENDING_WRITE_BYTES = 32 * 1024 * 1024;

	/**
	 * @brief Constructs a transport from an accepted TCP socket (plain TCP).
	 * 
	 * @param socket The accepted TCP socket (moved into the transport).
	 * @param maxPendingWriteBytes Maximum total bytes awaiting write completion.
	 */
	explicit TcpServerTransport(
		asio::ip::tcp::socket socket,
		std::size_t maxPendingWriteBytes = DEFAULT_MAX_PENDING_WRITE_BYTES);

	/**
	 * @brief Constructs a transport from an accepted TCP socket with TLS support.
	 * 
	 * @param socket The accepted TCP socket (moved into the transport).
	 * @param sslContext The SSL context for TLS encryption.
	 * @param maxPendingWriteBytes Maximum total bytes awaiting write completion.
	 */
	TcpServerTransport(
		asio::ip::tcp::socket socket,
		asio::ssl::context& sslContext,
		std::size_t maxPendingWriteBytes = DEFAULT_MAX_PENDING_WRITE_BYTES);

	/**
	 * @brief Starts the transport and begins reading from the socket.
	 * 
	 * If TLS is enabled, performs the SSL handshake first, then initiates
	 * the asynchronous read loop. Invokes the onConnected callback when ready.
	 * The transport will continue reading until stop() is called or the
	 * connection is closed.
	 */
	void start() override;

	/**
	 * @brief Stops the transport and closes the socket.
	 * 
	 * Closes the underlying socket and invokes the onDisconnected callback
	 * if set. Pending write operations may be cancelled.
	 */
	void stop() override;

	/**
	 * @brief Asynchronously sends data over the TCP connection.
	 * 
	 * Queues the data for sending. If a write operation is already in progress,
	 * the data is queued and will be sent when the current write completes.
	 * This method is thread-safe.
	 * 
	 * @param data Pointer to the data to send.
	 * @param size Number of bytes to send.
	 */
	void asyncSend(const std::uint8_t* data, std::size_t size) override;

	/**
	 * @brief Returns the transport name identifier.
	 * 
	 * @return The string "tcp-server-conn" or "tls-server-conn" if TLS is enabled.
	 */
	std::string name() const override;

	/**
	 * @brief Sets the server-internal close hook.
	 *
	 * Distinct from ITransport::onDisconnected (which is owned by the protocol
	 * consumer): this callback lets the owning TcpServer drop its reference to
	 * the transport so closed connections do not accumulate. It is invoked
	 * exactly once, on the first of stop(), a read/write failure, or a
	 * handshake failure. Thread-safe.
	 */
	void setOnClosed(std::function<void()> callback);

	/**
	 * @brief Clears the server-internal close hook.
	 *
	 * Called by the owning TcpServer during shutdown so a connection close
	 * racing with server destruction cannot invoke a stale callback.
	 * Thread-safe.
	 */
	void clearOnClosed();

private:
	/**
	 * @brief Notifies of connection closure: fires onDisconnected, and fires
	 *        the onClosed hook exactly once across the object's lifetime.
	 */
	void notifyClosed(const std::string& reason);

	/**
	 * @brief Initiates an asynchronous read operation.
	 * 
	 * Reads available data into the read buffer and invokes the onBytes callback
	 * with the received data. Continues reading until the connection is closed
	 * or stop() is called.
	 */
	void doRead();

	/**
	 * @brief Initiates an asynchronous write operation.
	 * 
	 * Writes the next queued data buffer to the socket. If more data is queued,
	 * continues writing until the queue is empty. Thread-safe via writeMutex_.
	 */
	void doWrite();

	/**
	 * @brief Completes the active write and starts the next queued write.
	 */
	void onWriteComplete(const std::error_code& error, std::size_t completedBytes);

	/**
	 * @brief Performs the TLS handshake (server-side).
	 * 
	 * Called automatically by start() if TLS is enabled. Invokes onConnected
	 * on success, or onError/onDisconnected on failure.
	 */
	void doHandshake();

private:
	asio::ip::tcp::socket socket_;                    ///< The TCP socket for communication.
	std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> sslStream_;  ///< SSL stream wrapper (null if TLS disabled).
	std::array<std::uint8_t, 8192> readBuf_{};        ///< Buffer for incoming data (8KB).
	std::mutex writeMutex_;                            ///< Mutex protecting the write queue.
	std::deque<std::vector<std::uint8_t>> writeQueue_; ///< Queue of pending write operations.
	std::size_t pendingWriteBytes_ {0};                ///< Bytes queued or owned by an active write.
	const std::size_t maxPendingWriteBytes_;           ///< Hard bound on pending write memory.
	bool writeInProgress_ {false};                     ///< Whether one async write currently owns the stream.
	bool writeQueueOverflowed_ {false};                ///< Ensures overflow disconnect is scheduled once.
	std::atomic<bool> stopped_ {false};                ///< Flag indicating if the transport is stopped.
	std::atomic<bool> closedNotified_ {false};         ///< Ensures close notification fires once.
	std::mutex onClosedMutex_;                         ///< Protects onClosed_ against concurrent set/clear/invoke.
	std::function<void()> onClosed_;                   ///< Server-internal close hook (see setOnClosed()).
};

} // namespace tlcfi::tcp


