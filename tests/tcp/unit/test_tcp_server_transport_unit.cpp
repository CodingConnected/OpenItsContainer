#include <gtest/gtest.h>
#include "tcp/TcpServer.hpp"
#include "tcp/TcpServerTransport.hpp"
#include "../../tcp/test_helpers.hpp"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

class TcpServerTransportUnitTest : public ::testing::Test {
protected:
	void SetUp() override {
		io_ = std::make_unique<asio::io_context>();
		runner_ = std::make_unique<test_helpers::IoContextRunner>(*io_);
	}

	void TearDown() override {
		// The io_context must outlive every io object (asio contract). Join
		// the io thread first, then destroy the servers and client sockets,
		// and only then the io_context itself — otherwise ~TcpServer /
		// ~socket would run against an already-destroyed io_context.
		runner_.reset();
		servers_.clear();
		clientSockets_.clear();
		io_.reset();
	}

	std::shared_ptr<tlcfi::tcp::TcpServerTransport> createTransport(unsigned short = 0, std::shared_ptr<asio::ip::tcp::socket>* outClientSocket = nullptr) {
		// Bind an OS-assigned ephemeral port (0) and read it back, instead of
		// a fixed port that can collide with another test / TIME_WAIT.
		auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
		const unsigned short port = server->port();
		std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport;
		test_helpers::AsyncWaiter acceptWaiter;

		server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> t) {
			transport = t;
			acceptWaiter.notify();
		};

		server->start();
		std::this_thread::sleep_for(std::chrono::milliseconds(50));

		// Connect a client to trigger accept
		auto clientSocket = std::make_shared<asio::ip::tcp::socket>(*io_);
		asio::ip::tcp::resolver resolver(*io_);
		std::error_code ec;
		auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port), ec);
		if (!ec) {
			asio::connect(*clientSocket, endpoints, ec);
			if (!ec) {
				bool accepted = acceptWaiter.wait_for(std::chrono::milliseconds(500));
				if (accepted && transport) {
					servers_.push_back(server);
					if (outClientSocket) {
						*outClientSocket = clientSocket;
					} else {
						clientSockets_.push_back(clientSocket);
					}
					return transport;
				}
			}
		}
		server->stop();
		return nullptr;
	}

	std::unique_ptr<asio::io_context> io_;
	std::unique_ptr<test_helpers::IoContextRunner> runner_;
	std::vector<std::shared_ptr<tlcfi::tcp::TcpServer>> servers_;
	std::vector<std::shared_ptr<asio::ip::tcp::socket>> clientSockets_;
};

TEST_F(TcpServerTransportUnitTest, NameReturnsCorrectValue) {
	auto transport = createTransport(55190);
	ASSERT_NE(transport, nullptr);
	EXPECT_EQ(transport->name(), "tcp-server-conn");
}

TEST_F(TcpServerTransportUnitTest, StartInvokesOnConnected) {
	auto transport = createTransport(55191);
	ASSERT_NE(transport, nullptr);

	bool connected = false;
	test_helpers::AsyncWaiter connectWaiter;
	transport->onConnected = [&]() {
		connected = true;
		connectWaiter.notify();
	};

	transport->start();

	EXPECT_TRUE(connectWaiter.wait_for(std::chrono::milliseconds(200)));
	EXPECT_TRUE(connected);
}

TEST_F(TcpServerTransportUnitTest, StopInvokesOnDisconnected) {
	auto transport = createTransport(55192);
	ASSERT_NE(transport, nullptr);
	transport->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	bool disconnected = false;
	std::string disconnectReason;
	test_helpers::AsyncWaiter disconnectWaiter;
	transport->onDisconnected = [&](const std::string& reason) {
		disconnected = true;
		disconnectReason = reason;
		disconnectWaiter.notify();
	};

	transport->stop();

	EXPECT_TRUE(disconnectWaiter.wait_for(std::chrono::milliseconds(200)));
	EXPECT_TRUE(disconnected);
	EXPECT_EQ(disconnectReason, "stop");
}

TEST_F(TcpServerTransportUnitTest, StopIsIdempotent) {
	auto transport = createTransport(55193);
	ASSERT_NE(transport, nullptr);
	transport->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	int disconnectCount = 0;
	transport->onDisconnected = [&](const std::string&) {
		disconnectCount++;
	};

	transport->stop();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	transport->stop();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	transport->stop();

	EXPECT_EQ(disconnectCount, 1);
}

