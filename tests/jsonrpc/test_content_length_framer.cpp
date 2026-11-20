#include <gtest/gtest.h>
#include "jsonrpc/ContentLengthFramer.hpp"

#include <string>
#include <vector>

// Tests for ContentLengthFramer. This framer is not used by any application
// code (TLC-FI uses BalancedJsonFramer) but is provided as an alternative and
// must remain correct — see the "Message Framing" section in AGENTS.md.

class ContentLengthFramerTest : public ::testing::Test {
protected:
	std::vector<std::string> receivedMessages_;
	std::vector<std::string> receivedErrors_;

	// Feed a string of bytes into the given framer.
	void feed(tlcfi::jsonrpc::ContentLengthFramer& framer, const std::string& bytes) {
		std::vector<std::uint8_t> data(bytes.begin(), bytes.end());
		framer.onBytesReceived(
			data.data(), data.size(),
			[this](const std::string& msg) { receivedMessages_.push_back(msg); },
			[this](const std::string& err) { receivedErrors_.push_back(err); });
	}
};

TEST_F(ContentLengthFramerTest, EncodeProducesContentLengthHeader) {
	tlcfi::jsonrpc::ContentLengthFramer framer;
	std::string json = R"({"key":"value"})";
	auto encoded = framer.encode(json);
	std::string decoded(encoded.begin(), encoded.end());

	EXPECT_EQ(decoded, "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json);
}

TEST_F(ContentLengthFramerTest, EncodeDecodeRoundTrip) {
	tlcfi::jsonrpc::ContentLengthFramer encoder;
	tlcfi::jsonrpc::ContentLengthFramer decoder;
	std::string json = R"({"method":"Alive","params":{"ticks":42}})";

	auto encoded = encoder.encode(json);
	feed(decoder, std::string(encoded.begin(), encoded.end()));

	ASSERT_EQ(receivedMessages_.size(), 1u);
	EXPECT_EQ(receivedMessages_[0], json);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(ContentLengthFramerTest, MultipleMessagesInOneBuffer) {
	tlcfi::jsonrpc::ContentLengthFramer framer;
	std::string a = R"({"a":1})";
	std::string b = R"({"b":2})";
	std::string buf = "Content-Length: " + std::to_string(a.size()) + "\r\n\r\n" + a +
	                  "Content-Length: " + std::to_string(b.size()) + "\r\n\r\n" + b;
	feed(framer, buf);

	ASSERT_EQ(receivedMessages_.size(), 2u);
	EXPECT_EQ(receivedMessages_[0], a);
	EXPECT_EQ(receivedMessages_[1], b);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(ContentLengthFramerTest, PartialHeaderThenRest) {
	tlcfi::jsonrpc::ContentLengthFramer framer;
	std::string json = R"({"k":"v"})";

	feed(framer, "Content-Length: " + std::to_string(json.size()));
	EXPECT_TRUE(receivedMessages_.empty());

	feed(framer, "\r\n\r\n" + json);
	ASSERT_EQ(receivedMessages_.size(), 1u);
	EXPECT_EQ(receivedMessages_[0], json);
}

TEST_F(ContentLengthFramerTest, PartialBodyThenRest) {
	tlcfi::jsonrpc::ContentLengthFramer framer;
	std::string json = R"({"hello":"world"})";

	feed(framer, "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json.substr(0, 5));
	EXPECT_TRUE(receivedMessages_.empty());

	feed(framer, json.substr(5));
	ASSERT_EQ(receivedMessages_.size(), 1u);
	EXPECT_EQ(receivedMessages_[0], json);
}

TEST_F(ContentLengthFramerTest, MissingContentLengthHeaderReportsError) {
	tlcfi::jsonrpc::ContentLengthFramer framer;
	feed(framer, "X-Other: 1\r\n\r\n{}");

	EXPECT_TRUE(receivedMessages_.empty());
	ASSERT_EQ(receivedErrors_.size(), 1u);
}

TEST_F(ContentLengthFramerTest, InvalidContentLengthValueReportsError) {
	tlcfi::jsonrpc::ContentLengthFramer framer;
	feed(framer, "Content-Length: notanumber\r\n\r\n{}");

	EXPECT_TRUE(receivedMessages_.empty());
	ASSERT_EQ(receivedErrors_.size(), 1u);
}

TEST_F(ContentLengthFramerTest, ContentLengthExceedingMaxReportsError) {
	tlcfi::jsonrpc::ContentLengthFramer framer(64);  // 64-byte cap
	feed(framer, "Content-Length: 1000000\r\n\r\n");

	EXPECT_TRUE(receivedMessages_.empty());
	ASSERT_EQ(receivedErrors_.size(), 1u);
}

TEST_F(ContentLengthFramerTest, UnterminatedHeaderExceedingMaxReportsError) {
	tlcfi::jsonrpc::ContentLengthFramer framer(32);  // 32-byte cap
	// Header block that never terminates with \r\n\r\n.
	feed(framer, std::string(100, 'A'));

	EXPECT_TRUE(receivedMessages_.empty());
	ASSERT_EQ(receivedErrors_.size(), 1u);
}

TEST_F(ContentLengthFramerTest, ZeroLengthBodyEmitsEmptyMessage) {
	tlcfi::jsonrpc::ContentLengthFramer framer;
	feed(framer, "Content-Length: 0\r\n\r\n");

	ASSERT_EQ(receivedMessages_.size(), 1u);
	EXPECT_TRUE(receivedMessages_[0].empty());
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(ContentLengthFramerTest, RecoversAfterMissingHeaderError) {
	tlcfi::jsonrpc::ContentLengthFramer framer;
	feed(framer, "Bad: header\r\n\r\nignored");
	ASSERT_EQ(receivedErrors_.size(), 1u);

	std::string json = R"({"ok":true})";
	feed(framer, "Content-Length: " + std::to_string(json.size()) + "\r\n\r\n" + json);
	ASSERT_EQ(receivedMessages_.size(), 1u);
	EXPECT_EQ(receivedMessages_[0], json);
}
