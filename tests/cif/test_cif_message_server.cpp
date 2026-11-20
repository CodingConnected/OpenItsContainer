#include <gtest/gtest.h>

#include "cif/CifMessageServer.hpp"
#include "cif/CifMessageServerConfig.hpp"

#include <asio.hpp>

#include <array>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Integration tests for CifMessageServer — the TCP bridge over the CIF
// IBER/UBER ring buffers. Each test owns standalone ring buffers (not the
// global CIF arrays) so the framing/wraparound/overflow logic can be exercised
// in isolation. Real TCP clients connect over localhost.

using cif::CifMessageServer;
using cif::CifMessageServerConfig;
using asio::ip::tcp;

namespace {

// Standalone CIF ring buffers plus the read/write pointer variables the server
// dereferences via char** in its config.
struct Rings {
	std::vector<char> dataIn;
	std::vector<char> dataOut;
	char* inRead{};
	char* inWrite{};
	char* outRead{};
	char* outWrite{};
	std::mutex cifMutex;

	Rings(int inSize, int outSize)
		: dataIn(static_cast<std::size_t>(inSize), 0)
		, dataOut(static_cast<std::size_t>(outSize), 0) {
		inRead = inWrite = dataIn.data();
		outRead = outWrite = dataOut.data();
	}

	CifMessageServerConfig config(unsigned short port) {
		CifMessageServerConfig c{};
		c.port = port;
		c.data_in_size = static_cast<int>(dataIn.size());
		c.data_out_size = static_cast<int>(dataOut.size());
		c.data_in = dataIn.data();
		c.data_out = dataOut.data();
		c.data_in_read_ptr = &inRead;
		c.data_in_write_ptr = &inWrite;
		c.data_out_read_ptr = &outRead;
		c.data_out_write_ptr = &outWrite;
		c.cif_mutex = &cifMutex;
		return c;
	}

	// Bytes from `start` to `end` walking the ring with wraparound.
	std::string slice(const std::vector<char>& buf, char* start, char* end) const {
		std::string s;
		const char* base = buf.data();
		const char* p = start;
		while (p != end) {
			s += *p;
			if (++p >= base + buf.size()) {
				p = base;
			}
		}
		return s;
	}
};

// Minimal blocking TCP client with a bounded async read.
struct TestClient {
	asio::io_context io;
	tcp::socket socket{io};

	bool connect(unsigned short port) {
		std::error_code ec;
		socket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port), ec);
		return !ec;
	}

	void send(const std::string& s) {
		asio::write(socket, asio::buffer(s));
	}

	// Read whatever arrives within `timeout`; returns "" on timeout.
	std::string readWithin(std::chrono::milliseconds timeout) {
		std::string result;
		std::array<char, 512> buf{};
		socket.async_read_some(asio::buffer(buf),
			[&](std::error_code ec, std::size_t n) {
				if (!ec) result.assign(buf.data(), n);
			});
		io.restart();
		io.run_for(timeout);
		return result;
	}

	bool waitForDisconnect(std::chrono::milliseconds timeout) {
		bool disconnected = false;
		std::array<char, 64> buf{};
		// Keep reading until EOF/RST or timeout. A single async_read_some is not
		// enough: the server may send buffered data before closing, which completes
		// the read with success rather than an error.
		std::function<void()> doRead = [&]() {
			socket.async_read_some(asio::buffer(buf),
				[&](std::error_code ec, std::size_t) {
					if (ec == asio::error::eof || ec == asio::error::connection_reset) {
						disconnected = true;
					} else if (!ec) {
						doRead(); // data arrived before disconnect; keep reading
					}
					// other errors (operation_aborted, etc.) — stop
				});
		};
		doRead();
		io.restart();
		io.run_for(timeout);
		if (!disconnected) {
			std::error_code cancelEc;
			socket.cancel(cancelEc);
			io.restart();
			io.run();
		}
		return disconnected;
	}
};

// Place a complete \r\n-terminated message into the dataOut ring starting at
// the current outWrite, advancing outWrite (with wraparound).
void pushToDataOut(Rings& r, const std::string& msgWithCrlf) {
	char* base = r.dataOut.data();
	char* end = base + r.dataOut.size();
	char* w = r.outWrite;
	for (char c : msgWithCrlf) {
		*w = c;
		if (++w >= end) w = base;
	}
	r.outWrite = w;
}

constexpr auto kPoll = std::chrono::milliseconds(1000);

} // namespace

TEST(CifMessageServerTest, DefaultsToLoopbackOnlyBind) {
	Rings rings(64, 64);
	EXPECT_EQ(rings.config(12749).bind_address, "127.0.0.1");
}

