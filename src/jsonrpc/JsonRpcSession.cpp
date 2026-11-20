#include "JsonRpcSession.hpp"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace tlcfi::jsonrpc {

namespace {
/// Maximum JSON nesting depth accepted by the parser. Guards against stack
/// exhaustion from maliciously deep input (e.g. thousands of nested arrays).
constexpr int kMaxJsonParseDepth = 128;

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;

bool isInvalidParamsError(const std::exception& ex) {
	return dynamic_cast<const nlohmann::json::exception*>(&ex) != nullptr
	    || dynamic_cast<const std::invalid_argument*>(&ex) != nullptr;
}

void sendHandlerExceptionError(JsonRpcSession& session, const RpcId& id, const std::exception& ex) {
	if (isInvalidParamsError(ex)) {
		session.sendError(id, kInvalidParams, "Invalid params");
	} else {
		session.sendError(id, kInternalError, "Internal error");
	}
}

/**
 * @brief Redacts GEN-FI passwords from a JSON string for safe trace logging.
 *
 * The registration message carries the participant password in cleartext
 * (`{"username":...,"password":...}`). This masks the password value before
 * the text is handed to the onTrace hook, so credentials never reach logs.
 * It is applied only to the traced copy — the bytes sent on the wire are
 * unaffected.
 *
 * A GEN-FI password may not contain '"' or ',', so the value never holds an
 * escaped quote and scanning to the next '"' is exact. The leading quote in
 * the "password" key match prevents false matches on substrings such as
 * "my_password".
 */
std::string redactCredentials(std::string text) {
	static constexpr char kKey[] = "\"password\"";
	static constexpr char kMask[] = "***";
	constexpr std::size_t kKeyLen = sizeof(kKey) - 1;
	constexpr std::size_t kMaskLen = sizeof(kMask) - 1;

	std::size_t pos = 0;
	while ((pos = text.find(kKey, pos)) != std::string::npos) {
		const std::size_t colon = text.find(':', pos + kKeyLen);
		if (colon == std::string::npos) break;
		const std::size_t openQuote = text.find('"', colon + 1);
		if (openQuote == std::string::npos) break;
		const std::size_t closeQuote = text.find('"', openQuote + 1);
		if (closeQuote == std::string::npos) break;

		text.replace(openQuote + 1, closeQuote - openQuote - 1, kMask);
		pos = openQuote + 1 + kMaskLen + 1;  // resume past the redacted value
	}
	return text;
}
} // namespace

JsonRpcSession::JsonRpcSession(tlcfi::tcp::TransportPtr transport, std::shared_ptr<IMessageFramer> framer)
	: transport_(std::move(transport)), framer_(std::move(framer)) {
	transport_->onConnected = [this]() {
		if (events.onConnected) events.onConnected();
	};
	transport_->onDisconnected = [this](const std::string& reason) {
		pending_.failAll(reason);
		if (events.onDisconnected) events.onDisconnected(reason);
	};
	transport_->onError = [this](const std::string& reason) {
		if (events.onProtocolError) events.onProtocolError(reason);
	};
	transport_->onBytes = [this](const std::uint8_t* data, std::size_t size) {
		framer_->onBytesReceived(
			data,
			size,
			[this](const std::string& jsonText) { this->handleIncomingJson(jsonText); },
			[this](const std::string& err) { if (events.onProtocolError) events.onProtocolError(err); }
		);
	};
}

JsonRpcSession::~JsonRpcSession() = default;

void JsonRpcSession::start() {
	transport_->start();
}

void JsonRpcSession::stop() {
	transport_->stop();
}

std::future<nlohmann::json> JsonRpcSession::sendRequest(const std::string& method,
	                                                     nlohmann::json params,
	                                                     std::chrono::milliseconds timeout) {
	auto reg = pending_.registerRequest(timeout);
	RpcId id = reg.first;
	std::future<nlohmann::json> fut = std::move(reg.second);
	nlohmann::json obj;
	obj["jsonrpc"] = "2.0";
	obj["id"] = std::holds_alternative<int64_t>(id) ? nlohmann::json(std::get<int64_t>(id))
		: std::holds_alternative<std::string>(id) ? nlohmann::json(std::get<std::string>(id))
		: nlohmann::json();
	obj["method"] = method;
	obj["params"] = std::move(params);
	sendJson(obj);
	return fut;
}

void JsonRpcSession::sendNotification(const std::string& method, nlohmann::json params) {
	nlohmann::json obj;
	obj["jsonrpc"] = "2.0";
	obj["method"] = method;
	obj["params"] = std::move(params);
	sendJson(obj);
}

void JsonRpcSession::sendResponse(const RpcId& id, const nlohmann::json& result) {
	nlohmann::json obj;
	obj["jsonrpc"] = "2.0";
	if (std::holds_alternative<int64_t>(id)) obj["id"] = std::get<int64_t>(id);
	else if (std::holds_alternative<std::string>(id)) obj["id"] = std::get<std::string>(id);
	else obj["id"] = nullptr;
	obj["result"] = result;
	sendJson(obj);
}

void JsonRpcSession::sendError(const RpcId& id, int code, const std::string& message, nlohmann::json data) {
	nlohmann::json obj;
	obj["jsonrpc"] = "2.0";
	if (std::holds_alternative<int64_t>(id)) obj["id"] = std::get<int64_t>(id);
	else if (std::holds_alternative<std::string>(id)) obj["id"] = std::get<std::string>(id);
	else obj["id"] = nullptr;
	obj["error"] = { {"code", code}, {"message", message}, {"data", data} };
	sendJson(obj);
}

