#include <gtest/gtest.h>
#include "tcp/TcpClientTransport.hpp"
#include "tcp/TcpServer.hpp"
#include "tcp/TcpServerTransport.hpp"
#include "../../tcp/test_helpers.hpp"

#include <asio/io_context.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

class TcpClientTransportUnitTest : public ::testing::Test {
protected:
	void SetUp() override {
		io_ = std::make_unique<asio::io_context>();
		runner_ = std::make_unique<test_helpers::IoContextRunner>(*io_);
	}

	void TearDown() override {
		runner_.reset();
		io_.reset();
	}

	std::unique_ptr<asio::io_context> io_;
	std::unique_ptr<test_helpers::IoContextRunner> runner_;
};

TEST_F(TcpClientTransportUnitTest, NameReturnsCorrectValue) {
	auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", "12345");
	EXPECT_EQ(client->name(), "tcp-client");
}

TEST_F(TcpClientTransportUnitTest, StartInitiatesConnection) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	
	bool serverAccepted = false;
	test_helpers::AsyncWaiter acceptWaiter;
	server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport>) {
		serverAccepted = true;
		acceptWaiter.notify();
	};
	
	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	bool clientConnected = false;
	test_helpers::AsyncWaiter connectWaiter;
	auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(testPort));
	client->onConnected = [&]() {
		clientConnected = true;
		connectWaiter.notify();
	};

	client->start();

	EXPECT_TRUE(connectWaiter.wait_for(std::chrono::milliseconds(500)));
	EXPECT_TRUE(clientConnected);
	EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(100)));
	EXPECT_TRUE(serverAccepted);

	client->stop();
	server->stop();
}

TEST_F(TcpClientTransportUnitTest, StopClosesConnection) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	bool disconnected = false;
	std::string disconnectReason;
	test_helpers::AsyncWaiter disconnectWaiter;
	
	auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(testPort));
	client->onConnected = [&]() {
		// Stop immediately after connection
		client->stop();
	};
	client->onDisconnected = [&](const std::string& reason) {
		disconnected = true;
		disconnectReason = reason;
		disconnectWaiter.notify();
	};

	client->start();
	
	EXPECT_TRUE(disconnectWaiter.wait_for(std::chrono::milliseconds(500)));
	EXPECT_TRUE(disconnected);
	EXPECT_EQ(disconnectReason, "stop");

	server->stop();
}

TEST_F(TcpClientTransportUnitTest, StopIsIdempotent) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	int disconnectCount = 0;
	test_helpers::AsyncWaiter connectWaiter;
	auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(testPort));
	client->onConnected = [&]() {
		connectWaiter.notify();
	};
	client->onDisconnected = [&](const std::string&) {
		disconnectCount++;
	};

	client->start();
	ASSERT_TRUE(connectWaiter.wait_for(std::chrono::milliseconds(500)));

	client->stop();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	client->stop();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	client->stop();

	EXPECT_EQ(disconnectCount, 1);

	server->stop();
}

TEST_F(TcpClientTransportUnitTest, StopFromWorkerThreadWhileActive) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();

	std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
	test_helpers::AsyncWaiter acceptWaiter;
	server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
		serverTransport = transport;
		transport->start();
		acceptWaiter.notify();
	};

	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	test_helpers::AsyncWaiter connectWaiter;
	test_helpers::AsyncWaiter disconnectWaiter;
	auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(testPort));
	client->onConnected = [&]() {
		connectWaiter.notify();
	};
	client->onDisconnected = [&](const std::string&) {
		disconnectWaiter.notify();
	};

	client->start();
	ASSERT_TRUE(connectWaiter.wait_for(std::chrono::milliseconds(500)));
	ASSERT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(500)));

	std::atomic<bool> keepSending {true};
	std::thread sender([&]() {
		std::vector<std::uint8_t> payload(1024, 0x5A);
		while (keepSending.load()) {
			try {
				client->asyncSend(payload.data(), payload.size());
			} catch (const std::exception&) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	});

	std::thread stopper([&]() {
		std::this_thread::sleep_for(std::chrono::milliseconds(25));
		client->stop();
	});

	stopper.join();
	keepSending.store(false);
	sender.join();

	EXPECT_TRUE(disconnectWaiter.wait_for(std::chrono::milliseconds(500)));

	if (serverTransport) {
		serverTransport->stop();
	}
	server->stop();
}

TEST_F(TcpClientTransportUnitTest, AsyncSendThrowsWhenNotConnected) {
	auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", "12345");
	
	std::vector<std::uint8_t> data{1, 2, 3};
	EXPECT_THROW(client->asyncSend(data.data(), data.size()), std::runtime_error);
}

