#include <gtest/gtest.h>
#include "jsonrpc/JsonRpcSession.hpp"
#include "jsonrpc/BalancedJsonFramer.hpp"
#include "jsonrpc/RpcTypes.hpp"
#include "jsonrpc/test_helpers.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <nlohmann/json.hpp>
#include <thread>

class JsonRpcSessionTest : public ::testing::Test {
protected:
	void SetUp() override {
		mockTransport_ = std::make_shared<test_helpers::jsonrpc::MockTransport>();
		mockFramer_ = std::make_shared<test_helpers::jsonrpc::MockFramer>();
		session_ = std::make_unique<tlcfi::jsonrpc::JsonRpcSession>(
			mockTransport_,
			mockFramer_
		);
	}

	void TearDown() override {
		session_.reset();
		mockFramer_.reset();
		mockTransport_.reset();
	}

	std::shared_ptr<test_helpers::jsonrpc::MockTransport> mockTransport_;
	std::shared_ptr<test_helpers::jsonrpc::MockFramer> mockFramer_;
	std::unique_ptr<tlcfi::jsonrpc::JsonRpcSession> session_;
};

TEST_F(JsonRpcSessionTest, ConstructionWiresTransportCallbacks) {
	// Callbacks should be set (we'll verify by triggering them)
	bool connectedCalled = false;
	session_->events.onConnected = [&]() { connectedCalled = true; };

	mockTransport_->start();
	EXPECT_TRUE(connectedCalled);
}

TEST_F(JsonRpcSessionTest, StartDelegatesToTransport) {
	session_->start();
	EXPECT_TRUE(mockTransport_->isStarted());
}

TEST_F(JsonRpcSessionTest, StopDelegatesToTransport) {
	session_->start();
	session_->stop();
	EXPECT_TRUE(mockTransport_->isStopped());
}

TEST_F(JsonRpcSessionTest, SendRequestCreatesValidJsonRpcRequest) {
	session_->start();

	auto future = session_->sendRequest("testMethod", nlohmann::json{{"param", "value"}}, 
	                                   std::chrono::milliseconds(1000));

	std::string sent = mockTransport_->getSentBytesAsString();
	auto sentJson = nlohmann::json::parse(sent);

	EXPECT_EQ(sentJson["jsonrpc"], "2.0");
	EXPECT_EQ(sentJson["method"], "testMethod");
	EXPECT_EQ(sentJson["params"]["param"], "value");
	EXPECT_TRUE(sentJson.contains("id"));
	EXPECT_TRUE(sentJson["id"].is_number_integer());
}

TEST_F(JsonRpcSessionTest, SendNotificationCreatesValidJsonRpcNotification) {
	session_->start();

	session_->sendNotification("notifyMethod", nlohmann::json{{"data", 123}});

	std::string sent = mockTransport_->getSentBytesAsString();
	auto sentJson = nlohmann::json::parse(sent);

	EXPECT_EQ(sentJson["jsonrpc"], "2.0");
	EXPECT_EQ(sentJson["method"], "notifyMethod");
	EXPECT_EQ(sentJson["params"]["data"], 123);
	EXPECT_FALSE(sentJson.contains("id"));
}

TEST_F(JsonRpcSessionTest, SendResponseCreatesValidJsonRpcResponse) {
	session_->start();

	tlcfi::jsonrpc::RpcId id{42};
	nlohmann::json result{{"value", "success"}};
	session_->sendResponse(id, result);

	std::string sent = mockTransport_->getSentBytesAsString();
	auto sentJson = nlohmann::json::parse(sent);

	EXPECT_EQ(sentJson["jsonrpc"], "2.0");
	EXPECT_EQ(sentJson["id"], 42);
	EXPECT_EQ(sentJson["result"]["value"], "success");
}

