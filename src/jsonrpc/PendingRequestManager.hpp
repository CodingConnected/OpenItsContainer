#pragma once

#include "RpcTypes.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <thread>

namespace tlcfi::jsonrpc {

/**
 * @brief Comparator for RpcId values used in ordered containers.
 * 
 * Provides a strict weak ordering for RpcId variants to enable their use as
 * keys in std::map. Compares by variant index first, then by value within
 * the same type. Null values compare equal to each other.
 */
struct RpcIdLess {
	/**
	 * @brief Compares two RpcId values for ordering.
	 * 
	 * @param a First RpcId to compare.
	 * @param b Second RpcId to compare.
	 * @return True if a should be ordered before b.
	 */
	bool operator()(const RpcId& a, const RpcId& b) const {
		if (a.index() != b.index()) return a.index() < b.index();
		if (std::holds_alternative<std::nullptr_t>(a)) return false;
		if (auto ai = std::get_if<int64_t>(&a)) {
			const auto& bi = std::get<int64_t>(b);
			return *ai < bi;
		}
		const auto& as = std::get<std::string>(a);
		const auto& bs = std::get<std::string>(b);
		return as < bs;
	}
};

/**
 * @brief Manages pending JSON-RPC requests and their associated futures.
 * 
 * This class tracks outgoing requests and pairs them with their responses using
 * unique request IDs. It generates sequential numeric IDs, registers requests
 * with timeout deadlines, and provides methods to resolve or reject requests
 * when responses arrive.
 * 
 * The manager is thread-safe and supports concurrent request registration and
 * resolution. When a connection is lost, all pending requests can be failed
 * at once.
 * 
 * @note Request IDs are generated sequentially starting from 1. A background
 *       sweeper thread enforces per-request deadlines: a request that is not
 *       resolved or rejected before its deadline is failed with a timeout
 *       error so the caller's future never blocks indefinitely.
 */
class PendingRequestManager {
public:
	/**
	 * @brief Constructs the manager and starts the timeout-sweeper thread.
	 */
	PendingRequestManager();

	/**
	 * @brief Stops the sweeper thread and fails any still-pending requests.
	 */
	~PendingRequestManager();
	/**
	 * @brief Structure holding state for a pending request.
	 * 
	 * Contains the promise that will be fulfilled when the response arrives
	 * and the deadline timestamp for timeout handling.
	 */
	struct Pending {
		std::promise<nlohmann::json> promise;                              ///< Promise to fulfill with the response.
		std::chrono::steady_clock::time_point deadline;                    ///< Time point when the request times out.
	};

	/**
	 * @brief Generates the next sequential request ID.
	 * 
	 * Returns a new numeric request ID, incrementing an internal counter.
	 * IDs start at 1 and increment for each call.
	 * 
	 * @return A new RpcId containing the next sequential integer ID.
	 */
	RpcId nextId();

	/**
	 * @brief Registers a new pending request with a timeout.
	 * 
	 * Creates a new request entry with a unique ID and associates it with
	 * a future that will be fulfilled when the response arrives. The request
	 * is stored with a deadline calculated from the current time plus the
	 * specified timeout.
	 * 
	 * @param timeout Maximum time to wait for a response before timing out.
	 * @return A pair containing the generated request ID and the associated future.
	 */
	std::pair<RpcId, std::future<nlohmann::json>> registerRequest(std::chrono::milliseconds timeout);

	/**
	 * @brief Resolves a pending request with a successful result.
	 * 
	 * Looks up the request by ID and fulfills its promise with the provided
	 * result JSON. Returns true if the request was found and resolved, false
	 * if no matching request exists.
	 * 
	 * @param id The request ID to resolve.
	 * @param result The result JSON to return.
	 * @return True if the request was found and resolved, false otherwise.
	 */
	bool resolve(const RpcId& id, const nlohmann::json& result);

	/**
	 * @brief Rejects a pending request with an error.
	 * 
	 * Looks up the request by ID and sets an exception on its promise with
	 * an error message formatted as JSON-RPC error response. Returns true
	 * if the request was found and rejected, false if no matching request exists.
	 * 
	 * @param id The request ID to reject.
	 * @param error The error details to include in the exception.
	 * @return True if the request was found and rejected, false otherwise.
	 */
	bool reject(const RpcId& id, const RpcError& error);

	/**
	 * @brief Fails all pending requests with a reason.
	 * 
	 * Sets an exception on all pending request promises with the provided
	 * reason string, then clears the pending requests map. Used when the
	 * connection is lost or the session is stopped.
	 * 
	 * @param reason The error reason to include in all exceptions.
	 */
	void failAll(const std::string& reason);

private:
	/// Background loop that fails requests whose deadline has passed.
	void sweepLoop();

	std::mutex mutex_;                                    ///< Mutex protecting the pending requests map and sweepStop_.
	std::map<RpcId, Pending, RpcIdLess> pending_;       ///< Map of request IDs to pending request state.
	std::atomic<int64_t> counter_ {1};                    ///< Atomic counter for generating sequential request IDs.
	std::condition_variable sweepCv_;                     ///< Wakes the sweeper for shutdown.
	bool sweepStop_ {false};                              ///< Set under mutex_ to stop the sweeper thread.
	std::thread sweepThread_;                             ///< Background timeout-sweeper thread.
};

} // namespace tlcfi::jsonrpc


