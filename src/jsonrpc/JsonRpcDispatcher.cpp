#include "JsonRpcDispatcher.hpp"

namespace tlcfi::jsonrpc {

void JsonRpcDispatcher::registerHandler(const std::string& method, Handler handler) {
	methodToHandler_[method] = std::move(handler);
}

void JsonRpcDispatcher::unregisterHandler(const std::string& method) {
	methodToHandler_.erase(method);
}

std::optional<nlohmann::json> JsonRpcDispatcher::tryHandleRequest(const std::string& method,
	                                                                const nlohmann::json& params,
	                                                                const RpcId&) {
	auto it = methodToHandler_.find(method);
	if (it == methodToHandler_.end()) {
		return std::nullopt;
	}
	return it->second(params);
}

} // namespace tlcfi::jsonrpc