TEST_F(JsonRpcSessionTest, SendErrorCreatesValidJsonRpcError) {
	session_->start();

	tlcfi::jsonrpc::RpcId id{99};
	session_->sendError(id, -32601, "Method not found", nlohmann::json{{"method", "unknown"}});

	std::string sent = mockTransport_->getSentBytesAsString();
	auto sentJson = nlohmann::json::parse(sent);

	EXPECT_EQ(sentJson["jsonrpc"], "2.0");
	EXPECT_EQ(sentJson["id"], 99);
	EXPECT_EQ(sentJson["error"]["code"], -32601);
	EXPECT_EQ(sentJson["error"]["message"], "Method not found");
	EXPECT_EQ(sentJson["error"]["data"]["method"], "unknown");
}

TEST_F(JsonRpcSessionTest, IncomingRequestDispatchesToRegisteredHandler) {
	session_->start();

	bool handlerCalled = false;
	nlohmann::json handlerParams;
	nlohmann::json handlerResult{{"result", "handled"}};

	session_->registerHandler("testMethod", [&](nlohmann::json params) {
		handlerCalled = true;
		handlerParams = params;
		return handlerResult;
	});

	// Send a request
	std::string request = R"({"jsonrpc":"2.0","id":1,"method":"testMethod","params":{"key":"value"}})";
	mockTransport_->simulateReceive(request);

	// Wait a bit for async processing
	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	// Check that response was sent
	std::string sent = mockTransport_->getSentBytesAsString();
	EXPECT_FALSE(sent.empty());
	
	auto responseJson = nlohmann::json::parse(sent);
	EXPECT_EQ(responseJson["jsonrpc"], "2.0");
	EXPECT_EQ(responseJson["id"], 1);
	EXPECT_EQ(responseJson["result"], handlerResult);
}

TEST_F(JsonRpcSessionTest, IncomingRequestTriggersOnRequestEvent) {
	session_->start();

	bool eventCalled = false;
	std::string receivedMethod;
	nlohmann::json receivedParams;
	tlcfi::jsonrpc::RpcId receivedId;

	session_->events.onRequest = [&](const std::string& method, nlohmann::json params, tlcfi::jsonrpc::RpcId id) {
		eventCalled = true;
		receivedMethod = method;
		receivedParams = params;
		receivedId = id;
	};

	std::string request = R"({"jsonrpc":"2.0","id":123,"method":"eventMethod","params":{"x":1}})";
	mockTransport_->simulateReceive(request);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	EXPECT_TRUE(eventCalled);
	EXPECT_EQ(receivedMethod, "eventMethod");
	EXPECT_EQ(receivedParams["x"], 1);
	EXPECT_TRUE(std::holds_alternative<int64_t>(receivedId));
	EXPECT_EQ(std::get<int64_t>(receivedId), 123);
}

TEST_F(JsonRpcSessionTest, IncomingRequestWithoutHandlerSendsMethodNotFound) {
	session_->start();

	std::string request = R"({"jsonrpc":"2.0","id":456,"method":"unknownMethod","params":{}})";
	mockTransport_->simulateReceive(request);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	std::string sent = mockTransport_->getSentBytesAsString();
	auto errorJson = nlohmann::json::parse(sent);

	EXPECT_EQ(errorJson["jsonrpc"], "2.0");
	EXPECT_EQ(errorJson["id"], 456);
	EXPECT_EQ(errorJson["error"]["code"], -32601);
	EXPECT_EQ(errorJson["error"]["message"], "Method not found");
}

TEST_F(JsonRpcSessionTest, HandlerExceptionSendsInternalError) {
	session_->start();

	session_->registerHandler("boom", [](nlohmann::json) -> nlohmann::json {
		throw std::runtime_error("handler failed");
	});

	std::string request = R"({"jsonrpc":"2.0","id":7,"method":"boom","params":{}})";
	mockTransport_->simulateReceive(request);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	auto errorJson = nlohmann::json::parse(mockTransport_->getSentBytesAsString());
	EXPECT_EQ(errorJson["id"], 7);
	EXPECT_EQ(errorJson["error"]["code"], -32603);
	EXPECT_EQ(errorJson["error"]["message"], "Internal error");
}

