#include <gtest/gtest.h>
#include "jsonrpc/JsonRpcDispatcher.hpp"
#include "jsonrpc/RpcTypes.hpp"

#include <nlohmann/json.hpp>

class JsonRpcDispatcherTest : public ::testing::Test {
protected:
	void SetUp() override {
		dispatcher_ = std::make_unique<tlcfi::jsonrpc::JsonRpcDispatcher>();
	}

	void TearDown() override {
		dispatcher_.reset();
	}

	std::unique_ptr<tlcfi::jsonrpc::JsonRpcDispatcher> dispatcher_;
};

TEST_F(JsonRpcDispatcherTest, RegisterHandler) {
	bool handlerCalled = false;
	nlohmann::json receivedParams;

	tlcfi::jsonrpc::JsonRpcDispatcher::Handler handler = [&](nlohmann::json params) {
		handlerCalled = true;
		receivedParams = params;
		return nlohmann::json{{"result", "success"}};
	};

	dispatcher_->registerHandler("testMethod", handler);

	// Verify handler is registered by trying to handle a request
	tlcfi::jsonrpc::RpcId id{1};
	auto result = dispatcher_->tryHandleRequest("testMethod", nlohmann::json{{"param", "value"}}, id);

	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(handlerCalled);
	EXPECT_EQ(receivedParams["param"], "value");
	EXPECT_EQ((*result)["result"], "success");
}

TEST_F(JsonRpcDispatcherTest, UnregisterHandler) {
	bool handlerCalled = false;

	tlcfi::jsonrpc::JsonRpcDispatcher::Handler handler = [&](nlohmann::json) {
		handlerCalled = true;
		return nlohmann::json{{"result", "success"}};
	};

	dispatcher_->registerHandler("testMethod", handler);
	dispatcher_->unregisterHandler("testMethod");

	// Verify handler is removed
	tlcfi::jsonrpc::RpcId id{1};
	auto result = dispatcher_->tryHandleRequest("testMethod", nlohmann::json{}, id);

	EXPECT_FALSE(result.has_value());
	EXPECT_FALSE(handlerCalled);
}

TEST_F(JsonRpcDispatcherTest, HandlerReplacement) {
	int callCount = 0;

	tlcfi::jsonrpc::JsonRpcDispatcher::Handler handler1 = [&](nlohmann::json) {
		callCount = 1;
		return nlohmann::json{{"result", "first"}};
	};

	tlcfi::jsonrpc::JsonRpcDispatcher::Handler handler2 = [&](nlohmann::json) {
		callCount = 2;
		return nlohmann::json{{"result", "second"}};
	};

	dispatcher_->registerHandler("testMethod", handler1);
	dispatcher_->registerHandler("testMethod", handler2); // Replace handler

	tlcfi::jsonrpc::RpcId id{1};
	auto result = dispatcher_->tryHandleRequest("testMethod", nlohmann::json{}, id);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ(callCount, 2);
	EXPECT_EQ((*result)["result"], "second");
}

TEST_F(JsonRpcDispatcherTest, TryHandleRequestWithHandler) {
	nlohmann::json expectedResult{{"sum", 5}};

	tlcfi::jsonrpc::JsonRpcDispatcher::Handler handler = [&](nlohmann::json params) {
		int a = params["a"];
		int b = params["b"];
		return nlohmann::json{{"sum", a + b}};
	};

	dispatcher_->registerHandler("add", handler);

	tlcfi::jsonrpc::RpcId id{42};
	auto result = dispatcher_->tryHandleRequest("add", nlohmann::json{{"a", 2}, {"b", 3}}, id);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ((*result)["sum"], 5);
}

TEST_F(JsonRpcDispatcherTest, TryHandleRequestWithoutHandler) {
	tlcfi::jsonrpc::RpcId id{1};
	auto result = dispatcher_->tryHandleRequest("nonexistent", nlohmann::json{{"param", "value"}}, id);

	EXPECT_FALSE(result.has_value());
}

