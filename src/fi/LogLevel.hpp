#pragma once

#include <functional>
#include <string>

namespace tlcfi::fi {

/**
 * @brief Log level enumeration for application logging
 */
enum class LogLevel {
	Trace,  ///< Detailed trace information (e.g., state updates)
	Debug,  ///< Debug informational messages (e.g., internal debug messages)
	Info,   ///< General informational messages (e.g., connection status)
	Warning,  ///< Warning messages (e.g., potential issues)
	Error,  ///< Error messages (e.g., connection failures)
	Fatal   ///< Fatal messages (e.g., unrecoverable errors)
};

/// Logging callback type used by all SDK components.
using LogCallback = std::function<void(LogLevel, const std::string&)>;

} // namespace tlcfi::fi


