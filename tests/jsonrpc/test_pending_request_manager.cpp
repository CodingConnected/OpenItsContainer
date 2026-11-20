#include <gtest/gtest.h>
#include "jsonrpc/PendingRequestManager.hpp"
#include "jsonrpc/RpcTypes.hpp"

#include <chrono>
#include <future>
#include <stdexcept>

class PendingRequestManagerTest : public ::testing::Test {
protected:
	void SetUp() override {
		manager_ = std::make_unique<tlcfi::jsonrpc::PendingRequestManager>();
	}

	void TearDown() override {
		manager_.reset();
	}

	std::unique_ptr<tlcfi::jsonrpc::PendingRequestManager> manager_;
};

TEST_F(PendingRequestManagerTest, SequentialIdGeneration) {
	auto id1 = manager_->nextId();
	auto id2 = manager_->nextId();
	auto id3 = manager_->nextId();

	ASSERT_TRUE(std::holds_alternative<int64_t>(id1));
	ASSERT_TRUE(std::holds_alternative<int64_t>(id2));
	ASSERT_TRUE(std::holds_alternative<int64_t>(id3));

	EXPECT_EQ(std::get<int64_t>(id1), 1);
	EXPECT_EQ(std::get<int64_t>(id2), 2);
	EXPECT_EQ(std::get<int64_t>(id3), 3);
}

