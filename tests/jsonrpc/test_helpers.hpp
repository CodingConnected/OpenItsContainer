#pragma once

#include "tcp/ITransport.hpp"
#include "jsonrpc/IMessageFramer.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace test_helpers::jsonrpc {

/**
 * @brief Mock transport implementation for testing JSON-RPC sessions.
 * 
 * This mock transport allows tests to simulate connection events, inject
 * incoming bytes, and capture outgoing bytes without requiring actual
 * network communication.
 */
class MockTransport : public tlcfi::tcp::ITransport {
public:
	MockTransport() : started_(false), stopped_(false) {}

	void start() override {
		std::lock_guard<std::mutex> lock(mutex_);
		started_ = true;
		stopped_ = false;
		if (onConnected) {
			onConnected();
		}
	}

	void stop() override {
		std::lock_guard<std::mutex> lock(mutex_);
		stopped_ = true;
		started_ = false;
		if (onDisconnected) {
			onDisconnected("Mock transport stopped");
		}
	}

	void asyncSend(const std::uint8_t* data, std::size_t size) override {
		std::lock_guard<std::mutex> lock(mutex_);
		if (rejectSends_) {
			throw std::runtime_error("Cannot send: not connected or stopping");
		}
		sentBytes_.insert(sentBytes_.end(), data, data + size);
	}

	std::string name() const override { return "mock-transport"; }

	/**
	 * @brief Simulates receiving bytes from the remote side.
	 * 
	 * Invokes the onBytes callback with the provided data, simulating
	 * incoming data from the transport.
	 * 
	 * @param data The data to simulate receiving.
	 */
	void simulateReceive(const std::vector<std::uint8_t>& data) {
		if (onBytes && !data.empty()) {
			onBytes(data.data(), data.size());
		}
	}

	/**
	 * @brief Simulates receiving bytes from a string.
	 * 
	 * Convenience method that converts a string to bytes and simulates receiving.
	 * 
	 * @param text The text to simulate receiving.
	 */
	void simulateReceive(const std::string& text) {
		std::vector<std::uint8_t> data(text.begin(), text.end());
		simulateReceive(data);
	}

	/**
	 * @brief Simulates a connection error.
	 * 
	 * Invokes the onError callback with the provided reason.
	 * 
	 * @param reason The error reason.
	 */
	void simulateError(const std::string& reason) {
		if (onError) {
			onError(reason);
		}
	}

	/**
	 * @brief Gets all bytes that were sent via asyncSend.
	 * 
	 * @return A copy of all sent bytes.
	 */
	std::vector<std::uint8_t> getSentBytes() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return sentBytes_;
	}

	/**
	 * @brief Gets sent bytes as a string.
	 * 
	 * @return A string containing all sent bytes.
	 */
	std::string getSentBytesAsString() const {
		auto bytes = getSentBytes();
		return std::string(bytes.begin(), bytes.end());
	}

	/**
	 * @brief Clears the sent bytes buffer.
	 */
	void clearSentBytes() {
		std::lock_guard<std::mutex> lock(mutex_);
		sentBytes_.clear();
	}

	/**
	 * @brief When enabled, asyncSend throws like a disconnected client transport.
	 */
	void setRejectSends(bool reject) {
		std::lock_guard<std::mutex> lock(mutex_);
		rejectSends_ = reject;
	}

	/**
	 * @brief Checks if the transport has been started.
	 */
	bool isStarted() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return started_;
	}

	/**
	 * @brief Checks if the transport has been stopped.
	 */
	bool isStopped() const {
		std::lock_guard<std::mutex> lock(mutex_);
		return stopped_;
	}

private:
	mutable std::mutex mutex_;
	std::vector<std::uint8_t> sentBytes_;
	bool started_;
	bool stopped_;
	bool rejectSends_ {false};
};

/**
 * @brief Mock framer implementation for testing JSON-RPC sessions.
 * 
 * This mock framer can operate in two modes:
 * - Pass-through mode: Simply passes JSON text through unchanged (default)
 * - Custom mode: Can simulate different framing behaviors for testing
 */
class MockFramer : public tlcfi::jsonrpc::IMessageFramer {
public:
	MockFramer() : passThrough_(true) {}

	void onBytesReceived(const std::uint8_t* data, std::size_t size,
	                    const std::function<void(const std::string&)>& onMessage,
	                    const std::function<void(const std::string&)>& onError) override {
		if (passThrough_) {
			// Pass-through mode: treat all bytes as complete JSON
			std::string jsonText(reinterpret_cast<const char*>(data), size);
			if (!jsonText.empty()) {
				onMessage(jsonText);
			}
		} else {
			// Custom mode: use the custom handler if set
			if (customOnBytesHandler_) {
				customOnBytesHandler_(data, size, onMessage, onError);
			}
		}
	}

	std::vector<std::uint8_t> encode(const std::string& jsonText) override {
		if (passThrough_) {
			// Pass-through mode: return bytes as-is
			return std::vector<std::uint8_t>(jsonText.begin(), jsonText.end());
		} else {
			// Custom mode: use the custom encoder if set
			if (customEncodeHandler_) {
				return customEncodeHandler_(jsonText);
			}
			return std::vector<std::uint8_t>(jsonText.begin(), jsonText.end());
		}
	}

	/**
	 * @brief Sets a custom handler for onBytesReceived.
	 * 
	 * When set, the framer will use this handler instead of pass-through behavior.
	 * Set to nullptr to return to pass-through mode.
	 */
	void setCustomOnBytesHandler(
		std::function<void(const std::uint8_t*, std::size_t,
		                  const std::function<void(const std::string&)>&,
		                  const std::function<void(const std::string&)>&)> handler) {
		customOnBytesHandler_ = std::move(handler);
		passThrough_ = (customOnBytesHandler_ == nullptr);
	}

	/**
	 * @brief Sets a custom handler for encode.
	 * 
	 * When set, the framer will use this handler instead of pass-through behavior.
	 * Set to nullptr to return to pass-through mode.
	 */
	void setCustomEncodeHandler(
		std::function<std::vector<std::uint8_t>(const std::string&)> handler) {
		customEncodeHandler_ = std::move(handler);
		passThrough_ = (customEncodeHandler_ == nullptr);
	}

private:
	bool passThrough_;
	std::function<void(const std::uint8_t*, std::size_t,
	                  const std::function<void(const std::string&)>&,
	                  const std::function<void(const std::string&)>&)> customOnBytesHandler_;
	std::function<std::vector<std::uint8_t>(const std::string&)> customEncodeHandler_;
};

} // namespace test_helpers::jsonrpc

