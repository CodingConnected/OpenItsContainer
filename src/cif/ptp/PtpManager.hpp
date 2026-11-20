#pragma once

// --- Project Includes ---
#include "cif/ptp/PtpLinkSet.hpp"

// --- Third-Party Includes ---
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

// --- Standard Library Includes ---
#include <thread>

// --- Namespace Declaration ---
namespace cif::ptp {

/**
 * @brief Process-wide owner of the PTP transport, created lazily on first use.
 *
 * The hosted controller's PTP module reaches the transport only through the C
 * driver symbols (see PtpTransport.cpp), and the FIRST such call constructs this
 * singleton. Construction pulls the link configuration from the always-linked
 * accessor @c tlcfi::runtime::activeOptions() (a PTP -> runtime reference, the
 * safe direction that never forces the PTP translation unit into a controller
 * that does not use it), brings every configured link up, and starts a single
 * io thread that all links share.
 *
 * The singleton is a thin adapter over @ref PtpLinkSet, which holds the real
 * logic and is what the unit tests exercise directly.
 */
class PtpManager {
public:
    /// @brief The lazily-constructed process-wide instance.
    static PtpManager& instance();

    ~PtpManager();

    PtpManager(const PtpManager&) = delete;
    PtpManager& operator=(const PtpManager&) = delete;

    // --- C driver contract, delegated to the link set ---
    // The linkId here is the controller's PTP->PORTNR (see PtpTransport.cpp).
    void initIber(int linkId) noexcept { links_.initIber(linkId); }
    void initUber(int linkId) noexcept { links_.initUber(linkId); }
    int  readChar(int linkId) noexcept { return links_.readChar(linkId); }
    int  busy(int linkId) noexcept { return links_.busy(linkId); }
    void writeChar(int linkId, char c) noexcept { links_.writeChar(linkId, c); }

    /// @brief Access the underlying link set (diagnostics / tests).
    PtpLinkSet& links() noexcept { return links_; }

private:
    PtpManager();

    // Member order matters: io_ must outlive the work guard, the links (whose
    // sockets/timers bind to it) and the thread; the thread is stopped first in
    // the destructor.
    asio::io_context io_;
    asio::executor_work_guard<asio::io_context::executor_type> workGuard_;
    PtpLinkSet links_;
    std::thread ioThread_;
};

} // namespace cif::ptp
