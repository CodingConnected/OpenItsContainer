#pragma once

#include "IMessageFramer.hpp"
#include "ITransport.hpp"
#include "JsonRpcDispatcher.hpp"
#include "PendingRequestManager.hpp"
#include "RpcTypes.hpp"

#include <memory>
#include <mutex>
#include <future>

namespace tlcfi::jsonrpc {

/**
 * @brief JSON-RPC 2.0 session that manages bidirectional RPC communication over a transport.
 * 
 * This class provides a complete JSON-RPC 2.0 implementation that can act as both a client
 * and a server. It handles request/response pairing, notification delivery, error handling,
 * and message framing. The session integrates with a transport layer (e.g., TCP) and a
 * message framer (e.g., Content-Length, NDJSON, Balanced JSON) to provide a high-level
 * RPC interface.
 * 
 * The session supports:
 * - Client operations: sending requests (with timeout) and notifications
 * - Server operations: registering method handlers and sending responses/errors
 * - Event callbacks: connection events, request/notification handlers, response handlers
 * - Batch requests: automatic handling of JSON-RPC batch arrays
 * 
 * @note The session must be started with start() after construction. All send methods
 *       are thread-safe. The transport and framer must remain valid for the lifetime
 *       of the session.
 */
class JsonRpcSession {
public:
	/**
	 * @brief Constructs a JSON-RPC session with the specified transport and framer.
	 * 
	 * Sets up the session to use the provided transport for byte-level communication
	 * and the framer for message boundary detection. The session wires up transport
	 * callbacks to handle incoming data and connection events.
	 * 
	 * @param transport The transport instance for sending/receiving bytes.
	 * @param framer The message framer for encoding/decoding message boundaries.
	 */
	JsonRpcSession(tlcfi::tcp::TransportPtr transport, std::shared_ptr<IMessageFramer> framer);

	/**
	 * @brief Destructor that cleans up the session.
	 * 
	 * Stops the transport and cancels any pending requests.
	 */
	~JsonRpcSession();

	/**
	 * @brief Starts the session and begins processing messages.
	 * 
	 * Initiates the underlying transport, which will begin reading incoming data
	 * and invoking callbacks. The onConnected event will be fired when the
	 * transport connection is established.
	 */
	void start();

	/**
	 * @brief Stops the session and closes the connection.
	 * 
	 * Stops the underlying transport, which will close the connection and
	 * cancel any pending operations. All pending requests will be failed.
	 * The onDisconnected event will be fired with a reason.
	 */
	void stop();

	/**
	 * @brief Sends a JSON-RPC request and returns a future for the response.
	 * 
	 * Creates a request message with the specified method and parameters, assigns
	 * it a unique ID, and sends it over the transport. Returns a future that will
	 * be fulfilled when the response arrives or rejected if the timeout expires
	 * or the connection is lost.
	 * 
	 * @param method The method name to invoke.
	 * @param params The parameters object (can be object, array, or null).
	 * @param timeout Maximum time to wait for a response before timing out.
	 * @return A future that will contain the result JSON or throw on error/timeout.
	 */
	std::future<nlohmann::json> sendRequest(const std::string& method,
	                                        nlohmann::json params,
	                                        std::chrono::milliseconds timeout);

	/**
	 * @brief Sends a JSON-RPC notification (no response expected).
	 * 
	 * Creates a notification message with the specified method and parameters
	 * and sends it over the transport. Notifications do not include an ID and
	 * do not expect a response.
	 * 
	 * @param method The method name to invoke.
	 * @param params The parameters object (can be object, array, or null).
	 */
	void sendNotification(const std::string& method, nlohmann::json params);

	/**
	 * @brief Sends a successful response to a request.
	 * 
	 * Creates a response message with the specified ID and result value.
	 * This should be called in response to a request that was received
	 * via the onRequest callback or a registered handler.
	 * 
	 * @param id The request ID from the original request.
	 * @param result The result value to return.
	 */
	void sendResponse(const RpcId& id, const nlohmann::json& result);