TEST_F(JsonRpcSessionTest, HandlerInvalidParamsSendsInvalidParamsError) {
	session_->start();

	session_->registerHandler("needsField", [](nlohmann::json params) -> nlohmann::json {
		return params.at("requiredField");
	});

	std::string request = R"({"jsonrpc":"2.0","id":8,"method":"needsField","params":{}})";
	mockTransport_->simulateReceive(request);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	auto errorJson = nlohmann::json::parse(mockTransport_->getSentBytesAsString());
	EXPECT_EQ(errorJson["id"], 8);
	EXPECT_EQ(errorJson["error"]["code"], -32602);
	EXPECT_EQ(errorJson["error"]["message"], "Invalid params");
}

TEST_F(JsonRpcSessionTest, OnRequestExceptionSendsInternalError) {
	session_->start();

	session_->events.onRequest = [](const std::string&, nlohmann::json, tlcfi::jsonrpc::RpcId) {
		throw std::runtime_error("application handler failed");
	};

	std::string request = R"({"jsonrpc":"2.0","id":9,"method":"appMethod","params":{}})";
	mockTransport_->simulateReceive(request);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	auto errorJson = nlohmann::json::parse(mockTransport_->getSentBytesAsString());
	EXPECT_EQ(errorJson["id"], 9);
	EXPECT_EQ(errorJson["error"]["code"], -32603);
	EXPECT_EQ(errorJson["error"]["message"], "Internal error");
}

TEST_F(JsonRpcSessionTest, ParseErrorSendsNullIdErrorResponse) {
	session_->start();

	std::string invalidJson = R"({"invalid": json})";
	mockTransport_->simulateReceive(invalidJson);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	auto errorJson = nlohmann::json::parse(mockTransport_->getSentBytesAsString());
	EXPECT_TRUE(errorJson["id"].is_null());
	EXPECT_EQ(errorJson["error"]["code"], -32700);
	EXPECT_EQ(errorJson["error"]["message"], "Parse error");
}

TEST_F(JsonRpcSessionTest, ParseErrorDoesNotThrowWhenSendFails) {
	session_->start();
	mockTransport_->setRejectSends(true);

	EXPECT_NO_THROW(mockTransport_->simulateReceive(R"({"invalid": json})"));
	EXPECT_TRUE(mockTransport_->getSentBytesAsString().empty());
}

TEST_F(JsonRpcSessionTest, HandlerErrorDoesNotThrowWhenSendFails) {
	session_->start();
	mockTransport_->setRejectSends(true);

	session_->registerHandler("boom", [](nlohmann::json) -> nlohmann::json {
		throw std::runtime_error("handler failed");
	});

	EXPECT_NO_THROW(mockTransport_->simulateReceive(
		R"({"jsonrpc":"2.0","id":7,"method":"boom","params":{}})"));
	EXPECT_TRUE(mockTransport_->getSentBytesAsString().empty());
}

TEST_F(JsonRpcSessionTest, IncomingNotificationTriggersOnNotificationEvent) {
	session_->start();

	bool eventCalled = false;
	std::string receivedMethod;
	nlohmann::json receivedParams;

	session_->events.onNotification = [&](const std::string& method, nlohmann::json params) {
		eventCalled = true;
		receivedMethod = method;
		receivedParams = params;
	};

	std::string notification = R"({"jsonrpc":"2.0","method":"notify","params":{"data":"test"}})";
	mockTransport_->simulateReceive(notification);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	EXPECT_TRUE(eventCalled);
	EXPECT_EQ(receivedMethod, "notify");
	EXPECT_EQ(receivedParams["data"], "test");
}

