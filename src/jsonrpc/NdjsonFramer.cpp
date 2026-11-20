#include "NdjsonFramer.hpp"

namespace tlcfi::jsonrpc {

NdjsonFramer::NdjsonFramer(std::size_t maxMessageSize)
	: maxMessageSize_(maxMessageSize) {
}

void NdjsonFramer::onBytesReceived(const std::uint8_t* data, std::size_t size,
	                                const std::function<void(const std::string&)>& onMessage,
	                                const std::function<void(const std::string&)>& onError) {
	buffer_.append(reinterpret_cast<const char*>(data), size);
	for (;;) {
		auto pos = buffer_.find('\n');
		if (pos == std::string::npos) {
			// No complete line yet. Guard against an endless line with no
			// newline causing unbounded buffer growth.
			if (buffer_.size() > maxMessageSize_) {
				buffer_.clear();
				if (onError) onError("NDJSON line exceeds maximum message size");
			}
			return;
		}
		std::string line = buffer_.substr(0, pos);
		buffer_.erase(0, pos + 1);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!line.empty()) onMessage(line);
	}
}

std::vector<std::uint8_t> NdjsonFramer::encode(const std::string& jsonText) {
	std::vector<std::uint8_t> out;
	out.reserve(jsonText.size() + 1);
	out.insert(out.end(), jsonText.begin(), jsonText.end());
	out.push_back('\n');
	return out;
}

} // namespace tlcfi::jsonrpc
