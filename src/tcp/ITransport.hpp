#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace tlcfi::tcp {

/**
 * @brief Minimal asynchronous transport interface that delivers byte streams.
 * 
 * This abstract interface defines the contract for transport implementations that
 * provide asynchronous bidirectional byte stream communication. Implementations
 * handle the low-level details of connection management, reading, and writing,
 * while delivering events through callbacks.
 * 
 * @note Callbacks should be set before calling start(). The asyncSend() method
 *       must be thread-safe in all implementations.
 */
class ITransport {
public:
	/**
	 * @brief Virtual destructor for proper polymorphic destruction.
	 */
	virtual ~ITransport() = default;

	/**
	 * @brief Starts the transport and begins the connection lifecycle.
	 * 
	 * Initiates reading and connection establishment. The exact behavior depends
	 * on the implementation (e.g., connecting for clients, accepting for servers).
	 * The onConnected callback is invoked when the connection is ready.
	 */
	virtual void start() = 0;

	/**
	 * @brief Stops the transport and closes the connection.
	 * 
	 * Terminates the connection and stops all I/O operations. The onDisconnected
	 * callback may be invoked with a reason string.
	 */
	virtual void stop() = 0;

	/**
	 * @brief Enqueues bytes to send asynchronously.
	 * 
	 * Queues data for transmission. The implementation must be thread-safe and
	 * handle the actual sending asynchronously. The data pointer must remain
	 * valid until the send operation completes (implementation-dependent).
	 * 
	 * @param data Pointer to the data to send.
	 * @param size Number of bytes to send.
	 */
	virtual void asyncSend(const std::uint8_t* data, std::size_t size) = 0;

	/**
	 * @brief Returns a human-readable name for the transport.
	 * 
	 * Useful for logging and debugging purposes. The default implementation
	 * returns "transport", but implementations should override to provide
	 * a more specific identifier.
	 * 
	 * @return A string identifier for this transport instance.
	 */
	virtual std::string name() const { return "transport"; }

	/**
	 * @brief Callback invoked when the connection is established.
	 * 
	 * Set this callback before calling start(). It will be invoked once when
	 * the transport becomes ready for communication.
	 */
	std::function<void()> onConnected;

	/**
	 * @brief Callback invoked when data is received.
	 * 
	 * Set this callback before calling start(). It will be invoked whenever
	 * data arrives on the transport. The data pointer is valid only during
	 * the callback invocation.
	 * 
	 * @param data Pointer to the received data.
	 * @param size Number of bytes received.
	 */
	std::function<void(const std::uint8_t* data, std::size_t size)> onBytes;

	/**
	 * @brief Callback invoked when an error occurs.
	 * 
	 * Set this callback before calling start(). It will be invoked when
	 * a non-fatal error occurs during transport operations.
	 * 
	 * @param reason Description of the error.
	 */
	std::function<void(const std::string& reason)> onError;

	/**
	 * @brief Callback invoked when the connection is disconnected.
	 * 
	 * Set this callback before calling start(). It will be invoked when
	 * the connection is closed, either due to an error, remote closure,
	 * or explicit stop() call.
	 * 
	 * @param reason Description of why the connection was closed.
	 */
	std::function<void(const std::string& reason)> onDisconnected;
};

/**
 * @brief Type alias for a shared pointer to an ITransport instance.
 * 
 * Transport instances are typically managed via shared pointers to support
 * asynchronous operations and enable_shared_from_this patterns.
 */
using TransportPtr = std::shared_ptr<ITransport>;

} // namespace tlcfi::tcp