	/**
	 * @brief Sends an error response to a request.
	 * 
	 * Creates an error response message with the specified ID, error code,
	 * message, and optional error data. This should be called when handling
	 * a request that resulted in an error.
	 * 
	 * @param id The request ID from the original request.
	 * @param code The error code (standard JSON-RPC codes or application-defined).
	 * @param message A human-readable error message.
	 * @param data Optional additional error data (defaults to null).
	 */
	void sendError(const RpcId& id, int code, const std::string& message, nlohmann::json data = nullptr);

	/**
	 * @brief Registers a handler for a specific method name.
	 * 
	 * When a request with the specified method name is received, the handler
	 * will be invoked synchronously. If the handler returns a value, it is sent
	 * as the response. If the handler throws, JsonRpcSession sends a JSON-RPC
	 * error response (-32602 for invalid params, -32603 otherwise).
	 * 
	 * @param method The method name to handle.
	 * @param handler The handler function that processes the request.
	 */
	void registerHandler(const std::string& method, JsonRpcDispatcher::Handler handler);

	/**
	 * @brief Event callbacks for connection and message lifecycle events.
	 * 
	 * Set these callbacks to receive notifications about connection state,
	 * incoming requests, notifications, responses, and protocol errors.
	 */
	JsonRpcEvents events;

	/**
	 * @brief Sends raw JSON text directly through the framer and transport.
	 * 
	 * This is a testing/diagnostics method that bypasses JSON-RPC message
	 * construction and sends the provided JSON text as-is. The text will
	 * be encoded by the framer and sent over the transport. Useful for
	 * testing edge cases or sending malformed messages.
	 * 
	 * @param jsonText The raw JSON text to send.
	 */
	void sendRawJsonText(const std::string& jsonText);

	/**
	 * @brief Optional trace callback for debugging message flow.
	 * 
	 * If set, this callback will be invoked for every message sent or received,
	 * providing the direction ("send" or "recv") and the raw JSON text. Useful
	 * for logging and debugging JSON-RPC communication.
	 */
	std::function<void(const char* direction, const std::string& text)> onTrace;

private:
	/**
	 * @brief Handles incoming JSON text from the framer.
	 * 
	 * Parses the JSON text and dispatches it to the appropriate handler
	 * based on whether it's a request, notification, response, or batch.
	 * 
	 * @param jsonText The JSON text received from the framer.
	 */
	void handleIncomingJson(const std::string& jsonText);

	/**
	 * @brief Classifies a parsed JSON value and dispatches it appropriately.
	 * 
	 * Determines whether the JSON object is a request, notification, response,
	 * or batch, and routes it to the appropriate handler. Handles batch arrays
	 * by recursively processing each element.
	 * 
	 * @param value The parsed JSON value to classify and dispatch.
	 */
	void classifyAndDispatch(const nlohmann::json& value);

	/**
	 * @brief Checks if a JSON object is a valid JSON-RPC 2.0 message.
	 * 
	 * @param obj The JSON object to validate.
	 * @return True if the object contains "jsonrpc": "2.0", false otherwise.
	 */
	static bool isJsonRpc(const nlohmann::json& obj);

	/**
	 * @brief Parses the ID field from a JSON-RPC message.
	 * 
	 * Extracts the ID from a JSON-RPC message, handling string, number,
	 * and null ID types as per the JSON-RPC 2.0 specification.
	 * 
	 * @param obj The JSON-RPC message object.
	 * @return The parsed RpcId variant (int64_t, string, or nullptr).
	 */
	static RpcId parseId(const nlohmann::json& obj);

	/**
	 * @brief Sends a JSON object as a JSON-RPC message.
	 * 
	 * Serializes the JSON object to text, encodes it using the framer,
	 * and sends it over the transport. Thread-safe via sendMutex_.
	 * 
	 * @param obj The JSON object to send.
	 */
	void sendJson(const nlohmann::json& obj);

private:
	tlcfi::tcp::TransportPtr transport_;              ///< The transport for byte-level communication.
	std::shared_ptr<IMessageFramer> framer_;          ///< The message framer for encoding/decoding boundaries.
	PendingRequestManager pending_;                    ///< Manager for tracking pending requests and timeouts.
	JsonRpcDispatcher dispatcher_;                    ///< Dispatcher for routing requests to registered handlers.
	std::mutex sendMutex_;                            ///< Mutex protecting send operations for thread-safety.
};

} // namespace tlcfi::jsonrpc


