#pragma once

#include "ITransport.hpp"
#include "TlsConfig.hpp"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>
#include <asio/steady_timer.hpp>

#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <system_error>

namespace tlcfi::tcp {

/**
 * @brief TCP transport implementation for client-side connections.
 * 
 * This class provides asynchronous bidirectional communication over a TCP socket
 * as a client. It connects to a remote host and port, then manages read/write
 * operations with proper queueing for writes to ensure thread-safety.
 * 
 * The transport reports connection failures and disconnections via callbacks.
 * Reconnection and re-initialization should be handled at the application level.
 * 
 * @note This class must be managed via std::shared_ptr due to enable_shared_from_this.
 *       The transport must be started with start() after construction to initiate
 *       the connection.
 */
class TcpClientTransport : public ITransport, public std::enable_shared_from_this<TcpClientTransport> {
public:
	static constexpr std::size_t DEFAULT_MAX_PENDING_WRITE_BYTES = 32 * 1024 * 1024;

	/**
	 * @brief Constructs a client transport for the specified host and port.
	 * 
	 * @param io The ASIO I/O context for asynchronous operations.
	 * @param host The hostname or IP address to connect to.
	 * @param port The port number or service name to connect to.
	 * @param tlsConfig Optional TLS configuration. If not provided or disabled, plain TCP is used.
	 * @param maxPendingWriteBytes Maximum total bytes awaiting write completion.
	 */
	TcpClientTransport(asio::io_context& io, std::string host, std::string port,
	                   const TlsConfig& tlsConfig = TlsConfig{},
	                   std::size_t maxPendingWriteBytes = DEFAULT_MAX_PENDING_WRITE_BYTES);

	/**
	 * @brief Starts the transport and initiates connection to the remote host.
	 * 
	 * Begins the asynchronous connection process. The transport will resolve
	 * the hostname, connect to the server, perform TLS handshake if enabled,
	 * and then start reading. The onConnected callback is invoked when the
	 * connection is established and ready.
	 */
	void start() override;

	/**
	 * @brief Stops the transport and closes the connection.
	 * 
	 * Cancels any pending operations, closes the socket on its I/O executor, and
	 * invokes the onDisconnected callback if set. Pending write operations may
	 * be cancelled. Safe to call from any thread while async I/O is in flight.
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
	 * @return The string "tcp-client" or "tls-client" if TLS is enabled.
	 */
	std::string name() const override;

private:
	/**
	 * @brief Initiates an asynchronous connection to the remote host.
	 * 
	 * Resolves the hostname and port, then attempts to connect. On success,
	 * invokes the onConnected callback and starts reading. On failure,
	 * invokes the onError callback.
	 */
	void doConnect();

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
	 * @brief Performs the TLS handshake (client-side).
	 * 
	 * Called automatically by doConnect() if TLS is enabled after TCP connection
	 * succeeds. Invokes onConnected on success, or onError/onDisconnected on failure.
	 */
	void doHandshake();

private:
	asio::io_context& io_;                              ///< Reference to the I/O context for async operations.
	asio::ip::tcp::resolver resolver_;                  ///< DNS resolver for hostname resolution.
	asio::ip::tcp::socket socket_;                      ///< The TCP socket for communication.
	std::unique_ptr<asio::ssl::context> sslContext_;    ///< SSL context for TLS (null if TLS disabled).
	std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket&>> sslStream_;  ///< SSL stream wrapper (null if TLS disabled).
	std::string host_;                                   ///< The hostname or IP address to connect to.
	std::string port_;                                   ///< The port number or service name to connect to.
	bool verifyPeer_ {false};                            ///< Whether to verify the server certificate (incl. its identity).
	std::string expectedPeerName_;                       ///< Override identity to verify against; empty = use host_.
	std::mutex writeMutex_;                              ///< Mutex protecting the write queue.
	std::deque<std::vector<std::uint8_t>> writeQueue_;   ///< Queue of pending write operations.
	std::size_t pendingWriteBytes_ {0};                  ///< Bytes queued or owned by an active write.
	const std::size_t maxPendingWriteBytes_;             ///< Hard bound on pending write memory.
	bool writeInProgress_ {false};                       ///< Whether one async write currently owns the stream.
	bool writeQueueOverflowed_ {false};                  ///< Ensures overflow disconnect is scheduled once.
	std::array<std::uint8_t, 8192> readBuf_{};           ///< Buffer for incoming data (8KB).
	std::atomic<bool> stopping_ {false};                 ///< Flag indicating if the transport is stopping.
	std::atomic<bool> connected_ {false};                ///< Flag indicating if the transport is connected.
	int lastVerifyError_ {0};                            ///< Last certificate verification error code (if any).
	std::string lastVerifyCertSubject_;                  ///< Subject of certificate that failed verification.
};

} // namespace tlcfi::tcp