TEST_F(TcpClientTransportUnitTest, DisconnectsWhenWriteQueueLimitIsExceeded) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	server->onAccept = [](std::shared_ptr<tlcfi::tcp::TcpServerTransport>) {};
	server->start();

	test_helpers::AsyncWaiter errorWaiter;
	std::string errorMessage;
	auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(
		*io_, "127.0.0.1", std::to_string(server->port()), tlcfi::tcp::TlsConfig{}, 8);
	client->onError = [&](const std::string& message) {
		errorMessage = message;
		errorWaiter.notify();
	};
	client->onConnected = [&]() {
		const std::vector<std::uint8_t> firstPayload(5, 0x42);
		const std::vector<std::uint8_t> secondPayload(4, 0x43);
		client->asyncSend(firstPayload.data(), firstPayload.size());
		client->asyncSend(secondPayload.data(), secondPayload.size());
	};

	client->start();

	ASSERT_TRUE(errorWaiter.wait_for(std::chrono::milliseconds(500)));
	EXPECT_EQ(errorMessage, "write queue limit exceeded");

	client->stop();
	server->stop();
}

TEST_F(TcpClientTransportUnitTest, AsyncSendSendsDataWhenConnected) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	
	std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
	test_helpers::AsyncWaiter acceptWaiter;
	server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
		serverTransport = transport;
		transport->start();
		acceptWaiter.notify();
	};
	
	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	std::vector<std::uint8_t> receivedData;
	test_helpers::AsyncWaiter dataWaiter;
	
	auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(testPort));
	client->onConnected = [&]() {
		std::vector<std::uint8_t> data{1, 2, 3, 4, 5};
		client->asyncSend(data.data(), data.size());
	};
	
	if (serverTransport) {
		serverTransport->onBytes = [&](const std::uint8_t* data, std::size_t size) {
			receivedData.insert(receivedData.end(), data, data + size);
			dataWaiter.notify();
		};
	}

	client->start();
	
	EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(500)));
	if (serverTransport) {
		serverTransport->onBytes = [&](const std::uint8_t* data, std::size_t size) {
			receivedData.insert(receivedData.end(), data, data + size);
			dataWaiter.notify();
		};
	}
	
	EXPECT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(500)));
	EXPECT_EQ(receivedData, (std::vector<std::uint8_t>{1, 2, 3, 4, 5}));

	client->stop();
	server->stop();
}

TEST_F(TcpClientTransportUnitTest, ReceivesDataFromServer) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	
	std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
	test_helpers::AsyncWaiter acceptWaiter;
	server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
		serverTransport = transport;
		transport->start();
		acceptWaiter.notify();
	};
	
	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	std::vector<std::uint8_t> receivedData;
	test_helpers::AsyncWaiter dataWaiter;
	
	auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(testPort));
	client->onBytes = [&](const std::uint8_t* data, std::size_t size) {
		receivedData.insert(receivedData.end(), data, data + size);
		dataWaiter.notify();
	};

	client->start();
	
	EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(500)));
	
	if (serverTransport) {
		std::vector<std::uint8_t> sendData{10, 20, 30};
		serverTransport->asyncSend(sendData.data(), sendData.size());
	}
	
	EXPECT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(500)));
	EXPECT_EQ(receivedData, (std::vector<std::uint8_t>{10, 20, 30}));

	client->stop();
	server->stop();
}

TEST_F(TcpClientTransportUnitTest, WriteQueueingWorks) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	
	std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
	test_helpers::AsyncWaiter acceptWaiter;
	server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
		serverTransport = transport;
		transport->start();
		acceptWaiter.notify();
	};
	
	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	std::vector<std::uint8_t> receivedData;
	int receiveCount = 0;
	test_helpers::AsyncWaiter dataWaiter;
	
	auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(testPort));
	client->onConnected = [&]() {
		// Send multiple messages quickly
		std::vector<std::uint8_t> data1{1, 2, 3};
		std::vector<std::uint8_t> data2{4, 5, 6};
		std::vector<std::uint8_t> data3{7, 8, 9};
		client->asyncSend(data1.data(), data1.size());
		client->asyncSend(data2.data(), data2.size());
		client->asyncSend(data3.data(), data3.size());
	};
	
	if (serverTransport) {
		serverTransport->onBytes = [&](const std::uint8_t* data, std::size_t size) {
			receivedData.insert(receivedData.end(), data, data + size);
			receiveCount++;
			if (receiveCount >= 3) {
				dataWaiter.notify();
			}
		};
	}

	client->start();
	
	EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(500)));
	if (serverTransport) {
		serverTransport->onBytes = [&](const std::uint8_t* data, std::size_t size) {
			receivedData.insert(receivedData.end(), data, data + size);
			receiveCount++;
			if (receiveCount >= 3) {
				dataWaiter.notify();
			}
		};
	}
	
	EXPECT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(500)));
	EXPECT_EQ(receivedData.size(), 9u);
	EXPECT_EQ(receivedData, (std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8, 9}));

	client->stop();
	server->stop();
}

