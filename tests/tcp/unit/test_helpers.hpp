#pragma once

#include "mock_asio.hpp"
#include "../../tcp/test_helpers.hpp"

#include <asio/io_context.hpp>
#include <memory>

namespace test_helpers::tcp::unit {

/**
 * @brief Helper to create a mock socket connected to another mock socket.
 * 
 * Creates a pair of mock sockets that can communicate with each other
 * through in-memory buffers. Data written to one socket appears in the
 * read buffer of the other.
 */
class MockSocketPair {
public:
	MockSocketPair(asio::io_context& io)
		: socket1_(io), socket2_(io) {
		socket1_.simulateConnect();
		socket2_.simulateConnect();
	}

	MockSocket& socket1() { return socket1_; }
	MockSocket& socket2() { return socket2_; }

	/**
	 * @brief Transfers data written to socket1 to socket2's read buffer.
	 */
	void transferSocket1ToSocket2() {
		auto written = socket1_.getWrittenData();
		if (!written.empty()) {
			socket2_.injectReadData(written);
			socket1_.clearWrittenData();
		}
	}

	/**
	 * @brief Transfers data written to socket2 to socket1's read buffer.
	 */
	void transferSocket2ToSocket1() {
		auto written = socket2_.getWrittenData();
		if (!written.empty()) {
			socket1_.injectReadData(written);
			socket2_.clearWrittenData();
		}
	}

	/**
	 * @brief Transfers data bidirectionally.
	 */
	void transferBoth() {
		transferSocket1ToSocket2();
		transferSocket2ToSocket1();
	}

private:
	MockSocket socket1_;
	MockSocket socket2_;
};

/**
 * @brief Helper class for unit tests that need an io_context and runner.
 */
class UnitTestFixture {
public:
	UnitTestFixture() 
		: io_(std::make_unique<asio::io_context>()),
		  runner_(std::make_unique<test_helpers::IoContextRunner>(*io_)) {}

	~UnitTestFixture() {
		runner_.reset();
		io_.reset();
	}

	asio::io_context& io() { return *io_; }

private:
	std::unique_ptr<asio::io_context> io_;
	std::unique_ptr<test_helpers::IoContextRunner> runner_;
};

} // namespace test_helpers::tcp::unit

