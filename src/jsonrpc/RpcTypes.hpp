#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <chrono>

#include <nlohmann/json.hpp>

namespace tlcfi::jsonrpc {

/**
 * @brief Type alias for JSON-RPC 2.0 request/response IDs.
 * 
 * According to the JSON-RPC 2.0 specification, the "id" field can be a string,
 * a number, or null. This variant type represents all three possibilities.
 * Requests use non-null IDs, while notifications use null IDs.
 */
using RpcId = std::variant<int64_t, std::string, std::nullptr_t>;

/**
 * @brief Structure representing a JSON-RPC error response.
 * 
 * Contains the standard JSON-RPC error fields: a numeric error code, a
 * human-readable error message, and optional additional error data.
 */
struct RpcError {
	int code {0};                    ///< Numeric error code (standard JSON-RPC codes or application-defined).
	std::string message;             ///< Human-readable error message.
	nlohmann::json data;            ///< Optional additional error data (can be null).
};

/**
 * @brief Structure containing event callbacks for JSON-RPC session lifecycle and messages.
 * 
 * All callbacks are optional (may be null/empty). Set the desired callbacks to
 * receive notifications about connection events, incoming requests/notifications,
 * responses, and protocol errors.
 */
struct JsonRpcEvents {
	/**
	 * @brief Callback invoked when a request is received.
	 * 
	 * Invoked for incoming requests (messages with both "method" and "id" fields).
	 * The callback should handle the request and send a response using
	 * JsonRpcSession::sendResponse() or JsonRpcSession::sendError().
	 * 
	 * @param method The method name from the request.
	 * @param params The request parameters.
	 * @param id The request ID (for correlating the response).
	 */
	std::function<void(const std::string& method, nlohmann::json params, RpcId id)> onRequest;

	/**
	 * @brief Callback invoked when a notification is received.
	 * 
	 * Invoked for incoming notifications (messages with "method" but no "id" field).
	 * Notifications do not expect a response.
	 * 
	 * @param method The method name from the notification.
	 * @param params The notification parameters.
	 */
	std::function<void(const std::string& method, nlohmann::json params)> onNotification;

	/**
	 * @brief Callback invoked when a response is received.
	 * 
	 * Invoked for incoming responses that do not match any pending request
	 * (e.g., responses to requests handled by registered handlers, or unsolicited
	 * responses). Responses matching pending requests are handled automatically.
	 * 
	 * @param id The response ID.
	 * @param result The result JSON from the response.
	 */
	std::function<void(RpcId id, nlohmann::json result)> onResponse;

	/**
	 * @brief Callback invoked when an error response is received.
	 * 
	 * Invoked for incoming error responses that do not match any pending request.
	 * Error responses matching pending requests cause the associated future to
	 * be rejected with an exception.
	 * 
	 * @param id The response ID.
	 * @param error The error details from the response.
	 */
	std::function<void(RpcId id, const RpcError& error)> onErrorResponse;

	/**
	 * @brief Callback invoked when a protocol error occurs.
	 * 
	 * Invoked for various protocol-level errors such as invalid message format,
	 * parse errors, or malformed JSON-RPC messages.
	 * 
	 * @param reason Description of the protocol error.
	 */
	std::function<void(const std::string& reason)> onProtocolError;

	/**
	 * @brief Callback invoked when the transport connection is established.
	 * 
	 * Invoked once when the underlying transport becomes ready for communication.
	 */
	std::function<void()> onConnected;

	/**
	 * @brief Callback invoked when the transport connection is closed.
	 * 
	 * Invoked when the connection is closed, either due to an error, remote
	 * closure, or explicit stop() call.
	 * 
	 * @param reason Description of why the connection was closed.
	 */
	std::function<void(const std::string& reason)> onDisconnected;
};

/**
 * @brief Enumeration of JSON-RPC message types.
 * 
 * Used for classifying incoming messages during parsing and dispatch.
 */
enum class MessageKind {
	Invalid,        ///< Message does not conform to JSON-RPC 2.0 format.
	Request,       ///< Request message (has "method" and "id").
	Notification,  ///< Notification message (has "method" but no "id").
	Response,      ///< Response message (has "id" and "result" or "error").
	Batch          ///< Batch message (array of messages).
};

} // namespace tlcfi::jsonrpc