TEST_F(JsonRpcSessionTest, IncomingResponseResolvesPendingRequest) {
	session_->start();

	auto future = session_->sendRequest("testMethod", nlohmann::json{}, std::chrono::milliseconds(1000));

	// Get the sent request to extract the ID
	std::string sentRequest = mockTransport_->getSentBytesAsString();
	mockTransport_->clearSentBytes();
	auto requestJson = nlohmann::json::parse(sentRequest);
	int64_t requestId = requestJson["id"];

	// Send response
	std::string response = R"({"jsonrpc":"2.0","id":)" + std::to_string(requestId) + R"(,"result":{"value":42}})";
	mockTransport_->simulateReceive(response);

	EXPECT_EQ(future.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
	auto result = future.get();
	EXPECT_EQ(result["value"], 42);
}

TEST_F(JsonRpcSessionTest, IncomingResponseTriggersOnResponseEvent) {
	session_->start();

	bool eventCalled = false;
	tlcfi::jsonrpc::RpcId receivedId;
	nlohmann::json receivedResult;

	session_->events.onResponse = [&](tlcfi::jsonrpc::RpcId id, nlohmann::json result) {
		eventCalled = true;
		receivedId = id;
		receivedResult = result;
	};

	// Send a response that doesn't match any pending request
	std::string response = R"({"jsonrpc":"2.0","id":9999,"result":{"unmatched":true}})";
	mockTransport_->simulateReceive(response);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	EXPECT_TRUE(eventCalled);
	EXPECT_TRUE(std::holds_alternative<int64_t>(receivedId));
	EXPECT_EQ(std::get<int64_t>(receivedId), 9999);
	EXPECT_EQ(receivedResult["unmatched"], true);
}

TEST_F(JsonRpcSessionTest, IncomingErrorResponseRejectsPendingRequest) {
	session_->start();

	auto future = session_->sendRequest("testMethod", nlohmann::json{}, std::chrono::milliseconds(1000));

	// Get the sent request to extract the ID
	std::string sentRequest = mockTransport_->getSentBytesAsString();
	mockTransport_->clearSentBytes();
	auto requestJson = nlohmann::json::parse(sentRequest);
	int64_t requestId = requestJson["id"];

	// Send error response
	std::string errorResponse = R"({"jsonrpc":"2.0","id":)" + std::to_string(requestId) + 
	                           R"(,"error":{"code":-32603,"message":"Internal error"}})";
	mockTransport_->simulateReceive(errorResponse);

	EXPECT_EQ(future.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
	EXPECT_THROW(future.get(), std::runtime_error);
}

TEST_F(JsonRpcSessionTest, IncomingErrorResponseTriggersOnErrorResponseEvent) {
	session_->start();

	bool eventCalled = false;
	tlcfi::jsonrpc::RpcId receivedId;
	tlcfi::jsonrpc::RpcError receivedError;

	session_->events.onErrorResponse = [&](tlcfi::jsonrpc::RpcId id, const tlcfi::jsonrpc::RpcError& error) {
		eventCalled = true;
		receivedId = id;
		receivedError = error;
	};

	// Send an error response that doesn't match any pending request
	std::string errorResponse = R"({"jsonrpc":"2.0","id":8888,"error":{"code":-1,"message":"Test error"}})";
	mockTransport_->simulateReceive(errorResponse);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	EXPECT_TRUE(eventCalled);
	EXPECT_TRUE(std::holds_alternative<int64_t>(receivedId));
	EXPECT_EQ(std::get<int64_t>(receivedId), 8888);
	EXPECT_EQ(receivedError.code, -1);
	EXPECT_EQ(receivedError.message, "Test error");
}

TEST_F(JsonRpcSessionTest, BatchMessagesHandled) {
	session_->start();

	int requestCount = 0;
	int notificationCount = 0;

	session_->events.onRequest = [&](const std::string&, nlohmann::json, tlcfi::jsonrpc::RpcId) {
		requestCount++;
	};

	session_->events.onNotification = [&](const std::string&, nlohmann::json) {
		notificationCount++;
	};

	std::string batch = R"([
		{"jsonrpc":"2.0","id":1,"method":"method1","params":{}},
		{"jsonrpc":"2.0","method":"notify1","params":{}},
		{"jsonrpc":"2.0","id":2,"method":"method2","params":{}}
	])";
	mockTransport_->simulateReceive(batch);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	EXPECT_EQ(requestCount, 2);
	EXPECT_EQ(notificationCount, 1);
}

