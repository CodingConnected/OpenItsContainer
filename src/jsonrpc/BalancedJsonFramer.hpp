#pragma once

#include "IMessageFramer.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tlcfi::jsonrpc {

/**
 * @brief Message framer that detects complete JSON values using balanced brace/bracket counting.
 * 
 * This framer accumulates incoming bytes and detects complete top-level JSON values
 * (objects or arrays) by tracking brace and bracket depth. It properly handles
 * string literals and escape sequences to avoid false positives when braces appear
 * inside strings.
 * 
 * The framer emits a message when the depth reaches zero after starting at zero,
 * indicating a complete top-level JSON value. This approach is suitable for streams
 * that send raw JSON objects without explicit framing headers or delimiters.
 * 
 * @note This framer does not validate JSON syntax - it only detects structural
 *       completeness. Invalid JSON may still be emitted if it has balanced braces.
 *       The framer does not use the onError callback.
 */
class BalancedJsonFramer final : public IMessageFramer {
public:
	/**
	 * @brief Constructs the framer with optional resource limits.
	 *
	 * @param maxMessageSize Maximum number of bytes that may accumulate for a
	 *        single JSON value before the framer reports an error and discards
	 *        the buffer. Guards against memory-exhaustion DoS.
	 * @param maxDepth Maximum brace/bracket nesting depth accepted; deeper input
	 *        is rejected via onError.
	 */
	explicit BalancedJsonFramer(std::size_t maxMessageSize = kDefaultMaxMessageSize,
	                            std::size_t maxDepth = kDefaultMaxFramerDepth);

	/**
	 * @brief Processes incoming bytes and emits complete JSON values.
	 * 
	 * Accumulates bytes and tracks brace/bracket depth, emitting a complete JSON
	 * text when a top-level value is detected (depth returns to zero).
	 * 
	 * @param data Pointer to the incoming byte data.
	 * @param size Number of bytes in the data buffer.
	 * @param onMessage Callback invoked with complete JSON text when a value is detected.
	 * @param onError Unused in this implementation (may be nullptr).
	 */
	void onBytesReceived(const std::uint8_t* data, std::size_t size,
	                    const std::function<void(const std::string&)>& onMessage,
	                    const std::function<void(const std::string&)>& onError) override;

	/**
	 * @brief Encodes JSON text as-is without additional framing.
	 * 
	 * Returns the JSON text as a byte vector without adding any framing headers
	 * or delimiters, since the balanced-brace detection handles message boundaries.
	 * 
	 * @param jsonText The JSON text to encode.
	 * @return A byte vector containing the JSON text.
	 */
	std::vector<std::uint8_t> encode(const std::string& jsonText) override;

private:
	/// Discards all buffered bytes and resets parsing state to top-level.
	void resetState();

	std::string buffer_;        ///< Accumulated bytes waiting for complete JSON value.
	std::size_t depth_ {0};     ///< Current brace/bracket nesting depth (0 = top-level).
	bool inString_ {false};     ///< Flag indicating if currently inside a string literal.
	bool escape_ {false};       ///< Flag indicating if the next character is escaped.
	std::size_t start_ {0};     ///< Start position of the current top-level JSON value.
	std::size_t processed_ {0}; ///< Number of characters already processed in current buffer.
	std::size_t maxMessageSize_; ///< Max bytes for one JSON value before error.
	std::size_t maxDepth_;       ///< Max nesting depth before error.
};

} // namespace tlcfi::jsonrpc