TEST_F(TcpServerTransportUnitTest, ReceivesDataFromClient) {
	std::shared_ptr<asio::ip::tcp::socket> clientSocket;
	auto transport = createTransport(55194, &clientSocket);
	ASSERT_NE(transport, nullptr);
	ASSERT_NE(clientSocket, nullptr);
	transport->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	std::vector<std::uint8_t> receivedData;
	test_helpers::AsyncWaiter dataWaiter;
	transport->onBytes = [&](const std::uint8_t* data, std::size_t size) {
		receivedData.insert(receivedData.end(), data, data + size);
		dataWaiter.notify();
	};

	// Send data from the client socket that was created with the transport
	std::vector<std::uint8_t> sendData{1, 2, 3, 4, 5};
	std::error_code ec;
	asio::write(*clientSocket, asio::buffer(sendData), ec);
	EXPECT_FALSE(ec);
	
	EXPECT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(500)));
	EXPECT_EQ(receivedData, sendData);
}

TEST_F(TcpServerTransportUnitTest, SendsDataToClient) {
	std::shared_ptr<asio::ip::tcp::socket> clientSocket;
	auto transport = createTransport(55195, &clientSocket);
	ASSERT_NE(transport, nullptr);
	ASSERT_NE(clientSocket, nullptr);
	transport->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// Send data from transport to client
	std::vector<std::uint8_t> sendData{10, 20, 30};
	transport->asyncSend(sendData.data(), sendData.size());

	// Give the async write time to complete
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// Read data from the client socket - use read_some to avoid blocking forever
	std::vector<std::uint8_t> receiveBuf(10);
	std::error_code ec;
	std::size_t received = clientSocket->read_some(asio::buffer(receiveBuf), ec);
	
	EXPECT_FALSE(ec);
	EXPECT_EQ(received, 3u);
	EXPECT_EQ(receiveBuf[0], 10);
	EXPECT_EQ(receiveBuf[1], 20);
	EXPECT_EQ(receiveBuf[2], 30);
}

TEST_F(TcpServerTransportUnitTest, WriteQueueingWorks) {
	std::shared_ptr<asio::ip::tcp::socket> clientSocket;
	auto transport = createTransport(55196, &clientSocket);
	ASSERT_NE(transport, nullptr);
	ASSERT_NE(clientSocket, nullptr);
	transport->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// Send multiple messages quickly
	std::vector<std::uint8_t> data1{1, 2};
	std::vector<std::uint8_t> data2{3, 4};
	std::vector<std::uint8_t> data3{5, 6};
	transport->asyncSend(data1.data(), data1.size());
	transport->asyncSend(data2.data(), data2.size());
	transport->asyncSend(data3.data(), data3.size());

	// Give the async writes time to complete
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// Read all data from the client socket - use read_some to avoid blocking forever
	std::vector<std::uint8_t> receiveBuf(10);
	std::error_code ec;
	std::size_t received = clientSocket->read_some(asio::buffer(receiveBuf), ec);
	
	EXPECT_FALSE(ec);
	EXPECT_GE(received, 6u);  // May read more if writes are batched
	EXPECT_EQ(receiveBuf[0], 1);
	EXPECT_EQ(receiveBuf[5], 6);
}

TEST_F(TcpServerTransportUnitTest, DisconnectsWhenWriteQueueLimitIsExceeded) {
	auto transport = createTransport();
	ASSERT_NE(transport, nullptr);
	transport->start();

	test_helpers::AsyncWaiter errorWaiter;
	std::string errorMessage;
	transport->onError = [&](const std::string& message) {
		errorMessage = message;
		errorWaiter.notify();
	};

	const std::vector<std::uint8_t> oversizedPayload(
		tlcfi::tcp::TcpServerTransport::DEFAULT_MAX_PENDING_WRITE_BYTES + 1, 0x42);
	transport->asyncSend(oversizedPayload.data(), oversizedPayload.size());

	ASSERT_TRUE(errorWaiter.wait_for(std::chrono::milliseconds(500)));
	EXPECT_EQ(errorMessage, "write queue limit exceeded");
}

