#include <gtest/gtest.h>
#include "tcp/TcpServer.hpp"
#include "tcp/TcpServerTransport.hpp"
#include "../../tcp/test_helpers.hpp"

#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <chrono>
#include <memory>
#include <set>
#include <thread>
#include <vector>

class TcpServerUnitTest : public ::testing::Test {
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

TEST_F(TcpServerUnitTest, StartBeginsAccepting) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	
	bool acceptCalled = false;
	test_helpers::AsyncWaiter acceptWaiter;
	server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport>) {
		acceptCalled = true;
		acceptWaiter.notify();
	};

	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// Connect a client
	asio::ip::tcp::socket clientSocket(*io_);
	asio::ip::tcp::resolver resolver(*io_);
	std::error_code ec;
	auto endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
	if (!ec) {
		asio::connect(clientSocket, endpoints, ec);
		if (!ec) {
			EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(500)));
			EXPECT_TRUE(acceptCalled);
		}
	}

	server->stop();
}

TEST_F(TcpServerUnitTest, StopPreventsNewAccepts) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	
	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	server->stop();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// Try to connect - should fail
	asio::ip::tcp::socket clientSocket(*io_);
	asio::ip::tcp::resolver resolver(*io_);
	std::error_code ec;
	auto endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
	if (!ec) {
		asio::connect(clientSocket, endpoints, ec);
		// Connection should fail after stop
		EXPECT_TRUE(ec);
	}
}

TEST_F(TcpServerUnitTest, AcceptsMultipleConnections) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	
	std::vector<std::shared_ptr<tlcfi::tcp::TcpServerTransport>> acceptedTransports;
	test_helpers::AsyncWaiter acceptWaiter;
	int acceptCount = 0;
	
	server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
		acceptedTransports.push_back(transport);
		acceptCount++;
		acceptWaiter.notify();
	};

	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// Connect first client
	asio::ip::tcp::socket client1(*io_);
	asio::ip::tcp::resolver resolver(*io_);
	std::error_code ec;
	auto endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
	if (!ec) {
		asio::connect(client1, endpoints, ec);
		if (!ec) {
			EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(500)));
			EXPECT_EQ(acceptCount, 1);
		}
	}

	acceptWaiter.reset();

	// Connect second client
	asio::ip::tcp::socket client2(*io_);
	endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
	if (!ec) {
		asio::connect(client2, endpoints, ec);
		if (!ec) {
			EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(500)));
			EXPECT_EQ(acceptCount, 2);
			EXPECT_EQ(acceptedTransports.size(), 2u);
			EXPECT_NE(acceptedTransports[0], acceptedTransports[1]);
		}
	}

	server->stop();
}

TEST_F(TcpServerUnitTest, StopClosesAllActiveTransports) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	
	std::vector<std::shared_ptr<tlcfi::tcp::TcpServerTransport>> acceptedTransports;
	test_helpers::AsyncWaiter acceptWaiter;
	
	server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
		acceptedTransports.push_back(transport);
		transport->start();
		acceptWaiter.notify();
	};

	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// Accept two connections
	asio::ip::tcp::socket client1(*io_);
	asio::ip::tcp::resolver resolver(*io_);
	std::error_code ec;
	auto endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
	if (!ec) {
		asio::connect(client1, endpoints, ec);
		if (!ec) {
			acceptWaiter.wait_for(std::chrono::milliseconds(500));
		}
	}

	acceptWaiter.reset();
	asio::ip::tcp::socket client2(*io_);
	endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
	if (!ec) {
		asio::connect(client2, endpoints, ec);
		if (!ec) {
			acceptWaiter.wait_for(std::chrono::milliseconds(500));
		}
	}

	EXPECT_EQ(acceptedTransports.size(), 2u);

	// Set up disconnect handlers
	int disconnectCount = 0;
	for (auto& transport : acceptedTransports) {
		transport->onDisconnected = [&](const std::string&) {
			disconnectCount++;
		};
	}

	// Stop server - should close all transports
	server->stop();
	std::this_thread::sleep_for(std::chrono::milliseconds(100));

	EXPECT_EQ(disconnectCount, 2);
}

TEST_F(TcpServerUnitTest, ContinuesAcceptingAfterConnection) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	
	int acceptCount = 0;
	test_helpers::AsyncWaiter acceptWaiter;
	
	server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport>) {
		acceptCount++;
		acceptWaiter.notify();
	};

	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// Accept first connection
	asio::ip::tcp::socket client1(*io_);
	asio::ip::tcp::resolver resolver(*io_);
	std::error_code ec;
	auto endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
	if (!ec) {
		asio::connect(client1, endpoints, ec);
		if (!ec) {
			EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(500)));
			EXPECT_EQ(acceptCount, 1);
		}
	}

	// Keep first connection open and accept second
	acceptWaiter.reset();
	asio::ip::tcp::socket client2(*io_);
	endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
	if (!ec) {
		asio::connect(client2, endpoints, ec);
		if (!ec) {
			EXPECT_TRUE(acceptWaiter.wait_for(std::chrono::milliseconds(500)));
			EXPECT_EQ(acceptCount, 2);
		}
	}

	server->stop();
}

TEST_F(TcpServerUnitTest, HandlesStopDuringAccept) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	
	int acceptCount = 0;
	server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport>) {
		acceptCount++;
	};

	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// Stop while accepting
	server->stop();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// Try to connect - should fail
	asio::ip::tcp::socket clientSocket(*io_);
	asio::ip::tcp::resolver resolver(*io_);
	std::error_code ec;
	auto endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
	if (!ec) {
		asio::connect(clientSocket, endpoints, ec);
		EXPECT_TRUE(ec);
	}

	// No new accepts should have occurred
	EXPECT_EQ(acceptCount, 0);
}

TEST_F(TcpServerUnitTest, EachConnectionGetsUniqueTransport) {
	auto server = std::make_shared<tlcfi::tcp::TcpServer>(*io_, 0);
	const unsigned short testPort = server->port();
	
	std::set<std::shared_ptr<tlcfi::tcp::TcpServerTransport>> transportSet;
	test_helpers::AsyncWaiter acceptWaiter;
	
	server->onAccept = [&](std::shared_ptr<tlcfi::tcp::TcpServerTransport> transport) {
		transportSet.insert(transport);
		acceptWaiter.notify();
	};

	server->start();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// Accept multiple connections
	std::vector<asio::ip::tcp::socket> clients;
	asio::ip::tcp::resolver resolver(*io_);
	std::error_code ec;
	auto endpoints = resolver.resolve("127.0.0.1", std::to_string(testPort), ec);
	
	for (int i = 0; i < 5; ++i) {
		clients.emplace_back(*io_);
		if (!ec) {
			asio::connect(clients.back(), endpoints, ec);
			if (!ec) {
				acceptWaiter.wait_for(std::chrono::milliseconds(500));
				acceptWaiter.reset();
			}
		}
	}

	EXPECT_EQ(transportSet.size(), 5u);

	server->stop();
}

