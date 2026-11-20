#include "ContentLengthFramer.hpp"

#include <algorithm>
#include <charconv>

using namespace std::string_literals;

namespace tlcfi::jsonrpc {

namespace {
constexpr const char* CL = "Content-Length:";
}

ContentLengthFramer::ContentLengthFramer(std::size_t maxMessageSize)
	: maxMessageSize_(maxMessageSize) {
}

void ContentLengthFramer::onBytesReceived(const std::uint8_t* data, std::size_t size,
	                                        const std::function<void(const std::string&)>& onMessage,
	                                        const std::function<void(const std::string&)>& onError) {
	buffer_.append(reinterpret_cast<const char*>(data), size);

	while (true) {
		if (inHeaders_) {
			// Look for end of headers \r\n\r\n
			auto pos = buffer_.find("\r\n\r\n");
			if (pos == std::string::npos) {
				// Header block not yet terminated. Guard against a peer that
				// streams headers endlessly without the \r\n\r\n terminator.
				if (buffer_.size() > maxMessageSize_) {
					onError("Header block exceeds maximum size");
					buffer_.clear();
				}
				return; // wait for more bytes
			}

			// Parse headers block
			std::string headers = buffer_.substr(0, pos);
			buffer_.erase(0, pos + 4);

			// Find Content-Length (case-sensitive for simplicity here)
			auto clPos = headers.find(CL);
			if (clPos == std::string::npos) {
				onError("Missing Content-Length header");
				buffer_.clear();
				return;
			}
			clPos += std::char_traits<char>::length(CL);
			// Skip spaces
			while (clPos < headers.size() && (headers[clPos] == ' ' || headers[clPos] == '\t')) clPos++;
			// Read until end of line
			auto endLine = headers.find('\n', clPos);
			std::string lenStr = (endLine == std::string::npos) ? headers.substr(clPos) : headers.substr(clPos, endLine - clPos);
			// Trim CR
			if (!lenStr.empty() && lenStr.back() == '\r') lenStr.pop_back();
			// Parse number
			contentLength_ = 0;
			auto res = std::from_chars(lenStr.data(), lenStr.data() + lenStr.size(), contentLength_);
			if (res.ec != std::errc()) {
				onError("Invalid Content-Length value");
				buffer_.clear();
				contentLength_ = 0;
				return;
			}
			// Reject an oversized declared body before waiting to buffer it,
			// preventing memory-exhaustion DoS from a huge Content-Length.
			if (contentLength_ > maxMessageSize_) {
				onError("Content-Length exceeds maximum message size");
				buffer_.clear();
				contentLength_ = 0;
				inHeaders_ = true;
				return;
			}
			inHeaders_ = false;
		}

		if (!inHeaders_) {
			if (buffer_.size() < contentLength_) {
				return; // wait for body
			}
			std::string body = buffer_.substr(0, contentLength_);
			buffer_.erase(0, contentLength_);
			inHeaders_ = true;
			contentLength_ = 0;
			onMessage(body);
			// loop to check if more messages are buffered
		}
	}
}

std::vector<std::uint8_t> ContentLengthFramer::encode(const std::string& jsonText) {
	std::string header = "Content-Length: " + std::to_string(jsonText.size()) + "\r\n\r\n";
	std::vector<std::uint8_t> out;
	out.reserve(header.size() + jsonText.size());
	out.insert(out.end(), header.begin(), header.end());
	out.insert(out.end(), jsonText.begin(), jsonText.end());
	return out;
}

} // namespace tlcfi::jsonrpc


