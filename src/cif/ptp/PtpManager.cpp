#include "cif/ptp/PtpManager.hpp"

#include "runtime/RuntimeApp.hpp" // tlcfi::runtime::activeOptions()

#include <iostream>

namespace cif::ptp {

PtpManager& PtpManager::instance() {
    // Thread-safe lazy init (C++11 magic statics). Constructed on the first C
    // driver call from the hosted controller.
    static PtpManager inst;
    return inst;
}

PtpManager::PtpManager()
    : workGuard_(asio::make_work_guard(io_))
    , links_(tlcfi::runtime::activeOptions().ptpLinks, io_,
             PtpLinkSet::DEFAULT_RECONNECT_INTERVAL) {
    // Bring links up (posts connect/listen work) then run the shared io thread.
    links_.bringUpAll();
    ioThread_ = std::thread([this] { io_.run(); });

    // One-line summary. Logging goes to std::cout for now — the manager is
    // created deep inside the control loop with no injection point for the app
    // LogCallback (a proper log sink is a later-phase item).
    std::cout << "[ptp] transport initialised: " << links_.linkCount() << " link(s)";
    for (const auto& c : links_.linkConfigs()) {
        std::cout << " {linkId=" << c.linkId << ", mode=" << toString(c.mode)
                  << ", peer=" << c.peerIp << ":" << c.tcpPort << "}";
    }
    std::cout << std::endl;
}

PtpManager::~PtpManager() {
    // Post teardown for every link (cancel timers, close sockets), then let the
    // io thread drain and exit once the work guard is released.
    links_.bringDownAll();
    workGuard_.reset();
    if (ioThread_.joinable()) {
        ioThread_.join();
    }
}

} // namespace cif::ptp
