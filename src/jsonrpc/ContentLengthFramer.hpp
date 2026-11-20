#pragma once

#include "IMessageFramer.hpp"

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace tlcfi::jsonrpc {

/**
 * @brief Message framer implementing Content-Length header framing for JSON-RPC over TCP.
 * 
 * This framer implements the framing protocol commonly used by JSON-RPC implementations
 * over TCP, where each message is prefixed with HTTP-like headers containing a
 * Content-Length field. The format is:
 * 
 *   Content-Length: N\r\n\r\n<json-body>
 * 
 * The framer parses the header to determine the body length, then extracts exactly
 * that many bytes as the JSON message. This approach allows for efficient parsing
 * and supports messages of any size.
 * 
 * @note This framer expects exactly one header: Content-Length. Other headers are
 *       ignored. The header parsing is case-sensitive for simplicity.
 */
class ContentLengthFramer final : public IMessageFramer {
public:
	/**
	 * @brief Constructs the framer with an optional message-size limit.
	 *
	 * @param maxMessageSize Maximum accepted Content-Length (and maximum size of
	 *        the header block). A larger declared length, or a header block that
	 *        grows past this without terminating, is rejected via onError.
	 *        Guards against memory-exhaustion DoS.
	 */
	explicit ContentLengthFramer(std::size_t maxMessageSize = kDefaultMaxMessageSize);

	/**
	 * @brief Processes incoming bytes and extracts messages using Content-Length headers.
	 * 
	 * Accumulates bytes and parses the Content-Length header to determine message
	 * boundaries. When a complete message is detected (header + body), invokes
	 * onMessage with the JSON body. If the header is missing or malformed, invokes
	 * onError and clears the buffer.
	 * 
	 * @param data Pointer to the incoming byte data.
	 * @param size Number of bytes in the data buffer.
	 * @param onMessage Callback invoked with complete JSON text when a message is detected.
	 * @param onError Callback invoked if the Content-Length header is missing or invalid.
	 */
	void onBytesReceived(const std::uint8_t* data, std::size_t size,
	                    const std::function<void(const std::string&)>& onMessage,
	                    const std::function<void(const std::string&)>& onError) override;

	/**
	 * @brief Encodes JSON text with a Content-Length header.
	 * 
	 * Prepends a Content-Length header to the JSON text in the format:
	 * "Content-Length: N\r\n\r\n" followed by the JSON body.
	 * 
	 * @param jsonText The JSON text to encode.
	 * @return A byte vector containing the framed message with header and body.
	 */
	std::vector<std::uint8_t> encode(const std::string& jsonText) override;

private:
	std::string buffer_;              ///< Accumulated bytes waiting for complete message.
	bool inHeaders_ {true};           ///< Flag indicating if currently parsing headers.
	std::size_t contentLength_ {0};   ///< Content-Length value parsed from headers.
	std::size_t maxMessageSize_;      ///< Max accepted Content-Length / header block size.
};

} // namespace tlcfi::jsonrpc


