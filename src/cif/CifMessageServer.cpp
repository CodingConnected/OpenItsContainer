#include "cif/CifMessageServer.hpp"

#include <asio/post.hpp>
#include <asio/write.hpp>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace cif {

CifMessageServer::CifMessageServer(const CifMessageServerConfig& config)
    : config_(config)
{
    onLog = [](::tlcfi::fi::LogLevel level, const std::string& message) {
        switch (level) {
        case ::tlcfi::fi::LogLevel::Trace:   std::cout << "[TRACE] " << message << "\n"; break;
        case ::tlcfi::fi::LogLevel::Debug:   std::cout << "[DEBUG] " << message << "\n"; break;
        case ::tlcfi::fi::LogLevel::Info:    std::cout << "[INFO] "  << message << "\n"; break;
        case ::tlcfi::fi::LogLevel::Warning: std::cout << "[WARNING] " << message << "\n"; break;
        case ::tlcfi::fi::LogLevel::Error:   std::cerr << "[ERROR] " << message << "\n"; break;
        case ::tlcfi::fi::LogLevel::Fatal:   std::cerr << "[FATAL] " << message << "\n"; break;
        }
    };
}

void CifMessageServer::log(::tlcfi::fi::LogLevel level, const std::string& message) {
    if (onLog) onLog(level, message);
}

CifMessageServer::~CifMessageServer() {
    stop();
}

void CifMessageServer::start() {
    if (config_.max_sessions == 0 || config_.max_inbound_line_size == 0 ||
        config_.max_pending_write_bytes == 0) {
        throw std::invalid_argument(
            "CifMessageServer limits must allow at least one session, one inbound byte, "
            "and one pending outbound byte");
    }

    std::error_code addressError;
    const auto bindAddress = asio::ip::make_address(config_.bind_address, addressError);
    if (addressError) {
        throw std::invalid_argument(
            "Invalid CifMessageServer bind address '" + config_.bind_address + "': " +
            addressError.message());
    }

    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;

    acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(
        ioContext_,
        asio::ip::tcp::endpoint(bindAddress, config_.port)
    );
    acceptor_->set_option(asio::ip::tcp::acceptor::reuse_address(true));

    log(::tlcfi::fi::LogLevel::Info, "CifMessageServer: Listening on "
        + config_.bind_address + ":" + std::to_string(config_.port));

    doAccept();

    workerThread_ = std::thread([this] {
        try {
            ioContext_.run();
        } catch (const std::exception& e) {
            log(::tlcfi::fi::LogLevel::Error, std::string("CifMessageServer: io_context error: ") + e.what());
        }
    });
}

void CifMessageServer::stop() {
    running_.store(false);
    clientsEnabled_.store(false);

    // Close acceptor to stop accepting new connections
    if (acceptor_) {
        std::error_code ec;
        acceptor_->close(ec);
        if (ec) {
            log(::tlcfi::fi::LogLevel::Error, "CifMessageServer: Error closing acceptor: " + ec.message());
        }
    }

    closeAllSessions();

    ioContext_.stop();
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
}

void CifMessageServer::enableClients() {
    // Idempotent: only act/log on the enabled->disabled transition, so repeated
    // calls (multiple lifecycle paths, multiple server instances) don't spam.
    if (clientsEnabled_.exchange(true)) {
        return; // already enabled
    }
    log(::tlcfi::fi::LogLevel::Info, "CifMessageServer: Accepting client connections");
}

void CifMessageServer::disableClients() {
    // Idempotent: only act/log on a real disable transition (see enableClients).
    if (!clientsEnabled_.exchange(false)) {
        return; // already disabled
    }
    if (config_.cifClientsOnlyDuringControl) {
        log(::tlcfi::fi::LogLevel::Info,
            "CifMessageServer: Refusing client connections, disconnecting existing clients");
        closeAllSessions();
    } else {
        log(::tlcfi::fi::LogLevel::Info,
            "CifMessageServer: Control loop inactive; inbound data will be rejected");
    }
}

