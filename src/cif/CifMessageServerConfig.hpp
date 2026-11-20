#pragma once

#include <cstddef>
#include <mutex>
#include <string>

namespace cif {

    struct CifMessageServerConfig {
        static constexpr std::size_t DEFAULT_MAX_PENDING_WRITE_BYTES = 32 * 1024 * 1024;

        unsigned short port;
        /// Address to listen on. Loopback is the secure default because inbound
        /// messages are injected into the controller's CIF_IBER command channel.
        std::string bind_address = "127.0.0.1";
        std::size_t max_sessions = 16;
        std::size_t max_inbound_line_size = 64 * 1024;
        /// Maximum total outbound bytes queued per client (including the write
        /// currently in flight). A client that stops reading is disconnected.
        std::size_t max_pending_write_bytes = DEFAULT_MAX_PENDING_WRITE_BYTES;
        int data_in_size;
        int data_out_size;
        char* data_in;
        char* data_out;
        char** data_in_read_ptr;
        char** data_in_write_ptr;
        char** data_out_read_ptr;
        char** data_out_write_ptr;
        /// Mutex serialising access to the shared CIF arrays/ring pointers with
        /// the control loop. If null, no cross-thread locking is performed.
        std::mutex* cif_mutex = nullptr;
        /// Message sent to each client immediately on connection (empty = disabled).
        std::string welcome_message = {};
        /// If true, the server echoes each received character back to the sending
        /// client and sends IAC WILL ECHO on connect so the telnet client disables
        /// its own local echo. Also handles backspace (BS/DEL) with erase redraw.
        bool echo_input = false;
        /// If true, connections are only accepted while the control loop is active
        /// (between enableClients() and disableClients() calls). New connections
        /// while inactive are refused, and existing sessions are closed on
        /// disableClients(). This is the legacy opt-in behaviour.
        ///
        /// If false (default), clients may connect at any time. Inbound data
        /// received while inactive is silently discarded, or answered with
        /// inactiveMessage if non-empty. Existing sessions survive disableClients().
        bool cifClientsOnlyDuringControl = false;
        /// Sent back to a client when it submits a line but the control loop is
        /// not active. Only used when cifClientsOnlyDuringControl is false.
        /// Empty (default) = silently discard the input.
        std::string inactiveMessage = {};
    };

} // namespace cif