TEST(CifMessageServerTest, RejectsInvalidBindAddress) {
	Rings rings(64, 64);
	auto config = rings.config(12749);
	config.bind_address = "not-an-ip-address";
	CifMessageServer server(config);

	EXPECT_THROW(server.start(), std::invalid_argument);
}

TEST(CifMessageServerTest, RefusesConnectionWhenClientsDisabledAndLegacyFlagSet) {
	Rings rings(64, 64);
	auto config = rings.config(12750);
	config.cifClientsOnlyDuringControl = true;
	CifMessageServer server(config);
	server.start();
	// enableClients() deliberately NOT called.

	TestClient client;
	ASSERT_TRUE(client.connect(12750));
	std::string msg = client.readWithin(kPoll);

	EXPECT_NE(msg.find("refused"), std::string::npos)
		<< "expected a refusal banner, got: " << msg;

	server.stop();
}

TEST(CifMessageServerTest, AcceptsConnectionBeforeAndAfterControlLoop) {
	// Default: cifClientsOnlyDuringControl = false — clients can connect at any time.
	Rings rings(64, 64);
	CifMessageServer server(rings.config(12765));
	server.start();
	// enableClients() deliberately NOT called yet.

	TestClient client;
	ASSERT_TRUE(client.connect(12765));
	// No refusal — connection should stay open.
	std::string unexpected = client.readWithin(std::chrono::milliseconds(200));
	EXPECT_TRUE(unexpected.empty())
		<< "expected no banner on early connect, got: " << unexpected;

	server.stop();
}

TEST(CifMessageServerTest, InactiveMessageSentWhenLineReceivedWhileControllerNotRunning) {
	Rings rings(64, 64);
	auto config = rings.config(12766);
	config.inactiveMessage = "CIF controller not running\r\n";
	CifMessageServer server(config);
	server.start();
	// enableClients() NOT called — controller not running.

	TestClient client;
	ASSERT_TRUE(client.connect(12766));
	client.send("STATUS\r\n");

	std::string response = client.readWithin(kPoll);
	EXPECT_EQ(response, "CIF controller not running\r\n");

	// Nothing should have been written to the ring.
	{
		std::lock_guard<std::mutex> lock(rings.cifMutex);
		EXPECT_EQ(rings.inWrite, rings.dataIn.data());
	}

	server.stop();
}

TEST(CifMessageServerTest, KeepsClientConnectedOnDisableClients) {
	// Default cifClientsOnlyDuringControl = false: disableClients() must not
	// close existing sessions.
	Rings rings(64, 64);
	CifMessageServer server(rings.config(12767));
	server.start();
	server.enableClients();

	TestClient client;
	ASSERT_TRUE(client.connect(12767));
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	server.disableClients();

	// Client must remain connected — waitForDisconnect should time out.
	EXPECT_FALSE(client.waitForDisconnect(std::chrono::milliseconds(400)))
		<< "client should not be disconnected by disableClients()";

	server.stop();
}

