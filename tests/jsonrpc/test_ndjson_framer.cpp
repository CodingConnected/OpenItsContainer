#include <gtest/gtest.h>
#include "jsonrpc/NdjsonFramer.hpp"

#include <string>
#include <vector>

// Tests for NdjsonFramer. This framer is not used by any application code
// (TLC-FI uses BalancedJsonFramer) but is provided as an alternative and must
// remain correct — see the "Message Framing" section in AGENTS.md.

class NdjsonFramerTest : public ::testing::Test {
protected:
	std::vector<std::string> receivedMessages_;
	std::vector<std::string> receivedErrors_;

	void feed(tlcfi::jsonrpc::NdjsonFramer& framer, const std::string& bytes) {
		std::vector<std::uint8_t> data(bytes.begin(), bytes.end());
		framer.onBytesReceived(
			data.data(), data.size(),
			[this](const std::string& msg) { receivedMessages_.push_back(msg); },
			[this](const std::string& err) { receivedErrors_.push_back(err); });
	}
};

TEST_F(NdjsonFramerTest, EncodeAppendsNewline) {
	tlcfi::jsonrpc::NdjsonFramer framer;
	std::string json = R"({"key":"value"})";
	auto encoded = framer.encode(json);

	EXPECT_EQ(std::string(encoded.begin(), encoded.end()), json + "\n");
}

TEST_F(NdjsonFramerTest, EncodeDecodeRoundTrip) {
	tlcfi::jsonrpc::NdjsonFramer encoder;
	tlcfi::jsonrpc::NdjsonFramer decoder;
	std::string json = R"({"method":"Alive"})";

	auto encoded = encoder.encode(json);
	feed(decoder, std::string(encoded.begin(), encoded.end()));

	ASSERT_EQ(receivedMessages_.size(), 1u);
	EXPECT_EQ(receivedMessages_[0], json);
	EXPECT_TRUE(receivedErrors_.empty());
}

TEST_F(NdjsonFramerTest, SingleLine) {
	tlcfi::jsonrpc::NdjsonFramer framer;
	feed(framer, "{\"a\":1}\n");

	ASSERT_EQ(receivedMessages_.size(), 1u);
	EXPECT_EQ(receivedMessages_[0], R"({"a":1})");
}

TEST_F(NdjsonFramerTest, MultipleLinesInOneBuffer) {
	tlcfi::jsonrpc::NdjsonFramer framer;
	feed(framer, "{\"a\":1}\n{\"b\":2}\n{\"c\":3}\n");

	ASSERT_EQ(receivedMessages_.size(), 3u);
	EXPECT_EQ(receivedMessages_[0], R"({"a":1})");
	EXPECT_EQ(receivedMessages_[1], R"({"b":2})");
	EXPECT_EQ(receivedMessages_[2], R"({"c":3})");
}

TEST_F(NdjsonFramerTest, CrlfLineEndingStripped) {
	tlcfi::jsonrpc::NdjsonFramer framer;
	feed(framer, "{\"a\":1}\r\n");

	ASSERT_EQ(receivedMessages_.size(), 1u);
	EXPECT_EQ(receivedMessages_[0], R"({"a":1})");
}

TEST_F(NdjsonFramerTest, EmptyLinesIgnored) {
	tlcfi::jsonrpc::NdjsonFramer framer;
	feed(framer, "\n\n{\"a\":1}\n\n");

	ASSERT_EQ(receivedMessages_.size(), 1u);
	EXPECT_EQ(receivedMessages_[0], R"({"a":1})");
}

TEST_F(NdjsonFramerTest, PartialLineBufferedUntilNewline) {
	tlcfi::jsonrpc::NdjsonFramer framer;

	feed(framer, "{\"key\":");
	EXPECT_TRUE(receivedMessages_.empty());

	feed(framer, "\"value\"}\n");
	ASSERT_EQ(receivedMessages_.size(), 1u);
	EXPECT_EQ(receivedMessages_[0], R"({"key":"value"})");
}

TEST_F(NdjsonFramerTest, TrailingPartialLineHeldBack) {
	tlcfi::jsonrpc::NdjsonFramer framer;
	feed(framer, "{\"a\":1}\n{\"b\":2}");

	ASSERT_EQ(receivedMessages_.size(), 1u);
	EXPECT_EQ(receivedMessages_[0], R"({"a":1})");

	feed(framer, "\n");
	ASSERT_EQ(receivedMessages_.size(), 2u);
	EXPECT_EQ(receivedMessages_[1], R"({"b":2})");
}

TEST_F(NdjsonFramerTest, OversizedLineWithoutNewlineReportsError) {
	tlcfi::jsonrpc::NdjsonFramer framer(64);  // 64-byte cap
	feed(framer, std::string(200, 'A'));

	EXPECT_TRUE(receivedMessages_.empty());
	ASSERT_EQ(receivedErrors_.size(), 1u);
}

TEST_F(NdjsonFramerTest, RecoversAfterOversizedLineError) {
	tlcfi::jsonrpc::NdjsonFramer framer(64);
	feed(framer, std::string(200, 'A'));
	ASSERT_EQ(receivedErrors_.size(), 1u);

	feed(framer, "{\"ok\":true}\n");
	ASSERT_EQ(receivedMessages_.size(), 1u);
	EXPECT_EQ(receivedMessages_[0], R"({"ok":true})");
}
