#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tlcfi::jsonrpc {

/// Default maximum size (bytes) of a single framed message. Guards against
/// memory-exhaustion DoS from an unterminated or oversized message.
inline constexpr std::size_t kDefaultMaxMessageSize = 16u * 1024u * 1024u;

/// Default maximum JSON nesting depth accepted by structural framers.
inline constexpr std::size_t kDefaultMaxFramerDepth = 256u;

/**
 * @brief Interface for framing byte streams into complete JSON messages.
 * 
 * This abstract interface defines the contract for message framing implementations
 * that handle the conversion between raw byte streams and complete JSON text messages.
 * Different framing strategies (e.g., Content-Length headers, newline-delimited,
 * balanced-brace parsing) can be implemented to support various JSON-RPC transport
 * protocols.
 * 
 * Implementations accumulate incoming bytes and detect complete messages based on
 * their framing rules, invoking callbacks when a complete message is detected.
 * They also encode outbound JSON text into the appropriate framed format.
 * 
 * @note Implementations must handle partial messages and buffer appropriately.
 *       The onBytesReceived method may be called multiple times before a complete
 *       message is detected, and may invoke onMessage multiple times if multiple
 *       complete messages are present in the received data.
 */
class IMessageFramer {
public:
	/**
	 * @brief Virtual destructor for proper polymorphic destruction.
	 */
	virtual ~IMessageFramer() = default;

	/**
	 * @brief Processes incoming bytes and extracts complete JSON messages.
	 * 
	 * Accumulates the provided bytes and attempts to extract one or more complete
	 * JSON messages according to the framing strategy. When a complete message is
	 * detected, invokes the onMessage callback with the JSON text. If a framing
	 * error occurs, invokes the onError callback with an error description.
	 * 
	 * Implementations must buffer partial messages internally and may invoke
	 * onMessage multiple times if multiple complete messages are present in
	 * a single call.
	 * 
	 * @param data Pointer to the incoming byte data.
	 * @param size Number of bytes in the data buffer.
	 * @param onMessage Callback invoked with complete JSON text when a message is detected.
	 * @param onError Callback invoked with an error string if framing fails.
	 */
	virtual void onBytesReceived(const std::uint8_t* data, std::size_t size,
	                            const std::function<void(const std::string&)>& onMessage,
	                            const std::function<void(const std::string&)>& onError) = 0;

	/**
	 * @brief Encodes a JSON text string into a framed byte buffer.
	 * 
	 * Converts a complete JSON text string into the appropriate framed format
	 * for transmission over the transport. The framing format (headers, delimiters,
	 * etc.) depends on the implementation.
	 * 
	 * @param jsonText The complete JSON text to encode.
	 * @return A byte vector containing the framed message ready for transmission.
	 */
	virtual std::vector<std::uint8_t> encode(const std::string& jsonText) = 0;
};

} // namespace tlcfi::jsonrpc