void CifMessageServer::doAccept() {
    if (!running_.load()) return;

    acceptor_->async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
        if (ec) {
            if (running_.load()) {
                log(::tlcfi::fi::LogLevel::Error, "CifMessageServer: Accept error: " + ec.message());
            }
            if (running_.load() &&
                ec != asio::error::operation_aborted &&
                acceptor_ != nullptr &&
                acceptor_->is_open()) {
                doAccept();
            }
            return;
        }

        // When cifClientsOnlyDuringControl is set, refuse connections while the
        // control loop is inactive. With the default (false), all connections
        // are accepted regardless; inbound data is handled in doRead.
        if (!clientsEnabled_.load() && config_.cifClientsOnlyDuringControl) {
            std::error_code writeEc;
            std::string refusal = "Controller not active, connection refused.\r\n";
            asio::write(socket, asio::buffer(refusal), writeEc);
            std::error_code closeEc;
            socket.close(closeEc);
            if (closeEc) {
                log(::tlcfi::fi::LogLevel::Error, "CifMessageServer: Error closing connection (controller not active): " + closeEc.message());
            }
            log(::tlcfi::fi::LogLevel::Warning, "CifMessageServer: Refused connection (controller not active)");
            doAccept();
            return;
        }

        auto endpoint = socket.remote_endpoint(ec);
        if (!ec) {
            log(::tlcfi::fi::LogLevel::Info, "CifMessageServer: Client connected from "
                + endpoint.address().to_string() + ":" + std::to_string(endpoint.port()));
        }

        auto session = std::make_shared<Session>(
            std::move(socket), config_.max_inbound_line_size);
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            if (sessions_.size() < config_.max_sessions) {
                sessions_.push_back(session);
                accepted = true;
            }
        }
        if (!accepted) {
            log(::tlcfi::fi::LogLevel::Warning, "CifMessageServer: Session limit reached, refusing client");
            std::error_code writeEc;
            const std::string refusal =
                "Connection refused: maximum number of clients already connected.\r\n";
            asio::write(session->socket, asio::buffer(refusal), writeEc);
            std::error_code closeEc;
            session->socket.close(closeEc);
            doAccept();
            return;
        }

        {
            // Build the on-connect packet: IAC WILL ECHO (when echo is on) followed
            // by the optional welcome message. Sent as one write so both arrive in a
            // single TCP segment, which avoids a race in the client's read path.
            std::string connectMsg;
            if (config_.echo_input) {
                connectMsg.append("\xFF\xFB\x01", 3); // IAC WILL ECHO
            }
            if (!config_.welcome_message.empty()) {
                connectMsg += config_.welcome_message;
            }
            if (!connectMsg.empty()) {
                enqueueWrite(session, std::make_shared<std::string>(std::move(connectMsg)));
            }
        }
        doRead(session);
        doAccept();
    });
}