TEST_F(JsonRpcDispatcherTest, HandlerReceivesCorrectParameters) {
	nlohmann::json receivedParams;

	tlcfi::jsonrpc::JsonRpcDispatcher::Handler handler = [&](nlohmann::json params) {
		receivedParams = params;
		return nlohmann::json{{"status", "ok"}};
	};

	dispatcher_->registerHandler("testMethod", handler);

	nlohmann::json inputParams{
		{"stringParam", "test"},
		{"numberParam", 42},
		{"boolParam", true},
		{"arrayParam", {1, 2, 3}},
		{"objectParam", {{"nested", "value"}}}
	};

	tlcfi::jsonrpc::RpcId id{123};
	dispatcher_->tryHandleRequest("testMethod", inputParams, id);

	EXPECT_EQ(receivedParams["stringParam"], "test");
	EXPECT_EQ(receivedParams["numberParam"], 42);
	EXPECT_EQ(receivedParams["boolParam"], true);
	EXPECT_EQ(receivedParams["arrayParam"], inputParams["arrayParam"]);
	EXPECT_EQ(receivedParams["objectParam"], inputParams["objectParam"]);
}

TEST_F(JsonRpcDispatcherTest, MultipleHandlers) {
	int handler1Called = 0;
	int handler2Called = 0;

	tlcfi::jsonrpc::JsonRpcDispatcher::Handler handler1 = [&](nlohmann::json) {
		handler1Called++;
		return nlohmann::json{{"method", "one"}};
	};

	tlcfi::jsonrpc::JsonRpcDispatcher::Handler handler2 = [&](nlohmann::json) {
		handler2Called++;
		return nlohmann::json{{"method", "two"}};
	};

	dispatcher_->registerHandler("method1", handler1);
	dispatcher_->registerHandler("method2", handler2);

	tlcfi::jsonrpc::RpcId id1{1};
	auto result1 = dispatcher_->tryHandleRequest("method1", nlohmann::json{}, id1);
	ASSERT_TRUE(result1.has_value());
	EXPECT_EQ((*result1)["method"], "one");
	EXPECT_EQ(handler1Called, 1);
	EXPECT_EQ(handler2Called, 0);

	tlcfi::jsonrpc::RpcId id2{2};
	auto result2 = dispatcher_->tryHandleRequest("method2", nlohmann::json{}, id2);
	ASSERT_TRUE(result2.has_value());
	EXPECT_EQ((*result2)["method"], "two");
	EXPECT_EQ(handler1Called, 1);
	EXPECT_EQ(handler2Called, 1);
}

TEST_F(JsonRpcDispatcherTest, HandlerReturnsNullJson) {
	tlcfi::jsonrpc::JsonRpcDispatcher::Handler handler = [](nlohmann::json) {
		return nlohmann::json(nullptr);
	};

	dispatcher_->registerHandler("nullMethod", handler);

	tlcfi::jsonrpc::RpcId id{1};
	auto result = dispatcher_->tryHandleRequest("nullMethod", nlohmann::json{}, id);

	ASSERT_TRUE(result.has_value());
	EXPECT_TRUE(result->is_null());
}

TEST_F(JsonRpcDispatcherTest, HandlerReturnsComplexJson) {
	tlcfi::jsonrpc::JsonRpcDispatcher::Handler handler = [](nlohmann::json) {
		return nlohmann::json{
			{"status", "success"},
			{"data", {
				{"items", {1, 2, 3}},
				{"metadata", {{"count", 3}}}
			}}
		};
	};

	dispatcher_->registerHandler("complexMethod", handler);

	tlcfi::jsonrpc::RpcId id{1};
	auto result = dispatcher_->tryHandleRequest("complexMethod", nlohmann::json{}, id);

	ASSERT_TRUE(result.has_value());
	EXPECT_EQ((*result)["status"], "success");
	EXPECT_EQ((*result)["data"]["items"], (nlohmann::json{1, 2, 3}));
	EXPECT_EQ((*result)["data"]["metadata"]["count"], 3);
}

