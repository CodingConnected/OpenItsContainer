#include "PendingRequestManager.hpp"

#include <stdexcept>

namespace tlcfi::jsonrpc {

namespace {
/// How often the sweeper checks for requests past their deadline.
constexpr auto kSweepInterval = std::chrono::milliseconds(200);
}

PendingRequestManager::PendingRequestManager() {
	sweepThread_ = std::thread([this] { sweepLoop(); });
}

PendingRequestManager::~PendingRequestManager() {
	{
		std::lock_guard<std::mutex> lock(mutex_);
		sweepStop_ = true;
	}
	sweepCv_.notify_all();
	if (sweepThread_.joinable()) {
		sweepThread_.join();
	}
	// Fail any requests still outstanding at destruction so callers waiting on
	// their futures do not observe a broken_promise.
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto& [id, p] : pending_) {
		p.promise.set_exception(std::make_exception_ptr(
			std::runtime_error("PendingRequestManager destroyed")));
	}
	pending_.clear();
}

RpcId PendingRequestManager::nextId() {
	return RpcId{counter_.fetch_add(1)};
}

std::pair<RpcId, std::future<nlohmann::json>> PendingRequestManager::registerRequest(std::chrono::milliseconds timeout) {
	RpcId id = nextId();
	std::promise<nlohmann::json> promise;
	auto future = promise.get_future();
	std::lock_guard<std::mutex> lock(mutex_);
	// Insert the pending request using RpcId as the key. The map uses RpcIdLess comparator
	// to provide ordering for the variant type (int64_t, std::string, or nullptr_t).
	pending_.emplace(id, Pending{std::move(promise), std::chrono::steady_clock::now() + timeout});
	return {id, std::move(future)};
}

bool PendingRequestManager::resolve(const RpcId& id, const nlohmann::json& result) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = pending_.find(id);
	if (it == pending_.end()) return false;
	it->second.promise.set_value(result);
	pending_.erase(it);
	return true;
}

bool PendingRequestManager::reject(const RpcId& id, const RpcError& error) {
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = pending_.find(id);
	if (it == pending_.end()) return false;
	// Represent error as JSON per JSON-RPC response
	nlohmann::json j;
	j["error"] = { {"code", error.code}, {"message", error.message}, {"data", error.data} };
	it->second.promise.set_exception(std::make_exception_ptr(std::runtime_error(j.dump())));
	pending_.erase(it);
	return true;
}

void PendingRequestManager::failAll(const std::string& reason) {
	std::lock_guard<std::mutex> lock(mutex_);
	for (auto& [id, p] : pending_) {
		p.promise.set_exception(std::make_exception_ptr(std::runtime_error(reason)));
	}
	pending_.clear();
}

void PendingRequestManager::sweepLoop() {
	std::unique_lock<std::mutex> lock(mutex_);
	while (!sweepStop_) {
		sweepCv_.wait_for(lock, kSweepInterval, [this] { return sweepStop_; });
		if (sweepStop_) break;
		// Fail every request whose deadline has passed; the caller's future
		// (whether waited on via wait_for or a blocking get()) then unblocks
		// with a timeout error instead of hanging on a lost response.
		const auto now = std::chrono::steady_clock::now();
		for (auto it = pending_.begin(); it != pending_.end(); ) {
			if (it->second.deadline <= now) {
				it->second.promise.set_exception(std::make_exception_ptr(
					std::runtime_error("RPC request timed out")));
				it = pending_.erase(it);
			} else {
				++it;
			}
		}
	}
}

} // namespace tlcfi::jsonrpc