TEST_F(PendingRequestManagerTest, RegisterRequestCreatesFuture) {
	auto [id, future] = manager_->registerRequest(std::chrono::milliseconds(1000));

	ASSERT_TRUE(std::holds_alternative<int64_t>(id));
	EXPECT_EQ(std::get<int64_t>(id), 1);

	// Future should be valid but not ready yet
	EXPECT_EQ(future.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
}

TEST_F(PendingRequestManagerTest, ResolveWithMatchingId) {
	auto [id, future] = manager_->registerRequest(std::chrono::milliseconds(1000));

	nlohmann::json result{{"value", 42}};
	bool resolved = manager_->resolve(id, result);

	EXPECT_TRUE(resolved);
	EXPECT_EQ(future.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
	
	auto receivedResult = future.get();
	EXPECT_EQ(receivedResult["value"], 42);
}

TEST_F(PendingRequestManagerTest, ResolveWithNonMatchingId) {
	auto [id1, future1] = manager_->registerRequest(std::chrono::milliseconds(1000));
	auto [id2, future2] = manager_->registerRequest(std::chrono::milliseconds(1000));

	// Try to resolve with wrong ID
	tlcfi::jsonrpc::RpcId wrongId{999};
	nlohmann::json result{{"value", 42}};
	bool resolved = manager_->resolve(wrongId, result);

	EXPECT_FALSE(resolved);
	// Both futures should still be pending
	EXPECT_EQ(future1.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
	EXPECT_EQ(future2.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);

	// Now resolve with correct ID
	EXPECT_TRUE(manager_->resolve(id1, result));
	EXPECT_EQ(future1.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
}

TEST_F(PendingRequestManagerTest, RejectWithMatchingId) {
	auto [id, future] = manager_->registerRequest(std::chrono::milliseconds(1000));

	tlcfi::jsonrpc::RpcError error;
	error.code = -32601;
	error.message = "Method not found";
	error.data = nlohmann::json{{"method", "unknown"}};

	bool rejected = manager_->reject(id, error);

	EXPECT_TRUE(rejected);
	EXPECT_EQ(future.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);

	EXPECT_THROW({
		try {
			future.get();
		} catch (const std::runtime_error& e) {
			// Verify error message contains JSON-RPC error format
			std::string errorMsg = e.what();
			EXPECT_NE(errorMsg.find("Method not found"), std::string::npos);
			throw;
		}
	}, std::runtime_error);
}

TEST_F(PendingRequestManagerTest, RejectWithNonMatchingId) {
	auto [id, future] = manager_->registerRequest(std::chrono::milliseconds(1000));

	tlcfi::jsonrpc::RpcId wrongId{999};
	tlcfi::jsonrpc::RpcError error;
	error.code = -1;
	error.message = "Test error";

	bool rejected = manager_->reject(wrongId, error);

	EXPECT_FALSE(rejected);
	// Future should still be pending
	EXPECT_EQ(future.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
}

TEST_F(PendingRequestManagerTest, FailAll) {
	auto [id1, future1] = manager_->registerRequest(std::chrono::milliseconds(1000));
	auto [id2, future2] = manager_->registerRequest(std::chrono::milliseconds(1000));
	auto [id3, future3] = manager_->registerRequest(std::chrono::milliseconds(1000));

	std::string reason = "Connection lost";
	manager_->failAll(reason);

	// All futures should be ready with exceptions
	EXPECT_EQ(future1.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
	EXPECT_EQ(future2.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
	EXPECT_EQ(future3.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);

	EXPECT_THROW({
		try {
			future1.get();
		} catch (const std::runtime_error& e) {
			EXPECT_STREQ(e.what(), reason.c_str());
			throw;
		}
	}, std::runtime_error);

	EXPECT_THROW({
		try {
			future2.get();
		} catch (const std::runtime_error& e) {
			EXPECT_STREQ(e.what(), reason.c_str());
			throw;
		}
	}, std::runtime_error);

	EXPECT_THROW({
		try {
			future3.get();
		} catch (const std::runtime_error& e) {
			EXPECT_STREQ(e.what(), reason.c_str());
			throw;
		}
	}, std::runtime_error);
}

TEST_F(PendingRequestManagerTest, MultipleRequestsTrackedSimultaneously) {
	auto [id1, future1] = manager_->registerRequest(std::chrono::milliseconds(1000));
	auto [id2, future2] = manager_->registerRequest(std::chrono::milliseconds(1000));
	auto [id3, future3] = manager_->registerRequest(std::chrono::milliseconds(1000));

	// Resolve them in different order
	nlohmann::json result2{{"id", 2}};
	EXPECT_TRUE(manager_->resolve(id2, result2));
	EXPECT_EQ(future2.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
	EXPECT_EQ(future2.get()["id"], 2);

	nlohmann::json result3{{"id", 3}};
	EXPECT_TRUE(manager_->resolve(id3, result3));
	EXPECT_EQ(future3.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
	EXPECT_EQ(future3.get()["id"], 3);

	nlohmann::json result1{{"id", 1}};
	EXPECT_TRUE(manager_->resolve(id1, result1));
	EXPECT_EQ(future1.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
	EXPECT_EQ(future1.get()["id"], 1);
}

TEST_F(PendingRequestManagerTest, ResolveRemovesEntry) {
	auto [id1, future1] = manager_->registerRequest(std::chrono::milliseconds(1000));
	auto [id2, future2] = manager_->registerRequest(std::chrono::milliseconds(1000));

	// Resolve first request
	nlohmann::json result{{"value", 1}};
	EXPECT_TRUE(manager_->resolve(id1, result));
	future1.get(); // Wait for it to complete

	// Try to resolve again - should fail
	EXPECT_FALSE(manager_->resolve(id1, result));

	// Second request should still be pending
	EXPECT_EQ(future2.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
}

TEST_F(PendingRequestManagerTest, RejectRemovesEntry) {
	auto [id1, future1] = manager_->registerRequest(std::chrono::milliseconds(1000));
	auto [id2, future2] = manager_->registerRequest(std::chrono::milliseconds(1000));

	// Reject first request
	tlcfi::jsonrpc::RpcError error;
	error.code = -1;
	error.message = "Test";
	EXPECT_TRUE(manager_->reject(id1, error));

	EXPECT_THROW(future1.get(), std::runtime_error);

	// Try to reject again - should fail
	EXPECT_FALSE(manager_->reject(id1, error));

	// Second request should still be pending
	EXPECT_EQ(future2.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
}

TEST_F(PendingRequestManagerTest, ComplexJsonResult) {
	auto [id, future] = manager_->registerRequest(std::chrono::milliseconds(1000));

	nlohmann::json result{
		{"status", "success"},
		{"data", {
			{"items", {1, 2, 3}},
			{"metadata", {{"count", 3}}}
		}}
	};

	EXPECT_TRUE(manager_->resolve(id, result));
	auto receivedResult = future.get();

	EXPECT_EQ(receivedResult["status"], "success");
	EXPECT_EQ(receivedResult["data"]["items"], (nlohmann::json{1, 2, 3}));
	EXPECT_EQ(receivedResult["data"]["metadata"]["count"], 3);
}

TEST_F(PendingRequestManagerTest, RequestTimesOutWhenNotResolved) {
	// A request that is never resolved must be failed by the background
	// sweeper once its deadline passes (sweep interval is 200ms).
	auto [id, future] = manager_->registerRequest(std::chrono::milliseconds(50));

	ASSERT_EQ(future.wait_for(std::chrono::milliseconds(2000)), std::future_status::ready)
		<< "the sweeper must unblock a timed-out request's future";

	try {
		future.get();
		FAIL() << "Expected a timeout exception";
	} catch (const std::runtime_error& e) {
		EXPECT_NE(std::string(e.what()).find("timed out"), std::string::npos);
	}
}

TEST_F(PendingRequestManagerTest, ResolveAfterTimeoutReportsNotFound) {
	// Once the sweeper has failed and removed a request, a late response for
	// that id must not match anything.
	auto [id, future] = manager_->registerRequest(std::chrono::milliseconds(50));

	ASSERT_EQ(future.wait_for(std::chrono::milliseconds(2000)), std::future_status::ready);
	EXPECT_THROW(future.get(), std::runtime_error);

	EXPECT_FALSE(manager_->resolve(id, nlohmann::json{{"late", true}}))
		<< "a timed-out entry must already be removed from the pending map";
}

TEST_F(PendingRequestManagerTest, RequestResolvedBeforeDeadlineNotTimedOut) {
	// A request resolved well before its deadline must carry the real result,
	// never the timeout exception.
	auto [id, future] = manager_->registerRequest(std::chrono::milliseconds(5000));

	nlohmann::json result{{"ok", true}};
	EXPECT_TRUE(manager_->resolve(id, result));

	ASSERT_EQ(future.wait_for(std::chrono::milliseconds(200)), std::future_status::ready);
	EXPECT_EQ(future.get()["ok"], true);
}

TEST_F(PendingRequestManagerTest, RejectErrorContainsJsonRpcFormat) {
	auto [id, future] = manager_->registerRequest(std::chrono::milliseconds(1000));

	tlcfi::jsonrpc::RpcError error;
	error.code = -32603;
	error.message = "Internal error";
	error.data = nlohmann::json{{"details", "Something went wrong"}};

	EXPECT_TRUE(manager_->reject(id, error));

	try {
		future.get();
		FAIL() << "Expected std::runtime_error";
	} catch (const std::runtime_error& e) {
		std::string errorMsg = e.what();
		// Error should be JSON-RPC formatted
		EXPECT_NE(errorMsg.find("\"code\""), std::string::npos);
		EXPECT_NE(errorMsg.find("\"message\""), std::string::npos);
		EXPECT_NE(errorMsg.find("Internal error"), std::string::npos);
		EXPECT_NE(errorMsg.find("\"data\""), std::string::npos);
	}
}

