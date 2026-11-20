#pragma once

#include "TcpServerTransport.hpp"
#include "TlsConfig.hpp"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ssl.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <set>

namespace tlcfi::tcp {

/// Default cap on simultaneously accepted connections. Guards against
/// connection-flood / slowloris resource-exhaustion attacks.
inline constexpr std::size_t kDefaultMaxConnections = 64;

/**
 * @brief TCP server that accepts incoming connections and creates transport instances.
 * 
 * This class manages a TCP acceptor that listens on a specified port and creates
 * TcpServerTransport instances for each accepted connection. The server uses
 * asynchronous I/O operations via ASIO.
 * 
 * @note The server must be started with start() after construction. Connections
 *       are handled through the onAccept callback.
 */
class TcpServer : public std::enable_shared_from_this<TcpServer> {
public:
	/**
	 * @brief Constructs a TCP server bound to the specified port.
	 * 
	 * @param io The ASIO I/O context for asynchronous operations.
	 * @param port The port number to listen on.
	 * @param tlsConfig Optional TLS configuration. If not provided or disabled, plain TCP is used.
	 * @param maxConnections Maximum number of simultaneously accepted connections.
	 *        Connections beyond this limit are immediately closed.
	 */
	TcpServer(asio::io_context& io, unsigned short port, const TlsConfig& tlsConfig = TlsConfig{},
	          std::size_t maxConnections = kDefaultMaxConnections);

	/**
	 * @brief Callback invoked when a new connection is accepted.
	 * 
	 * This callback receives a ready-to-use TcpServerTransport instance that
	 * has been created for the new connection. The transport is not started
	 * automatically - the callback should call start() on it when ready.
	 * 
	 * @param transport A shared pointer to the newly created transport instance.
	 */
	std::function<void(std::shared_ptr<TcpServerTransport>)> onAccept;

	/**
	 * @brief Starts accepting incoming connections.
	 * 
	 * Begins the asynchronous accept loop. The server will continue accepting
	 * connections until stop() is called.
	 */
	void start();

	/**
	 * @brief Stops accepting new connections.
	 * 
	 * Closes the acceptor and prevents new connections from being accepted.
	 * Also closes all existing active connections.
	 */
	void stop();

	/**
	 * @brief Returns the local TCP port the acceptor is bound to.
	 *
	 * Useful when the server is constructed with port 0, which asks the OS to
	 * assign a free ephemeral port: this returns the actual assigned port.
	 *
	 * @return The bound port, or 0 if the acceptor is not bound.
	 */
	unsigned short port() const {
		std::error_code ec;
		auto endpoint = acceptor_.local_endpoint(ec);
		return ec ? 0 : endpoint.port();
	}

	~TcpServer();

private:
	/**
	 * @brief Initiates an asynchronous accept operation.
	 * 
	 * When a connection is accepted, creates a TcpServerTransport instance
	 * and invokes the onAccept callback if set. Continues accepting if
	 * the server is not stopping.
	 */
	void doAccept();

	/**
	 * @brief Detaches each active transport's onClosed hook and stops it.
	 *
	 * Shared by stop() and the destructor.
	 */
	void stopTransports();

private:
	asio::io_context& io_;              ///< Reference to the I/O context for async operations.
	asio::ip::tcp::acceptor acceptor_;  ///< TCP acceptor for incoming connections.
	std::unique_ptr<asio::ssl::context> sslContext_;  ///< SSL context for TLS (null if TLS disabled).
	std::atomic<bool> stopping_ {false}; ///< Set when shutting down; read on the io thread, written by stop()/~TcpServer.
	std::size_t maxConnections_;        ///< Cap on simultaneously accepted connections.
	std::mutex transportsMutex_;        ///< Mutex protecting the active transports set.
	std::set<std::shared_ptr<TcpServerTransport>> activeTransports_;  ///< Set of active transport connections.
};

} // namespace tlcfi::tcp