void JsonRpcSession::registerHandler(const std::string& method, JsonRpcDispatcher::Handler handler) {
	dispatcher_.registerHandler(method, std::move(handler));
}

void JsonRpcSession::handleIncomingJson(const std::string& jsonText) {
	if (onTrace) onTrace("recv", redactCredentials(jsonText));
	try {
		// Parse with a depth-limiting callback: the callback runs at each
		// nesting level, so throwing here aborts parsing well before the
		// recursive parser can exhaust the stack.
		auto depthGuard = [](int depth, nlohmann::json::parse_event_t,
		                     nlohmann::json&) -> bool {
			if (depth > kMaxJsonParseDepth) {
				throw std::runtime_error("JSON nesting depth exceeds maximum");
			}
			return true;
		};
		auto value = nlohmann::json::parse(jsonText, depthGuard);
		classifyAndDispatch(value);
	} catch (const std::exception& ex) {
		sendError(RpcId{nullptr}, kParseError, "Parse error");
		if (events.onProtocolError) events.onProtocolError(std::string("Parse error: ") + ex.what());
	}
}

bool JsonRpcSession::isJsonRpc(const nlohmann::json& obj) {
	return obj.is_object() && obj.contains("jsonrpc") && obj["jsonrpc"] == "2.0";
}

RpcId JsonRpcSession::parseId(const nlohmann::json& obj) {
	if (!obj.contains("id")) return RpcId{nullptr};
	const auto& idv = obj["id"];
	if (idv.is_number_integer()) return RpcId{ idv.get<int64_t>() };
	if (idv.is_string()) return RpcId{ idv.get<std::string>() };
	if (idv.is_null()) return RpcId{ nullptr };
	return RpcId{ nullptr };
}

void JsonRpcSession::classifyAndDispatch(const nlohmann::json& value) {
	if (value.is_array()) {
		for (const auto& item : value) classifyAndDispatch(item);
		return;
	}
	if (!isJsonRpc(value)) {
		if (events.onProtocolError) events.onProtocolError("Invalid Request: missing jsonrpc 2.0");
		return;
	}
	const bool hasMethod = value.contains("method");
	const bool hasId = value.contains("id");
	const bool hasResult = value.contains("result");
	const bool hasError = value.contains("error");

	if (hasMethod) {
		const RpcId id = hasId ? parseId(value) : RpcId{nullptr};
		try {
			if (!value["method"].is_string()) {
				if (hasId) {
					sendError(id, kInvalidRequest, "Invalid Request");
				} else if (events.onProtocolError) {
					events.onProtocolError("Invalid Request: method must be a string");
				}
				return;
			}
			const std::string method = value["method"].get<std::string>();
			nlohmann::json params = value.value("params", nlohmann::json::object());
			if (value.contains("params")
			    && !params.is_null()
			    && !params.is_object()
			    && !params.is_array()) {
				if (hasId) {
					sendError(id, kInvalidRequest, "Invalid Request");
				} else if (events.onProtocolError) {
					events.onProtocolError("Invalid Request: params must be an object, array, or null");
				}
				return;
			}
			if (hasId) {
				if (auto handled = dispatcher_.tryHandleRequest(method, params, id)) {
					sendResponse(id, *handled);
				} else if (events.onRequest) {
					events.onRequest(method, std::move(params), id);
				} else {
					sendError(id, kMethodNotFound, "Method not found");
				}
			} else if (events.onNotification) {
				events.onNotification(method, std::move(params));
			}
		} catch (const std::exception& ex) {
			if (hasId) {
				sendHandlerExceptionError(*this, id, ex);
			} else if (events.onProtocolError) {
				events.onProtocolError(ex.what());
			}
		}
		return;
	}

	if (hasId && (hasResult ^ hasError)) {
		RpcId id = parseId(value);
		if (hasResult) {
			if (!pending_.resolve(id, value["result"])) {
				if (events.onResponse) events.onResponse(id, value["result"]);
			}
		} else {
			RpcError err;
			err.code = value["error"].value("code", 0);
			err.message = value["error"].value("message", std::string{});
			err.data = value["error"].value("data", nlohmann::json());
			if (!pending_.reject(id, err)) {
				if (events.onErrorResponse) events.onErrorResponse(id, err);
			}
		}
		return;
	}

	if (events.onProtocolError) events.onProtocolError("Invalid Request/Response shape");
}

void JsonRpcSession::sendJson(const nlohmann::json& obj) {
	const std::string text = obj.dump();
	if (onTrace) onTrace("send", redactCredentials(text));
	auto bytes = framer_->encode(text);
	std::lock_guard<std::mutex> lock(sendMutex_);
	try {
		transport_->asyncSend(bytes.data(), bytes.size());
	} catch (...) {
		// A disconnect/stop can race an inbound message whose handler tries to
		// answer on the I/O thread. Swallow the send failure and let the
		// connection lifecycle report the loss instead of std::terminate.
	}
}

void JsonRpcSession::sendRawJsonText(const std::string& jsonText) {
	const std::string& text = jsonText;
	if (onTrace) onTrace("send", redactCredentials(text));
	auto bytes = framer_->encode(text);
	std::lock_guard<std::mutex> lock(sendMutex_);
	try {
		transport_->asyncSend(bytes.data(), bytes.size());
	} catch (...) {
		// Same disconnect race as sendJson().
	}
}

} // namespace tlcfi::jsonrpc


