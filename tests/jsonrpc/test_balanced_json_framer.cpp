#include <gtest/gtest.h>
#include "jsonrpc/BalancedJsonFramer.hpp"

#include <string>
#include <vector>

class BalancedJsonFramerTest : public ::testing::Test {
protected:
	void SetUp() override {
		framer_ = std::make_unique<tlcfi::jsonrpc::BalancedJsonFramer>();
	}

	void TearDown() override {
		framer_.reset();
	}

	std::unique_ptr<tlcfi::jsonrpc::BalancedJsonFramer> framer_;
	std::vector<std::string> receivedMessages_;
	std::vector<std::string> receivedErrors_;

	void onMessage(const std::string& message) {
		receivedMessages_.push_back(message);
	}

	void onError(const std::string& error) {
		receivedErrors_.push_back(error);
	}
};

TEST_F(BalancedJsonFramerTest, SingleCompleteObject) {
	std::string json = R"({"key":"value"})";
	std::vector<std::uint8_t> data(json.begin(), json.end());

	framer_->onBytesReceived(
		data.data(), data.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);

	ASSERT_EQ(receivedMessages_.size(), 1);
	EXPECT_EQ(receivedMessages_[0], json);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(BalancedJsonFramerTest, SingleCompleteArray) {
	std::string json = R"([1,2,3])";
	std::vector<std::uint8_t> data(json.begin(), json.end());

	framer_->onBytesReceived(
		data.data(), data.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);

	ASSERT_EQ(receivedMessages_.size(), 1);
	EXPECT_EQ(receivedMessages_[0], json);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(BalancedJsonFramerTest, BracesInsideStringIgnored) {
	std::string json = R"({"key":"value {with braces}"})";
	std::vector<std::uint8_t> data(json.begin(), json.end());

	framer_->onBytesReceived(
		data.data(), data.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);

	ASSERT_EQ(receivedMessages_.size(), 1);
	EXPECT_EQ(receivedMessages_[0], json);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(BalancedJsonFramerTest, BracketsInsideStringIgnored) {
	std::string json = R"({"key":"value [with brackets]"})";
	std::vector<std::uint8_t> data(json.begin(), json.end());

	framer_->onBytesReceived(
		data.data(), data.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);

	ASSERT_EQ(receivedMessages_.size(), 1);
	EXPECT_EQ(receivedMessages_[0], json);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(BalancedJsonFramerTest, EscapedQuotesInString) {
	std::string json = R"({"key":"value \"with quotes\""})";
	std::vector<std::uint8_t> data(json.begin(), json.end());

	framer_->onBytesReceived(
		data.data(), data.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);

	ASSERT_EQ(receivedMessages_.size(), 1);
	EXPECT_EQ(receivedMessages_[0], json);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(BalancedJsonFramerTest, EscapedBackslash) {
	std::string json = R"({"key":"value\\with\\backslashes"})";
	std::vector<std::uint8_t> data(json.begin(), json.end());

	framer_->onBytesReceived(
		data.data(), data.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);

	ASSERT_EQ(receivedMessages_.size(), 1);
	EXPECT_EQ(receivedMessages_[0], json);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(BalancedJsonFramerTest, MultipleCompleteObjects) {
	std::string json1 = R"({"key1":"value1"})";
	std::string json2 = R"({"key2":"value2"})";
	std::string combined = json1 + json2;
	std::vector<std::uint8_t> data(combined.begin(), combined.end());

	framer_->onBytesReceived(
		data.data(), data.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);

	ASSERT_EQ(receivedMessages_.size(), 2);
	EXPECT_EQ(receivedMessages_[0], json1);
	EXPECT_EQ(receivedMessages_[1], json2);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(BalancedJsonFramerTest, MultipleCompleteArrays) {
	std::string json1 = R"([1,2])";
	std::string json2 = R"([3,4])";
	std::string combined = json1 + json2;
	std::vector<std::uint8_t> data(combined.begin(), combined.end());

	framer_->onBytesReceived(
		data.data(), data.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);

	ASSERT_EQ(receivedMessages_.size(), 2);
	EXPECT_EQ(receivedMessages_[0], json1);
	EXPECT_EQ(receivedMessages_[1], json2);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(BalancedJsonFramerTest, PartialMessageBuffered) {
	std::string json = R"({"key":"value"})";
	std::string partial1 = R"({"key":)";
	std::string partial2 = R"("value"})";

	std::vector<std::uint8_t> data1(partial1.begin(), partial1.end());
	framer_->onBytesReceived(
		data1.data(), data1.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);

	EXPECT_TRUE(receivedMessages_.empty());

	std::vector<std::uint8_t> data2(partial2.begin(), partial2.end());
	framer_->onBytesReceived(
		data2.data(), data2.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);

	ASSERT_EQ(receivedMessages_.size(), 1);
	EXPECT_EQ(receivedMessages_[0], json);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(BalancedJsonFramerTest, NestedObjects) {
	std::string json = R"({"outer":{"inner":"value"}})";
	std::vector<std::uint8_t> data(json.begin(), json.end());

	framer_->onBytesReceived(
		data.data(), data.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);

	ASSERT_EQ(receivedMessages_.size(), 1);
	EXPECT_EQ(receivedMessages_[0], json);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(BalancedJsonFramerTest, NestedArrays) {
	std::string json = R"([[1,2],[3,4]])";
	std::vector<std::uint8_t> data(json.begin(), json.end());

	framer_->onBytesReceived(
		data.data(), data.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);

	ASSERT_EQ(receivedMessages_.size(), 1);
	EXPECT_EQ(receivedMessages_[0], json);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(BalancedJsonFramerTest, DeeplyNestedStructure) {
	std::string json = R"({"level1":{"level2":{"level3":{"level4":"deep"}}}})";
	std::vector<std::uint8_t> data(json.begin(), json.end());

	framer_->onBytesReceived(
		data.data(), data.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);

	ASSERT_EQ(receivedMessages_.size(), 1);
	EXPECT_EQ(receivedMessages_[0], json);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(BalancedJsonFramerTest, MixedNestedStructures) {
	std::string json = R"({"array":[1,2,3],"object":{"nested":"value"}})";
	std::vector<std::uint8_t> data(json.begin(), json.end());

	framer_->onBytesReceived(
		data.data(), data.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);

	ASSERT_EQ(receivedMessages_.size(), 1);
	EXPECT_EQ(receivedMessages_[0], json);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(BalancedJsonFramerTest, EncodeReturnsBytesUnchanged) {
	std::string json = R"({"key":"value"})";
	auto encoded = framer_->encode(json);

	std::string decoded(encoded.begin(), encoded.end());
	EXPECT_EQ(decoded, json);
}

TEST_F(BalancedJsonFramerTest, EncodeHandlesComplexJson) {
	std::string json = R"({"key":"value","array":[1,2,3],"nested":{"inner":"data"}})";
	auto encoded = framer_->encode(json);

	std::string decoded(encoded.begin(), encoded.end());
	EXPECT_EQ(decoded, json);
}

TEST_F(BalancedJsonFramerTest, MultiplePartialMessages) {
	std::string json1 = R"({"key1":"value1"})";
	std::string json2 = R"({"key2":"value2"})";
	
	// Send first object partially
	std::string partial1 = R"({"key1":)";
	std::vector<std::uint8_t> data1(partial1.begin(), partial1.end());
	framer_->onBytesReceived(
		data1.data(), data1.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);
	EXPECT_TRUE(receivedMessages_.empty());

	// Complete first object and start second
	std::string combined = R"("value1"}{"key2":)";
	std::vector<std::uint8_t> data2(combined.begin(), combined.end());
	framer_->onBytesReceived(
		data2.data(), data2.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);
	ASSERT_EQ(receivedMessages_.size(), 1);
	EXPECT_EQ(receivedMessages_[0], json1);

	// Complete second object
	std::string partial2 = R"("value2"})";
	std::vector<std::uint8_t> data3(partial2.begin(), partial2.end());
	framer_->onBytesReceived(
		data3.data(), data3.size(),
		[this](const std::string& msg) { this->onMessage(msg); },
		[this](const std::string& err) { this->onError(err); }
	);
	ASSERT_EQ(receivedMessages_.size(), 2);
	EXPECT_EQ(receivedMessages_[1], json2);
}