void CifMessageServer::doRead(std::shared_ptr<Session> session) {
    if (!running_.load()) return;

    session->socket.async_read_some(asio::buffer(session->readBuf),
        [this, session](std::error_code ec, std::size_t n) {
            if (ec) {
                if (ec != asio::error::operation_aborted) {
                    log(::tlcfi::fi::LogLevel::Info, "CifMessageServer: Client disconnected");
                }
                removeSession(session);
                return;
            }

            std::string toEcho;

            for (std::size_t i = 0; i < n; ++i) {
                const unsigned char c = static_cast<unsigned char>(session->readBuf[i]);

                // Telnet IAC state machine: skip negotiation bytes transparently.
                switch (session->iacState) {
                    case Session::IacState::Iac:
                        // WILL/WONT/DO/DONT (0xFB–0xFE): one more option byte follows.
                        session->iacState = (c >= 0xFB && c <= 0xFE)
                            ? Session::IacState::IacOption
                            : Session::IacState::None;
                        continue;
                    case Session::IacState::IacOption:
                        session->iacState = Session::IacState::None;
                        continue;
                    case Session::IacState::None:
                        break;
                }

                if (c == 0xFF) { // IAC
                    session->iacState = Session::IacState::Iac;
                    session->lastWasCR = false;
                    continue;
                }

                if (c == '\r') {
                    session->lastWasCR = true;
                    if (config_.echo_input) toEcho += "\r\n";
                    if (!session->lineBuffer.empty()) {
                        if (clientsEnabled_.load()) {
                            writeToDataIn(session->lineBuffer);
                        } else if (!config_.inactiveMessage.empty()) {
                            enqueueWrite(session, std::make_shared<std::string>(config_.inactiveMessage));
                        }
                        session->lineBuffer.clear();
                    }
                } else if (c == '\n' || c == '\0') {
                    // LF or NULL: skip if it immediately follows CR (CRLF / CR-NUL),
                    // otherwise treat bare LF as a line terminator (no echo).
                    if (!session->lastWasCR && c == '\n' && !session->lineBuffer.empty()) {
                        if (clientsEnabled_.load()) {
                            writeToDataIn(session->lineBuffer);
                        } else if (!config_.inactiveMessage.empty()) {
                            enqueueWrite(session, std::make_shared<std::string>(config_.inactiveMessage));
                        }
                        session->lineBuffer.clear();
                    }
                    session->lastWasCR = false;
                } else if (c == 0x08 || c == 0x7F) { // BS / DEL
                    session->lastWasCR = false;
                    if (!session->lineBuffer.empty()) {
                        session->lineBuffer.pop_back();
                        if (config_.echo_input) toEcho += "\x08 \x08";
                    }
                } else if (c >= 0x20) { // printable
                    session->lastWasCR = false;
                    if (session->lineBuffer.size() >= session->maxLineSize) {
                        log(::tlcfi::fi::LogLevel::Warning,
                            "CifMessageServer: Inbound line exceeds configured limit");
                        removeSession(session);
                        return;
                    }
                    session->lineBuffer += static_cast<char>(c);
                    if (config_.echo_input) toEcho += static_cast<char>(c);
                } else {
                    session->lastWasCR = false;
                }
            }

            if (!toEcho.empty()) {
                enqueueWrite(session, std::make_shared<std::string>(std::move(toEcho)));
            }

            doRead(session);
        });
}

void CifMessageServer::writeToDataIn(const std::string& message) {
    // Write message bytes + \r terminator into CIF_IBER[] ring buffer
    // at CIF_IBER_SCHRIJF, advance pointer with wrap.
    //
    // Runs on the message-server's io thread; CIF_IBER[] and its read/write
    // pointers are consumed by the legacy controller on the control-loop
    // thread, so serialise the whole operation via the shared CIF mutex.
    std::unique_lock<std::mutex> cifLock;
    if (config_.cif_mutex) {
        cifLock = std::unique_lock<std::mutex>(*config_.cif_mutex);
    }

    char* bufStart = config_.data_in;
    char* bufEnd = config_.data_in + config_.data_in_size;
    char* writePtr = *config_.data_in_write_ptr;

    // Guard against overrunning unread data in the CIF_IBER ring. The message
    // plus its '\r' terminator must fit in the free space; one slot is kept
    // free so read==write unambiguously means empty.
    if (config_.data_in_read_ptr != nullptr && config_.data_in_size >= 2) {
        const char* readPtr = *config_.data_in_read_ptr;
        const std::size_t capacity = static_cast<std::size_t>(config_.data_in_size);
        const std::size_t used = (writePtr >= readPtr)
            ? static_cast<std::size_t>(writePtr - readPtr)
            : capacity - static_cast<std::size_t>(readPtr - writePtr);
        const std::size_t freeSpace = capacity - 1 - used;
        const std::size_t needed = message.size() + 1; // + '\r'
        if (needed > freeSpace) {
            log(::tlcfi::fi::LogLevel::Warning, "CifMessageServer: CIF_IBER ring full, dropping inbound message ("
                + std::to_string(needed) + " bytes needed, " + std::to_string(freeSpace) + " free)");
            return;
        }
    }

    for (char c : message) {
        *writePtr = c;
        ++writePtr;
        if (writePtr >= bufEnd) {
            writePtr = bufStart;
        }
    }

    // Write \r terminator
    *writePtr = '\r';
    ++writePtr;
    if (writePtr >= bufEnd) {
        writePtr = bufStart;
    }

    *config_.data_in_write_ptr = writePtr;
}