TEST_F(JsonRpcSessionTest, InvalidJsonTriggersProtocolError) {
	session_->start();

	bool errorCalled = false;
	std::string errorReason;

	session_->events.onProtocolError = [&](const std::string& reason) {
		errorCalled = true;
		errorReason = reason;
	};

	std::string invalidJson = R"({"invalid": json})";
	mockTransport_->simulateReceive(invalidJson);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	EXPECT_TRUE(errorCalled);
	EXPECT_NE(errorReason.find("Parse error"), std::string::npos);
}

TEST_F(JsonRpcSessionTest, MissingJsonRpcVersionTriggersProtocolError) {
	session_->start();

	bool errorCalled = false;

	session_->events.onProtocolError = [&](const std::string&) {
		errorCalled = true;
	};

	std::string invalidMessage = R"({"method":"test","id":1})";
	mockTransport_->simulateReceive(invalidMessage);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	EXPECT_TRUE(errorCalled);
}

TEST_F(JsonRpcSessionTest, InvalidRequestResponseShapeTriggersProtocolError) {
	session_->start();

	bool errorCalled = false;

	session_->events.onProtocolError = [&](const std::string&) {
		errorCalled = true;
	};

	// Message with both result and error (invalid)
	std::string invalidMessage = R"({"jsonrpc":"2.0","id":1,"result":{},"error":{"code":-1}})";
	mockTransport_->simulateReceive(invalidMessage);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	EXPECT_TRUE(errorCalled);
}

TEST_F(JsonRpcSessionTest, OnConnectedEventPropagates) {
	bool connectedCalled = false;
	session_->events.onConnected = [&]() { connectedCalled = true; };

	session_->start();

	EXPECT_TRUE(connectedCalled);
}

TEST_F(JsonRpcSessionTest, OnDisconnectedEventPropagates) {
	bool disconnectedCalled = false;
	std::string disconnectReason;

	session_->events.onDisconnected = [&](const std::string& reason) {
		disconnectedCalled = true;
		disconnectReason = reason;
	};

	session_->start();
	session_->stop();

	EXPECT_TRUE(disconnectedCalled);
	EXPECT_FALSE(disconnectReason.empty());
}

