#pragma once

#include "IMessageFramer.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tlcfi::jsonrpc {

/**
 * @brief Message framer implementing newline-delimited JSON (NDJSON) framing.
 * 
 * This framer implements the NDJSON format where each JSON message is terminated
 * by a newline character (LF, '\n'). Messages are separated by line breaks, making
 * this format suitable for streaming JSON data where each line is a complete
 * JSON value.
 * 
 * The framer splits incoming bytes on newline characters and emits each non-empty
 * line as a complete message. CRLF line endings are handled by stripping the
 * trailing '\r' character.
 * 
 * @note Empty lines are ignored. The framer does not validate JSON syntax - it
 *       only splits on newlines. The onError callback is unused.
 */
class NdjsonFramer final : public IMessageFramer {
public:
	/**
	 * @brief Constructs the framer with an optional message-size limit.
	 *
	 * @param maxMessageSize Maximum number of bytes that may accumulate before a
	 *        terminating newline is seen. If exceeded, the buffer is discarded
	 *        and onError is invoked. Guards against memory-exhaustion DoS.
	 */
	explicit NdjsonFramer(std::size_t maxMessageSize = kDefaultMaxMessageSize);

	/**
	 * @brief Processes incoming bytes and extracts newline-delimited messages.
	 * 
	 * Accumulates bytes and splits on newline characters ('\n'). Each non-empty
	 * line is emitted as a complete message. Handles CRLF line endings by
	 * stripping the trailing carriage return.
	 * 
	 * @param data Pointer to the incoming byte data.
	 * @param size Number of bytes in the data buffer.
	 * @param onMessage Callback invoked with complete JSON text for each line.
	 * @param onError Unused in this implementation (may be nullptr).
	 */
	void onBytesReceived(const std::uint8_t* data, std::size_t size,
	                    const std::function<void(const std::string&)>& onMessage,
	                    const std::function<void(const std::string&)>& onError) override;

	/**
	 * @brief Encodes JSON text with a trailing newline.
	 * 
	 * Appends a newline character ('\n') to the JSON text to create a properly
	 * formatted NDJSON message.
	 * 
	 * @param jsonText The JSON text to encode.
	 * @return A byte vector containing the JSON text followed by a newline.
	 */
	std::vector<std::uint8_t> encode(const std::string& jsonText) override;

private:
	std::string buffer_;          ///< Accumulated bytes waiting for complete line.
	std::size_t maxMessageSize_;  ///< Max bytes for one line before error.
};

} // namespace tlcfi::jsonrpc


