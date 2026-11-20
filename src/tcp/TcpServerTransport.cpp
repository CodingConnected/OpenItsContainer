#include "TcpServerTransport.hpp"

#include <asio/post.hpp>
#include <asio/ssl.hpp>
#include <asio/write.hpp>

#include <stdexcept>
#include <utility>

namespace tlcfi::tcp {

TcpServerTransport::TcpServerTransport(asio::ip::tcp::socket socket, std::size_t maxPendingWriteBytes)
	: socket_(std::move(socket))
	, maxPendingWriteBytes_(maxPendingWriteBytes) {
	if (maxPendingWriteBytes_ == 0) {
		throw std::invalid_argument("maxPendingWriteBytes must be greater than zero");
	}
}

TcpServerTransport::TcpServerTransport(asio::ip::tcp::socket socket, asio::ssl::context& sslContext,
                                       std::size_t maxPendingWriteBytes)
	: socket_(std::move(socket))
	, sslStream_(std::make_unique<asio::ssl::stream<asio::ip::tcp::socket&>>(socket_, sslContext))
	, maxPendingWriteBytes_(maxPendingWriteBytes) {
	if (maxPendingWriteBytes_ == 0) {
		throw std::invalid_argument("maxPendingWriteBytes must be greater than zero");
	}
}

void TcpServerTransport::start() {
	stopped_ = false;
	if (sslStream_) {
		// Perform TLS handshake first
		doHandshake();
	} else {
		// Plain TCP - start reading immediately
		if (onConnected) onConnected();
		doRead();
	}
}

void TcpServerTransport::stop() {
	if (stopped_.exchange(true)) return;  // Already stopped, make idempotent

	// Clear the write queue while holding the lock.
	{
		std::lock_guard<std::mutex> lock(writeMutex_);
		writeQueue_.clear();
		pendingWriteBytes_ = 0;
		writeInProgress_ = false;
	}

	// asio sockets are not thread-safe: cancelling/closing the socket on the
	// caller's thread races an in-flight async_read_some / async_write on the
	// io thread. Post the cancel+close onto the socket's executor so all
	// socket access stays on the io thread (mirrors asyncSend()). Capturing
	// `self` keeps the transport alive until the close runs.
	//
	// Note: we don't explicitly shut down the SSL stream to avoid potential
	// hangs; closing the socket makes all operations fail immediately and the
	// SSL stream is destroyed with the object.
	auto self = shared_from_this();
	asio::post(socket_.get_executor(), [self]() {
		std::error_code ec;
		self->socket_.cancel(ec);
		self->socket_.close(ec);
	});

	notifyClosed("stop");
}

void TcpServerTransport::notifyClosed(const std::string& reason) {
	// onDisconnected keeps its original semantics: it may fire on a read/write
	// failure and again on a subsequent stop() (consumers expect stop() to
	// always report "stop"). The onClosed hook is server-internal bookkeeping
	// and must fire exactly once, on whichever path closes the connection first.
	if (onDisconnected) onDisconnected(reason);
	if (!closedNotified_.exchange(true)) {
		// Copy the hook under the mutex, then invoke the copy outside the lock:
		// this races safely with setOnClosed()/clearOnClosed() and does not
		// hold the lock during the callback.
		std::function<void()> hook;
		{
			std::lock_guard<std::mutex> lock(onClosedMutex_);
			hook = onClosed_;
		}
		if (hook) hook();
	}
}

void TcpServerTransport::setOnClosed(std::function<void()> callback) {
	std::lock_guard<std::mutex> lock(onClosedMutex_);
	onClosed_ = std::move(callback);
}

void TcpServerTransport::clearOnClosed() {
	std::lock_guard<std::mutex> lock(onClosedMutex_);
	onClosed_ = nullptr;
}

void TcpServerTransport::asyncSend(const std::uint8_t* data, std::size_t size) {
	bool startWrite = false;
	bool queueOverflow = false;
	{
		std::lock_guard<std::mutex> lock(writeMutex_);
		if (stopped_) return;
		if (writeQueueOverflowed_) return;
		if (size > maxPendingWriteBytes_ - pendingWriteBytes_) {
			writeQueueOverflowed_ = true;
			queueOverflow = true;
		} else {
			writeQueue_.emplace_back(data, data + size);
			pendingWriteBytes_ += size;
		}
		if (!queueOverflow && !writeInProgress_) {
			writeInProgress_ = true;
			startWrite = true;
		}
	}
	if (queueOverflow) {
		auto self = shared_from_this();
		asio::post(socket_.get_executor(), [self]() {
			if (self->onError) {
				self->onError("write queue limit exceeded");
			}
			self->stop();
		});
		return;
	}
	if (startWrite) {
		// Drive the write on the socket's executor (the io thread) rather than
		// the caller's thread.
		auto self = shared_from_this();
		asio::post(socket_.get_executor(), [self]() { self->doWrite(); });
	}
}

void TcpServerTransport::doRead() {
	auto self = shared_from_this();
	
	if (sslStream_) {
		// TLS read
		sslStream_->async_read_some(asio::buffer(readBuf_), [this, self](std::error_code ec, std::size_t n){
			if (ec) {
				// Check for SSL shutdown errors - these are normal when peer closes gracefully
				// Note: asio::error::eof means the peer closed the connection
				if (ec == asio::ssl::error::stream_truncated) {
					// SSL stream was truncated (peer didn't send proper shutdown)
					if (!stopped_) notifyClosed(ec.message());
				} else if (ec == asio::error::eof) {
					// Peer closed connection - this is normal
					if (!stopped_) notifyClosed(ec.message());
				} else if (ec.category() == asio::error::get_ssl_category()) {
					// SSL-specific error
					if (!stopped_) notifyClosed(ec.message());
				} else if (ec != asio::error::operation_aborted) {
					// Other errors
					if (!stopped_) notifyClosed(ec.message());
				}
				return;
			}
			if (onBytes) onBytes(readBuf_.data(), n);
			if (!stopped_) doRead();
		});
	} else {
		// Plain TCP read
		socket_.async_read_some(asio::buffer(readBuf_), [this, self](std::error_code ec, std::size_t n){
			if (ec) {
				if (!stopped_) notifyClosed(ec.message());
				return;
			}
			if (onBytes) onBytes(readBuf_.data(), n);
			if (!stopped_) doRead();
		});
	}
}

void TcpServerTransport::doWrite() {
	if (stopped_) return;  // Don't start new writes if stopping

	std::shared_ptr<std::vector<std::uint8_t>> data = std::make_shared<std::vector<std::uint8_t>>();
	{
		std::lock_guard<std::mutex> lock(writeMutex_);
		if (stopped_ || writeQueue_.empty()) {
			writeInProgress_ = false;
			return;
		}
		*data = std::move(writeQueue_.front());
		writeQueue_.pop_front();  // Pop immediately after move
	}
	auto self = shared_from_this();

	if (stopped_) return;  // Check again before async operation

	if (sslStream_) {
		// TLS write
		asio::async_write(*sslStream_, asio::buffer(*data), [this, self, data](std::error_code ec, std::size_t){
			if (stopped_) return;  // Don't continue if stopped
			onWriteComplete(ec, data->size());
		});
	} else {
		// Plain TCP write
		asio::async_write(socket_, asio::buffer(*data), [this, self, data](std::error_code ec, std::size_t){
			if (stopped_) return;  // Don't continue if stopped
			onWriteComplete(ec, data->size());
		});
	}
}

void TcpServerTransport::onWriteComplete(const std::error_code& error, std::size_t completedBytes) {
	if (stopped_) return;

	if (error) {
		{
			std::lock_guard<std::mutex> lock(writeMutex_);
			writeQueue_.clear();
			pendingWriteBytes_ = 0;
			writeInProgress_ = false;
		}
		if (onError) onError(error.message());
		return;
	}

	bool hasMore = false;
	{
		std::lock_guard<std::mutex> lock(writeMutex_);
		if (stopped_) {
			writeInProgress_ = false;
			return;
		}
		pendingWriteBytes_ -= completedBytes;
		hasMore = !writeQueue_.empty();
		if (!hasMore) {
			writeInProgress_ = false;
		}
	}
	if (hasMore) {
		doWrite();
	}
}

void TcpServerTransport::doHandshake() {
	auto self = shared_from_this();
	sslStream_->async_handshake(asio::ssl::stream_base::server,
		[this, self](std::error_code ec) {
			if (ec) {
				if (!stopped_) {
					std::string errorMsg = "TLS handshake failed: " + ec.message() + " (code: " + std::to_string(ec.value()) + ")";
					if (onError) onError(errorMsg);
					notifyClosed(errorMsg);
				}
				return;
			}
			
			// Handshake successful - connection is ready
			if (!stopped_) {
				if (onConnected) onConnected();
				doRead();
			}
		});
}

std::string TcpServerTransport::name() const {
	return sslStream_ ? "tls-server-conn" : "tcp-server-conn";
}

} // namespace tlcfi::tcp


