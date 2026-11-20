#pragma once

#include "RpcTypes.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <optional>

namespace tlcfi::jsonrpc {

/**
 * @brief Dispatcher that routes JSON-RPC requests to registered method handlers.
 * 
 * This class maintains a registry of method name to handler function mappings
 * and provides functionality to synchronously invoke handlers for incoming requests.
 * Handlers are invoked with the request parameters and return a JSON result value
 * that will be sent as the response.
 * 
 * The dispatcher is used by JsonRpcSession to handle requests that have registered
 * handlers. If no handler is registered for a method, the session can fall back
 * to emitting an onRequest event for manual handling.
 * 
 * @note Handlers are invoked synchronously on the thread that calls tryHandleRequest.
 *       Handler exceptions should be caught by the caller if error responses are desired.
 */
class JsonRpcDispatcher {
public:
	/**
	 * @brief Type alias for a method handler function.
	 * 
	 * Handlers receive the request parameters as JSON and return the result as JSON.
	 * The handler may throw exceptions or return nullopt to indicate errors, which
	 * should be handled by the caller.
	 */
	using Handler = std::function<nlohmann::json(nlohmann::json)>;

	/**
	 * @brief Registers a handler for a specific method name.
	 * 
	 * Associates a handler function with a method name. If a handler already exists
	 * for the method, it is replaced. The handler will be invoked synchronously when
	 * a request with this method name is received.
	 * 
	 * @param method The method name to handle.
	 * @param handler The handler function to invoke for requests with this method.
	 */
	void registerHandler(const std::string& method, Handler handler);

	/**
	 * @brief Unregisters the handler for a specific method name.
	 * 
	 * Removes the handler association for the specified method. Subsequent requests
	 * for this method will not be handled by the dispatcher.
	 * 
	 * @param method The method name to unregister.
	 */
	void unregisterHandler(const std::string& method);

	/**
	 * @brief Attempts to handle a request synchronously using a registered handler.
	 * 
	 * Looks up a handler for the specified method name and invokes it with the
	 * provided parameters. Returns the handler's result if a handler exists, or
	 * nullopt if no handler is registered.
	 * 
	 * @param method The method name from the request.
	 * @param params The request parameters.
	 * @param id The request ID (unused by the dispatcher, provided for context).
	 * @return The handler's result JSON if a handler exists, nullopt otherwise.
	 */
	std::optional<nlohmann::json> tryHandleRequest(const std::string& method,
	                                              const nlohmann::json& params,
	                                              const RpcId& id);

private:
	std::unordered_map<std::string, Handler> methodToHandler_;  ///< Map of method names to handler functions.
};

} // namespace tlcfi::jsonrpc


