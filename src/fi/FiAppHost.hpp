#pragma once

#include "fi/FiConsumerApp.hpp"
#include "fi/LogLevel.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <string>

namespace tlcfi::fi {

/**
 * @brief Application host wrapper for FiConsumerApp with automatic reconnection
 * 
 * This class provides a high-level wrapper around FiConsumerApp that handles:
 * - Automatic connection retry with exponential backoff
 * - Full initialization sequence (connect → register → readMeta → subscribeAll)
 * - Disconnection monitoring and automatic reconnection
 * - Configurable logging with trace/info/error levels
 * - Default callback implementations that use the logging system
 * 
 * The host manages the complete lifecycle of the application, automatically
 * retrying failed operations and reconnecting after disconnections. Applications
 * can customize behavior through configuration and logging callbacks.
 * 
 * @note This class is thread-safe for concurrent method calls.
 */
class FiAppHost {
public:
	/**
	 * @brief Configuration structure for FiAppHost
	 */
	struct Config {
		std::chrono::milliseconds connectTimeout;        ///< Timeout for connection attempts
		std::chrono::milliseconds operationTimeout;      ///< Timeout for operations (register, readMeta, subscribe)
		std::chrono::milliseconds initialRetryDelay;      ///< Initial retry delay (exponential backoff start)
		std::chrono::milliseconds maxRetryDelay;         ///< Maximum retry delay (exponential backoff cap)
		bool enableDefaultCallbacks;                      ///< Enable default callback implementations with logging
		
		/// Default constructor with sensible defaults
		Config()
			: connectTimeout(5000)
			, operationTimeout(5000)
			, initialRetryDelay(1000)
			, maxRetryDelay(30000)
			, enableDefaultCallbacks(true)
		{}
	};

	/**
	 * @brief Constructs an application host for the given FiConsumerApp.
	 * 
	 * @param app Reference to the FiConsumerApp to host (must remain valid for lifetime of host).
	 * @param config Configuration for connection retry and timeouts.
	 */
	FiAppHost(FiConsumerApp& app, const Config& config = Config{});

	/**
	 * @brief Destructor - stops the application if still running.
	 */
	~FiAppHost();

	/**
	 * @brief Runs the connection retry loop until shouldRun becomes false.
	 * 
	 * This method continuously attempts to connect and initialize the application,
	 * automatically retrying on failures and reconnecting after disconnections.
	 * 
	 * @param shouldRun Atomic flag that controls the loop. When set to false, the loop exits.
	 */
	void run(std::atomic<bool>& shouldRun);

	/**
	 * @brief Logging callback function type.
	 * 
	 * Applications can set this callback to customize logging behavior.
	 * The callback receives the log level and message, and can decide
	 * what to do with it (print, filter, route to file, etc.).
	 * 
	 * @param level Log level (Trace, Info, Error)
	 * @param message Log message
	 */
	using LogCallback = std::function<void(LogLevel level, const std::string& message)>;

	/// @brief Logging callback - invoked for all log messages
	LogCallback onLog;

private:
	/**
	 * @brief Attempts to establish connection and perform full initialization.
	 * 
	 * Performs: connect() → registerApp() → readMeta() → subscribeAll()
	 * 
	 * @return true if all steps succeed, false otherwise.
	 */
	bool attemptConnection();

	/**
	 * @brief Waits for the specified delay, checking shouldRun periodically.
	 * 
	 * @param delay Delay duration
	 * @param shouldRun Flag to check for early exit
	 */
	void waitWithBackoff(std::chrono::milliseconds delay, std::atomic<bool>& shouldRun);

	/**
	 * @brief Logs a message using the configured logging callback.
	 * 
	 * @param level Log level
	 * @param message Log message
	 */
	void log(LogLevel level, const std::string& message);

	/**
	 * @brief Sets up default callbacks on the hosted app that use the logging system.
	 */
	void setupDefaultCallbacks();

	FiConsumerApp& app_;           ///< Reference to the hosted application
	Config config_;                 ///< Configuration
	std::chrono::milliseconds currentRetryDelay_;  ///< Current retry delay (for exponential backoff)
	bool firstConnection_;          ///< Flag to track if this is the first successful connection
};

} // namespace tlcfi::fi