TEST_F(JsonRpcSessionTest, DisconnectFailsAllPendingRequests) {
	session_->start();

	auto future1 = session_->sendRequest("method1", nlohmann::json{}, std::chrono::milliseconds(1000));
	auto future2 = session_->sendRequest("method2", nlohmann::json{}, std::chrono::milliseconds(1000));

	session_->stop();

	EXPECT_EQ(future1.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
	EXPECT_EQ(future2.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);

	EXPECT_THROW(future1.get(), std::runtime_error);
	EXPECT_THROW(future2.get(), std::runtime_error);
}

TEST_F(JsonRpcSessionTest, OnTraceCallbackInvoked) {
	session_->start();

	std::vector<std::pair<std::string, std::string>> traces;
	session_->onTrace = [&](const char* direction, const std::string& text) {
		traces.push_back({direction, text});
	};

	session_->sendNotification("test", nlohmann::json{});

	std::string sent = mockTransport_->getSentBytesAsString();
	EXPECT_FALSE(traces.empty());
	EXPECT_EQ(traces[0].first, "send");
	EXPECT_FALSE(traces[0].second.empty());

	traces.clear();
	std::string request = R"({"jsonrpc":"2.0","method":"test","params":{}})";
	mockTransport_->simulateReceive(request);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	EXPECT_FALSE(traces.empty());
	EXPECT_EQ(traces[0].first, "recv");
}

TEST_F(JsonRpcSessionTest, OnTraceRedactsPasswordOnSend) {
	session_->start();

	std::string sendTrace;
	session_->onTrace = [&](const char* direction, const std::string& text) {
		if (std::string(direction) == "send") sendTrace += text;
	};

	// A registration-style payload carrying credentials.
	session_->sendNotification("RegisterParticipant",
		nlohmann::json{{"username", "ctrl"}, {"password", "s3cr3t-pw"}});

	// The trace must not leak the password, and must show the mask instead.
	EXPECT_EQ(sendTrace.find("s3cr3t-pw"), std::string::npos)
		<< "password leaked into send trace: " << sendTrace;
	EXPECT_NE(sendTrace.find(R"("password":"***")"), std::string::npos)
		<< "password not redacted in send trace: " << sendTrace;
	// Username is intentionally preserved for debuggability.
	EXPECT_NE(sendTrace.find("ctrl"), std::string::npos);

	// The bytes actually sent on the wire must still carry the real password:
	// redaction applies to the traced copy only, never to what is transmitted.
	std::string wire = mockTransport_->getSentBytesAsString();
	EXPECT_NE(wire.find("s3cr3t-pw"), std::string::npos)
		<< "redaction must not alter the bytes sent on the wire: " << wire;
}

TEST_F(JsonRpcSessionTest, OnTraceRedactsPasswordOnReceive) {
	session_->start();

	std::string recvTrace;
	session_->onTrace = [&](const char* direction, const std::string& text) {
		if (std::string(direction) == "recv") recvTrace += text;
	};

	std::string incoming =
		R"({"jsonrpc":"2.0","id":1,"method":"RegisterParticipant","params":{"username":"ctrl","password":"s3cr3t-pw"}})";
	mockTransport_->simulateReceive(incoming);

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	EXPECT_FALSE(recvTrace.empty());
	EXPECT_EQ(recvTrace.find("s3cr3t-pw"), std::string::npos)
		<< "password leaked into recv trace: " << recvTrace;
	EXPECT_NE(recvTrace.find(R"("password":"***")"), std::string::npos)
		<< "password not redacted in recv trace: " << recvTrace;
}

TEST_F(JsonRpcSessionTest, SendRawJsonText) {
	session_->start();

	std::string rawJson = R"({"custom":"format"})";
	session_->sendRawJsonText(rawJson);

	std::string sent = mockTransport_->getSentBytesAsString();
	EXPECT_EQ(sent, rawJson);
}

TEST_F(JsonRpcSessionTest, StringIdInRequest) {
	session_->start();

	std::string request = R"({"jsonrpc":"2.0","id":"string-id","method":"test","params":{}})";
	mockTransport_->simulateReceive(request);

	bool responseReceived = false;
	session_->events.onResponse = [&](tlcfi::jsonrpc::RpcId id, nlohmann::json) {
		EXPECT_TRUE(std::holds_alternative<std::string>(id));
		EXPECT_EQ(std::get<std::string>(id), "string-id");
		responseReceived = true;
	};

	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	// Should send method not found error with string ID
	std::string sent = mockTransport_->getSentBytesAsString();
	auto errorJson = nlohmann::json::parse(sent);
	EXPECT_EQ(errorJson["id"], "string-id");
}

TEST_F(JsonRpcSessionTest, NullIdInNotification) {
	session_->start();

	std::string notification = R"({"jsonrpc":"2.0","id":null,"method":"test","params":{}})";
	
	bool requestReceived = false;
	session_->events.onRequest = [&](const std::string&, nlohmann::json, tlcfi::jsonrpc::RpcId id) {
		EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(id));
		requestReceived = true;
	};

	mockTransport_->simulateReceive(notification);
	std::this_thread::sleep_for(std::chrono::milliseconds(10));

	EXPECT_TRUE(requestReceived);
}