TEST_F(TcpClientTransportUnitTest, OnErrorCalledOnConnectionFailure) {
	bool errorCalled = false;
	std::string errorMessage;
	test_helpers::AsyncWaiter errorWaiter;
	
	// Use an invalid hostname that will definitely fail DNS resolution
	auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "invalid-hostname-that-does-not-exist.local", "12345");
	client->onError = [&](const std::string& reason) {
		errorCalled = true;
		errorMessage = reason;
		errorWaiter.notify();
	};

	client->start();
	
	// Wait for error callback (DNS resolution should fail quickly)
	EXPECT_TRUE(errorWaiter.wait_for(std::chrono::milliseconds(3000)));
	EXPECT_TRUE(errorCalled);
	EXPECT_FALSE(errorMessage.empty());
	// Error message should contain "resolve"
	EXPECT_TRUE(errorMessage.find("resolve") != std::string::npos);

	client->stop();
}

TEST_F(TcpClientTransportUnitTest, OnDisconnectedCalledOnServerClose) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	
	std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
	test_helpers::AsyncWaiter acceptWaiter;
	server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
		serverTransport = transport;
		transport->start();
		acceptWaiter.notify();
	};
	
	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	bool disconnected = false;
	test_helpers::AsyncWaiter disconnectWaiter;
	
	auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(testPort));
	client->onConnected = [&]() {
		// Close server transport to simulate disconnect
		if (serverTransport) {
			serverTransport->stop();
		}
	};
	client->onDisconnected = [&](const std::string&) {
		disconnected = true;
		disconnectWaiter.notify();
	};

	client->start();
	
	EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(500)));
	if (serverTransport) {
		serverTransport->stop();
	}
	
	EXPECT_TRUE(disconnectWaiter.wait_for(std::chrono::milliseconds(500)));
	EXPECT_TRUE(disconnected);

	client->stop();
	server->stop();
}

TEST_F(TcpClientTransportUnitTest, MultipleReadsHandledCorrectly) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	
	std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
	test_helpers::AsyncWaiter acceptWaiter;
	server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
		serverTransport = transport;
		transport->start();
		acceptWaiter.notify();
	};
	
	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	std::vector<std::uint8_t> receivedData;
	int readCount = 0;
	test_helpers::AsyncWaiter dataWaiter;
	
	auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(testPort));
	client->onBytes = [&](const std::uint8_t* data, std::size_t size) {
		receivedData.insert(receivedData.end(), data, data + size);
		readCount++;
		if (readCount >= 3) {
			dataWaiter.notify();
		}
	};

	client->start();
	
	EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(500)));
	
	if (serverTransport) {
		// Send multiple chunks
		std::vector<std::uint8_t> chunk1{1, 2};
		std::vector<std::uint8_t> chunk2{3, 4};
		std::vector<std::uint8_t> chunk3{5, 6};
		serverTransport->asyncSend(chunk1.data(), chunk1.size());
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		serverTransport->asyncSend(chunk2.data(), chunk2.size());
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		serverTransport->asyncSend(chunk3.data(), chunk3.size());
	}
	
	EXPECT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(500)));
	EXPECT_EQ(receivedData.size(), 6u);

	client->stop();
	server->stop();
}

TEST_F(TcpClientTransportUnitTest, ConcurrentAsyncSendIsThreadSafe) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();

	constexpr int SEND_COUNT = 32;
	constexpr std::size_t PAYLOAD_SIZE = 4096;
	constexpr std::size_t EXPECTED_SIZE = SEND_COUNT * PAYLOAD_SIZE;
	std::vector<std::uint8_t> receivedData;
	test_helpers::AsyncWaiter dataWaiter;

	std::shared_ptr<tlcfi::tcp::TcpServerTransport> serverTransport;
	test_helpers::AsyncWaiter acceptWaiter;
	server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
		serverTransport = transport;
		transport->onBytes = [&](const std::uint8_t* data, std::size_t size) {
			receivedData.insert(receivedData.end(), data, data + size);
			if (receivedData.size() >= EXPECTED_SIZE) {
				dataWaiter.notify();
			}
		};
		transport->start();
		acceptWaiter.notify();
	};
	
	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	test_helpers::AsyncWaiter connectWaiter;
	auto client = std::make_shared<tlcfi::tcp::TcpClientTransport>(*io_, "127.0.0.1", std::to_string(testPort));
	client->onConnected = [&]() {
		connectWaiter.notify();
	};

	client->start();
	EXPECT_TRUE(connectWaiter.wait_for(std::chrono::milliseconds(500)));
	EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(100)));

	std::vector<std::thread> threads;
	for (int i = 0; i < SEND_COUNT; ++i) {
		threads.emplace_back([&, i]() {
			std::vector<std::uint8_t> data(PAYLOAD_SIZE, static_cast<std::uint8_t>(i));
			client->asyncSend(data.data(), data.size());
		});
	}

	for (auto& t : threads) {
		t.join();
	}

	ASSERT_TRUE(dataWaiter.wait_for(std::chrono::milliseconds(3000)));
	ASSERT_EQ(receivedData.size(), EXPECTED_SIZE);
	for (int i = 0; i < SEND_COUNT; ++i) {
		EXPECT_EQ(std::count(receivedData.begin(), receivedData.end(), static_cast<std::uint8_t>(i)),
		          PAYLOAD_SIZE);
	}

	client->stop();
	server->stop();
}

