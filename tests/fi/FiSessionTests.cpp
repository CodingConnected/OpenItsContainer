#include <gtest/gtest.h>

#include "fi/FiSession.hpp"
#include "jsonrpc/JsonRpcSession.hpp"
#include "jsonrpc/BalancedJsonFramer.hpp"
#include "jsonrpc/test_helpers.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <thread>

using namespace tlcfi::fi;

namespace {

class DisconnectingTransport : public test_helpers::jsonrpc::MockTransport {
public:
    void asyncSend(const std::uint8_t*, std::size_t) override {
        throw std::runtime_error("Cannot send: not connected");
    }
};

} // namespace

TEST(FiSessionTests, RegisterStoresSessionAndFacilitiesReference) {
    auto transport = std::make_shared<test_helpers::jsonrpc::MockTransport>();
    auto framer = std::make_shared<test_helpers::jsonrpc::MockFramer>();
    auto rpc = std::make_shared<tlcfi::jsonrpc::JsonRpcSession>(transport, framer);

    FiSession session(rpc);
    session.start();
    
    // Start transport to trigger onConnected callback
    transport->start();
    
    // Wait a bit for async initialization
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    tlcfi::genfi::RegistrationRequest req;
    req.username = "user";
    req.password = "pw";
    req.type = 0;
    req.version = {1, 1, 0};
    req.uri = "uri";

    auto fut = session.registerApp(req, std::chrono::milliseconds(1000));

    // Wait for async request to be sent
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    auto sentStr = transport->getSentBytesAsString();
    ASSERT_FALSE(sentStr.empty()) << "No request was sent";
    
    auto sent = nlohmann::json::parse(sentStr);
    ASSERT_TRUE(sent.contains("id"));
    auto id = sent["id"];

    transport->clearSentBytes();

    tlcfi::genfi::RegistrationReply reply;
    reply.sessionid = "sess-123";
    reply.facilities.type = "TLCFacilities";
    reply.facilities.ids = {"FAC-001"};
    reply.version = {1, 1, 0};

    nlohmann::json response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", nlohmann::json(reply)}};
    transport->simulateReceive(response.dump());
    
    // Wait for async response processing
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Wait for future with timeout to avoid hanging
    auto status = fut.wait_for(std::chrono::milliseconds(100));
    ASSERT_EQ(status, std::future_status::ready) << "Future did not become ready";
    
    auto result = fut.get();
    EXPECT_EQ(result.sessionid, "sess-123");

    // readMeta should use stored facilities reference (type string + ids)
    auto metaFuture = session.readMeta(std::chrono::milliseconds(500));
    
    // Wait for async request to be sent
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    auto metaSentStr = transport->getSentBytesAsString();
    ASSERT_FALSE(metaSentStr.empty()) << "No ReadMeta request was sent";
    
    auto metaRequest = nlohmann::json::parse(metaSentStr);
    EXPECT_EQ(metaRequest["method"], "ReadMeta");
    EXPECT_EQ(metaRequest["params"]["type"], "TLCFacilities");
    ASSERT_TRUE(metaRequest["params"]["ids"].is_array());
    EXPECT_EQ(metaRequest["params"]["ids"][0], "FAC-001");
    transport->clearSentBytes();
    
    session.stop();
}

TEST(FiSessionTests, UpdateStateDoesNotThrowWhenDisconnectRacesSend) {
    auto transport = std::make_shared<DisconnectingTransport>();
    auto framer = std::make_shared<test_helpers::jsonrpc::MockFramer>();
    auto rpc = std::make_shared<tlcfi::jsonrpc::JsonRpcSession>(transport, framer);
    FiSession session(rpc);

    tlcfi::genfi::ObjectStateUpdate update;
    update.objects.type = 3;
    update.objects.ids = {"SG1"};
    update.states.push_back({{"reqState", 1}});

    tlcfi::genfi::ObjectStateUpdateGroup group;
    group.update.push_back(std::move(update));

    EXPECT_NO_THROW(session.updateState(group));
}

TEST(FiSessionTests, SubscribeParsesObjectDataSnapshot) {
    auto transport = std::make_shared<test_helpers::jsonrpc::MockTransport>();
    auto framer = std::make_shared<test_helpers::jsonrpc::MockFramer>();
    auto rpc = std::make_shared<tlcfi::jsonrpc::JsonRpcSession>(transport, framer);

    FiSession session(rpc);
    session.start();
    
    // Start transport to trigger onConnected callback
    transport->start();
    
    // Wait a bit for async initialization
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    tlcfi::genfi::ObjectReference ref;
    ref.type = 3; // SignalGroup
    ref.ids = {"SG1", "SG2"};

    auto fut = session.subscribe(ref, std::chrono::milliseconds(1000));

    // Wait for async request to be sent
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    auto sentStr = transport->getSentBytesAsString();
    ASSERT_FALSE(sentStr.empty()) << "No Subscribe request was sent";
    
    auto request = nlohmann::json::parse(sentStr);
    ASSERT_TRUE(request.contains("id"));
    auto id = request["id"];
    EXPECT_EQ(request["method"], "Subscribe");
    EXPECT_EQ(request["params"]["type"], 3);
    ASSERT_EQ(request["params"]["ids"].size(), 2);
    transport->clearSentBytes();

    nlohmann::json snapshot;
    snapshot["objects"] = {{"type", 3}, {"ids", {"SG1", "SG2"}}};
    snapshot["data"] = {
        nlohmann::json({{"id", "SG1"}, {"state", 1}}),
        nlohmann::json({{"id", "SG2"}, {"state", 2}})
    };
    snapshot["ticks"] = 1;

    nlohmann::json response = {{"jsonrpc", "2.0"}, {"id", id}, {"result", snapshot}};
    transport->simulateReceive(response.dump());
    
    // Wait for async response processing
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Wait for future with timeout to avoid hanging
    auto status = fut.wait_for(std::chrono::milliseconds(100));
    ASSERT_EQ(status, std::future_status::ready) << "Future did not become ready";
    
    auto result = fut.get();
    EXPECT_EQ(result.objects.ids.size(), 2);
    EXPECT_EQ(result.data.size(), 2);
    EXPECT_EQ(result.objects.ids[0], "SG1");
    EXPECT_EQ(result.objects.ids[1], "SG2");
    
    session.stop();
}

