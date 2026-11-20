#include "BalancedJsonFramer.hpp"

namespace tlcfi::jsonrpc {

BalancedJsonFramer::BalancedJsonFramer(std::size_t maxMessageSize, std::size_t maxDepth)
	: maxMessageSize_(maxMessageSize), maxDepth_(maxDepth) {
}

void BalancedJsonFramer::resetState() {
	buffer_.clear();
	depth_ = 0;
	inString_ = false;
	escape_ = false;
	start_ = 0;
	processed_ = 0;
}

void BalancedJsonFramer::onBytesReceived(const std::uint8_t* data, std::size_t size,
	                                     const std::function<void(const std::string&)>& onMessage,
	                                     const std::function<void(const std::string&)>& onError) {
	buffer_.append(reinterpret_cast<const char*>(data), size);

	// Start processing from where we left off (or 0 if depth is 0, meaning we're looking for a new JSON value)
	std::size_t startIdx = (depth_ == 0) ? 0 : processed_;

	for (std::size_t i = startIdx; i < buffer_.size(); ++i) {
		char c = buffer_[i];
		if (inString_) {
			if (escape_) { escape_ = false; continue; }
			if (c == '\\') { escape_ = true; continue; }
			if (c == '"') { inString_ = false; continue; }
			continue;
		}
		// not in string
		if (c == '"') { inString_ = true; continue; }
		if (c == '{' || c == '[') {
			if (depth_ >= maxDepth_) {
				// Reject pathologically nested input rather than overflowing depth_.
				resetState();
				if (onError) onError("JSON nesting depth exceeds maximum");
				return;
			}
			if (depth_ == 0) {
				start_ = i; // Mark start of new top-level JSON value
			}
			depth_++;
			continue;
		}
		if (c == '}' || c == ']') {
			if (depth_ == 0) {
				// Stray closing brace at top level — the stream is malformed
				// or de-synchronised; discard rather than underflowing depth_.
				resetState();
				if (onError) onError("Unbalanced closing brace in JSON stream");
				return;
			}
			depth_--;
			if (depth_ == 0) {
				// end of top-level JSON value
				std::string message = buffer_.substr(start_, i - start_ + 1);
				onMessage(message);
				// reset for next
				buffer_.erase(0, i + 1);
				processed_ = 0;
				start_ = 0;
				i = static_cast<std::size_t>(-1); // will become 0 after ++i
			}
		}
	}

	// Update processed_ to track how much of the buffer we've processed.
	// If we emitted a message, processed_ was reset above.
	if (depth_ > 0) {
		processed_ = buffer_.size();
	}

	// Guard against unbounded memory growth from an unterminated value (or a
	// flood of junk at top level). Applies regardless of depth.
	if (buffer_.size() > maxMessageSize_) {
		resetState();
		if (onError) onError("JSON message exceeds maximum size");
	}
}

std::vector<std::uint8_t> BalancedJsonFramer::encode(const std::string& jsonText) {
	std::vector<std::uint8_t> out(jsonText.begin(), jsonText.end());
	return out;
}

} // namespace tlcfi::jsonrpc