void CifMessageServer::pollDataOut() {
    // Called on the control-loop thread. CIF_UBER[] is filled only by the
    // legacy controller (also the control-loop thread), so draining it here
    // needs no lock. The empty case is handled naturally by the loop below.
    char* bufStart = config_.data_out;
    char* bufEnd = config_.data_out + config_.data_out_size;
    char* readPtr = *config_.data_out_read_ptr;

    std::vector<std::string> messages;

    while (readPtr != *config_.data_out_write_ptr) {
        char c = *readPtr;
        ++readPtr;
        if (readPtr >= bufEnd) {
            readPtr = bufStart;
        }

        dataOutPartial_ += c;

        // Check for \r\n message boundary
        if (dataOutPartial_.size() >= 2 &&
            dataOutPartial_[dataOutPartial_.size() - 2] == '\r' &&
            dataOutPartial_[dataOutPartial_.size() - 1] == '\n') {
            // Complete message found — strip \r\n and send with \r\n for telnet
            std::string msg = dataOutPartial_.substr(0, dataOutPartial_.size() - 2);
            dataOutPartial_.clear();

            if (!msg.empty()) {
                messages.push_back(msg + "\r\n");
            }
        }
    }

    *config_.data_out_read_ptr = readPtr;

    // Hand the actual socket writes to the server's own io_context thread, so
    // a slow or stuck TCP client can never block the real-time control loop.
    for (auto& m : messages) {
        asio::post(ioContext_, [this, msg = std::move(m)]() { broadcastToClients(msg); });
    }
}

void CifMessageServer::broadcastToClients(const std::string& message) {
    auto data = std::make_shared<std::string>(message);
    std::vector<std::shared_ptr<Session>> sessions;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex_);
        sessions = sessions_;
    }

    for (const auto& session : sessions) {
        enqueueWrite(session, data);
    }
}

void CifMessageServer::enqueueWrite(const std::shared_ptr<Session>& session,
                                    std::shared_ptr<std::string> message) {
    if (session->writeQueueOverflowed) {
        return;
    }

    const std::size_t messageSize = message->size();
    if (messageSize > config_.max_pending_write_bytes - session->pendingWriteBytes) {
        disconnectSession(session, "Outbound write queue limit exceeded");
        return;
    }

    session->pendingWriteBytes += messageSize;
    session->writeQueue.push_back(std::move(message));
    if (session->writeInProgress) return;

    session->writeInProgress = true;
    doWrite(session);
}

void CifMessageServer::disconnectSession(const std::shared_ptr<Session>& session,
                                           const char* reason) {
    if (session->writeQueueOverflowed) {
        return;
    }
    session->writeQueueOverflowed = true;
    log(::tlcfi::fi::LogLevel::Warning, std::string("CifMessageServer: ") + reason + ", disconnecting client");
    std::error_code closeEc;
    session->socket.close(closeEc);
    removeSession(session);
}

void CifMessageServer::doWrite(const std::shared_ptr<Session>& session) {
    if (session->writeQueue.empty()) {
        session->writeInProgress = false;
        return;
    }

    const auto data = session->writeQueue.front();
    asio::async_write(session->socket, asio::buffer(*data),
        [this, session, data](std::error_code ec, std::size_t) {
            if (session->writeQueueOverflowed) {
                session->writeInProgress = false;
                return;
            }
            if (ec) {
                if (ec != asio::error::operation_aborted) {
                    log(::tlcfi::fi::LogLevel::Warning, "CifMessageServer: Write error, removing client");
                }
                session->writeQueue.clear();
                session->pendingWriteBytes = 0;
                session->writeInProgress = false;
                std::error_code closeEc;
                session->socket.close(closeEc);
                removeSession(session);
                return;
            }

            if (session->writeQueue.empty()) {
                session->writeInProgress = false;
                return;
            }

            session->pendingWriteBytes -= data->size();
            session->writeQueue.pop_front();
            doWrite(session);
        });
}

void CifMessageServer::removeSession(const std::shared_ptr<Session>& session) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    sessions_.erase(
        std::remove(sessions_.begin(), sessions_.end(), session),
        sessions_.end()
    );
}

void CifMessageServer::closeAllSessions() {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (auto& session : sessions_) {
        std::error_code ec;
        session->socket.close(ec);
    }
    sessions_.clear();
}

} // namespace cif