TEST(CifMessageServerTest, InboundMessageWrittenToDataIn) {
	Rings rings(64, 64);
	CifMessageServer server(rings.config(12751));
	server.start();
	server.enableClients();

	TestClient client;
	ASSERT_TRUE(client.connect(12751));
	client.send("HELLO\n");

	// Wait for the server io thread to process the line.
	std::string content;
	for (int i = 0; i < 100; ++i) {
		{
			std::lock_guard<std::mutex> lock(rings.cifMutex);
			content = rings.slice(rings.dataIn, rings.dataIn.data(), rings.inWrite);
		}
		if (!content.empty()) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	EXPECT_EQ(content, "HELLO\r") << "CIF_IBER should hold the line + \\r terminator";

	server.stop();
}

TEST(CifMessageServerTest, OutboundMessageBroadcastToClient) {
	Rings rings(64, 64);
	CifMessageServer server(rings.config(12752));
	server.start();
	server.enableClients();

	TestClient client;
	ASSERT_TRUE(client.connect(12752));
	// Let the server register the accepted session.
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	pushToDataOut(rings, "WORLD\r\n");
	server.pollDataOut();

	EXPECT_EQ(client.readWithin(kPoll), "WORLD\r\n");

	server.stop();
}

TEST(CifMessageServerTest, MultiClientBroadcast) {
	Rings rings(64, 64);
	CifMessageServer server(rings.config(12753));
	server.start();
	server.enableClients();

	TestClient a, b;
	ASSERT_TRUE(a.connect(12753));
	ASSERT_TRUE(b.connect(12753));
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	pushToDataOut(rings, "PING\r\n");
	server.pollDataOut();

	EXPECT_EQ(a.readWithin(kPoll), "PING\r\n");
	EXPECT_EQ(b.readWithin(kPoll), "PING\r\n");

	server.stop();
}

TEST(CifMessageServerTest, OutboundRingWraparound) {
	// 8-byte dataOut ring; place "AB\r\n" so it straddles the buffer end.
	Rings rings(64, 8);
	rings.outRead = rings.dataOut.data() + 6;
	rings.outWrite = rings.dataOut.data() + 6;

	CifMessageServer server(rings.config(12754));
	server.start();
	server.enableClients();

	TestClient client;
	ASSERT_TRUE(client.connect(12754));
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	pushToDataOut(rings, "AB\r\n");  // writes idx 6,7 then wraps to 0,1
	ASSERT_EQ(rings.outWrite, rings.dataOut.data() + 2);
	server.pollDataOut();

	EXPECT_EQ(client.readWithin(kPoll), "AB\r\n");

	server.stop();
}

TEST(CifMessageServerTest, InboundRingWraparound) {
	// 10-byte dataIn ring; start the write pointer near the end so the
	// inbound message wraps.
	Rings rings(10, 64);
	rings.inRead = rings.dataIn.data() + 8;
	rings.inWrite = rings.dataIn.data() + 8;

	CifMessageServer server(rings.config(12755));
	server.start();
	server.enableClients();

	TestClient client;
	ASSERT_TRUE(client.connect(12755));
	client.send("HEY\n");

	std::string content;
	for (int i = 0; i < 100; ++i) {
		{
			std::lock_guard<std::mutex> lock(rings.cifMutex);
			if (rings.inWrite != rings.dataIn.data() + 8) {
				content = rings.slice(rings.dataIn, rings.dataIn.data() + 8, rings.inWrite);
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	EXPECT_EQ(content, "HEY\r");

	server.stop();
}

TEST(CifMessageServerTest, InboundRingFullDropsMessage) {
	// 8-byte dataIn ring pre-filled so only 1 byte is free; an oversized
	// inbound message must be dropped and the write pointer left untouched.
	Rings rings(8, 64);
	rings.inRead = rings.dataIn.data();
	rings.inWrite = rings.dataIn.data() + 6;  // used=6, free = 8-1-6 = 1

	CifMessageServer server(rings.config(12756));
	server.start();
	server.enableClients();

	TestClient client;
	ASSERT_TRUE(client.connect(12756));
	client.send("TOOLONG\n");  // 7 chars + \r = 8 needed, only 1 free

	// Give the server time to process and drop the line.
	std::this_thread::sleep_for(std::chrono::milliseconds(300));

	{
		std::lock_guard<std::mutex> lock(rings.cifMutex);
		EXPECT_EQ(rings.inWrite, rings.dataIn.data() + 6)
			<< "write pointer must not move when the ring cannot fit the message";
	}

	server.stop();
}

TEST(CifMessageServerTest, DisconnectsClientWhoseInboundLineExceedsLimit) {
	Rings rings(64, 64);
	auto config = rings.config(12757);
	config.max_inbound_line_size = 8;
	CifMessageServer server(config);
	server.start();
	server.enableClients();

	TestClient client;
	ASSERT_TRUE(client.connect(12757));
	client.send("123456789");

	EXPECT_TRUE(client.waitForDisconnect(kPoll));
	{
		std::lock_guard<std::mutex> lock(rings.cifMutex);
		EXPECT_EQ(rings.inWrite, rings.dataIn.data());
	}

	server.stop();
}

TEST(CifMessageServerTest, RefusesConnectionsBeyondSessionLimit) {
	Rings rings(64, 64);
	auto config = rings.config(12758);
	config.max_sessions = 1;
	CifMessageServer server(config);
	server.start();
	server.enableClients();

	TestClient first;
	ASSERT_TRUE(first.connect(12758));
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	TestClient excess;
	ASSERT_TRUE(excess.connect(12758));
	// The excess client is told why before the socket is closed.
	const std::string refusal = excess.readWithin(kPoll);
	EXPECT_NE(refusal.find("Connection refused"), std::string::npos);
	EXPECT_TRUE(excess.waitForDisconnect(kPoll));

	// The first (accepted) client must remain connected.
	EXPECT_FALSE(first.waitForDisconnect(std::chrono::milliseconds(300)));

	server.stop();
}

TEST(CifMessageServerTest, SlowClientDoesNotBlockBroadcastToOtherClients) {
	constexpr std::size_t MESSAGE_SIZE = 1024 * 1024;
	Rings rings(64, static_cast<int>(MESSAGE_SIZE + 1024));
	CifMessageServer server(rings.config(12759));
	server.start();
	server.enableClients();

	TestClient slowClient;
	TestClient healthyClient;
	ASSERT_TRUE(slowClient.connect(12759));
	ASSERT_TRUE(healthyClient.connect(12759));
	std::this_thread::sleep_for(std::chrono::milliseconds(200));

	pushToDataOut(rings, std::string(MESSAGE_SIZE, 'X') + "\r\n");
	server.pollDataOut();

	const auto received = healthyClient.readWithin(std::chrono::milliseconds(2000));
	EXPECT_FALSE(received.empty());
	EXPECT_EQ(received.front(), 'X');

	server.stop();
}

TEST(CifMessageServerTest, DisconnectsClientWhenOutboundQueueOverflows) {
	Rings rings(64, 8192);
	auto config = rings.config(12760);
	config.max_pending_write_bytes = 256;
	CifMessageServer server(config);
	server.start();
	server.enableClients();

	TestClient slowClient;
	ASSERT_TRUE(slowClient.connect(12760));
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	const std::string payload(128, 'X');
	for (int i = 0; i < 20; ++i) {
		pushToDataOut(rings, payload + "\r\n");
		server.pollDataOut();
	}

	EXPECT_TRUE(slowClient.waitForDisconnect(std::chrono::milliseconds(3000)));

	server.stop();
}

TEST(CifMessageServerTest, WelcomeMessageSentOnConnect) {
	Rings rings(64, 64);
	auto config = rings.config(12761);
	config.welcome_message = "\r\nCIF console ready.\r\n";
	CifMessageServer server(config);
	server.start();
	server.enableClients();

	TestClient client;
	ASSERT_TRUE(client.connect(12761));

	std::string received = client.readWithin(kPoll);
	EXPECT_EQ(received, "\r\nCIF console ready.\r\n");

	server.stop();
}

TEST(CifMessageServerTest, EchoInputSendsIacWillEchoBeforeWelcome) {
	Rings rings(64, 64);
	auto config = rings.config(12762);
	config.echo_input = true;
	config.welcome_message = "ready\r\n";
	CifMessageServer server(config);
	server.start();
	server.enableClients();

	TestClient client;
	ASSERT_TRUE(client.connect(12762));

	std::string received = client.readWithin(kPoll);
	// IAC WILL ECHO (3 bytes) must precede the welcome message.
	ASSERT_GE(received.size(), 3u);
	EXPECT_EQ(static_cast<unsigned char>(received[0]), 0xFF); // IAC
	EXPECT_EQ(static_cast<unsigned char>(received[1]), 0xFB); // WILL
	EXPECT_EQ(static_cast<unsigned char>(received[2]), 0x01); // ECHO option
	EXPECT_EQ(received.substr(3), "ready\r\n");

	server.stop();
}

TEST(CifMessageServerTest, EchoInputCharactersEchoedBack) {
	Rings rings(64, 64);
	auto config = rings.config(12763);
	config.echo_input = true;
	CifMessageServer server(config);
	server.start();
	server.enableClients();

	TestClient client;
	ASSERT_TRUE(client.connect(12763));
	// Drain the IAC WILL ECHO sent on connect.
	client.readWithin(std::chrono::milliseconds(200));

	// Send a line: server should echo the printable chars + CRLF.
	client.send("hi\r\n");
	std::string echo = client.readWithin(kPoll);
	EXPECT_EQ(echo, "hi\r\n");

	// The line must also have been written to CIF_IBER.
	std::string content;
	for (int i = 0; i < 100; ++i) {
		{
			std::lock_guard<std::mutex> lock(rings.cifMutex);
			content = rings.slice(rings.dataIn, rings.dataIn.data(), rings.inWrite);
		}
		if (!content.empty()) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT_EQ(content, "hi\r");

	server.stop();
}

TEST(CifMessageServerTest, EchoInputBackspaceErasesCharacter) {
	Rings rings(64, 64);
	auto config = rings.config(12764);
	config.echo_input = true;
	CifMessageServer server(config);
	server.start();
	server.enableClients();

	TestClient client;
	ASSERT_TRUE(client.connect(12764));
	client.readWithin(std::chrono::milliseconds(200)); // drain IAC WILL ECHO

	// 'a', BS, 'b', CR: the BS erases 'a', so only 'b' reaches CIF_IBER.
	client.send("a\x08" "b\r");
	std::string echo = client.readWithin(kPoll);
	// Echoed: 'a', then erase sequence for BS, then 'b', then CRLF.
	EXPECT_EQ(echo, "a\x08 \x08" "b\r\n");

	std::string content;
	for (int i = 0; i < 100; ++i) {
		{
			std::lock_guard<std::mutex> lock(rings.cifMutex);
			content = rings.slice(rings.dataIn, rings.dataIn.data(), rings.inWrite);
		}
		if (!content.empty()) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT_EQ(content, "b\r");

	server.stop();
}