TEST_F(TcpServerTransportUnitTest, MultipleReadsHandledCorrectly) {
	std::shared_ptr<asio::ip::tcp::socket> clientSocket;
	auto transport = createTransport(55197, &clientSocket);
	ASSERT_NE(transport, nullptr);
	ASSERT_NE(clientSocket, nullptr);
	transport->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	std::vector<std::uint8_t> receivedData;
	int readCount = 0;
	test_helpers::AsyncWaiter dataWaiter;
	transport->onBytes = [&](const std::uint8_t* data, std::size_t size) {
		receivedData.insert(receivedData.end(), data, data + size);
		readCount++;
		if (readCount >= 3) {
			dataWaiter.notify();
		}
	};

	// Send multiple chunks from the client socket
	std::vector<std::uint8_t> chunk1{1, 2};
	std::vector<std::uint8_t> chunk2{3, 4};
	std::vector<std::uint8_t> chunk3{5, 6};
	std::error_code ec;
	asio::write(*clientSocket, asio::buffer(chunk1), ec);
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	asio::write(*clientSocket, asio::buffer(chunk2), ec);
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	asio::write(*clientSocket, asio::buffer(chunk3), ec);
	
	EXPECT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(500)));
	EXPECT_EQ(receivedData.size(), 6u);
}

TEST_F(TcpServerTransportUnitTest, OnDisconnectedCalledOnClientClose) {
	std::shared_ptr<asio::ip::tcp::socket> clientSocket;
	auto transport = createTransport(55198, &clientSocket);
	ASSERT_NE(transport, nullptr);
	ASSERT_NE(clientSocket, nullptr);
	transport->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	bool disconnected = false;
	test_helpers::AsyncWaiter disconnectWaiter;
	transport->onDisconnected = [&](const std::string&) {
		disconnected = true;
		disconnectWaiter.notify();
	};

	// Close the client connection to trigger disconnect
	// Use error_code version to avoid exceptions
	std::error_code ec;
	clientSocket->close(ec);
	clientSocket.reset();  // Release the socket reference

	// Wait for disconnect callback to be invoked
	EXPECT_TRUE(disconnectWaiter.wait_for(std::chrono::milliseconds(1000)));
	EXPECT_TRUE(disconnected);
	
	// Stop the transport to ensure clean shutdown
	transport->stop();
	
	// Give time for async operations to complete before test ends
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(TcpServerTransportUnitTest, ConcurrentAsyncSendIsThreadSafe) {
	std::shared_ptr<asio::ip::tcp::socket> clientSocket;
	auto transport = createTransport(55199, &clientSocket);
	ASSERT_NE(transport, nullptr);
	ASSERT_NE(clientSocket, nullptr);
	transport->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	constexpr int SEND_COUNT = 32;
	constexpr std::size_t PAYLOAD_SIZE = 4096;
	constexpr std::size_t EXPECTED_SIZE = SEND_COUNT * PAYLOAD_SIZE;
	std::vector<std::uint8_t> receivedData(EXPECTED_SIZE);
	test_helpers::AsyncWaiter dataWaiter;
	asio::async_read(*clientSocket, asio::buffer(receivedData),
		[&](const std::error_code& error, std::size_t size) {
			if (!error && size == EXPECTED_SIZE) {
				dataWaiter.notify();
			}
		});

	std::vector<std::thread> threads;
	for (int i = 0; i < SEND_COUNT; ++i) {
		threads.emplace_back([&, i]() {
			std::vector<std::uint8_t> data(PAYLOAD_SIZE, static_cast<std::uint8_t>(i));
			transport->asyncSend(data.data(), data.size());
		});
	}

	for (auto& t : threads) {
		t.join();
	}

	ASSERT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(3000)));
	for (int i = 0; i < SEND_COUNT; ++i) {
		EXPECT_EQ(std::count(receivedData.begin(), receivedData.end(), static_cast<std::uint8_t>(i)),
		          PAYLOAD_SIZE);
	}
}

TEST_F(TcpServerTransportUnitTest, ContinuesReadingAfterPartialRead) {
	std::shared_ptr<asio::ip::tcp::socket> clientSocket;
	auto transport = createTransport(55200, &clientSocket);
	ASSERT_NE(transport, nullptr);
	ASSERT_NE(clientSocket, nullptr);
	transport->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	std::vector<std::uint8_t> receivedData;
	test_helpers::AsyncWaiter dataWaiter;
	transport->onBytes = [&](const std::uint8_t* data, std::size_t size) {
		receivedData.insert(receivedData.end(), data, data + size);
		if (receivedData.size() >= 10) {
			dataWaiter.notify();
		}
	};

	// Send large chunk that may be read in parts
	std::vector<std::uint8_t> largeData(100, 0x42);
	std::error_code ec;
	asio::write(*clientSocket, asio::buffer(largeData), ec);
	
	EXPECT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(500)));
	EXPECT_GE(receivedData.size(), 10u);
}

