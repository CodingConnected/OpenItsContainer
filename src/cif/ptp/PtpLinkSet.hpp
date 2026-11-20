#pragma once

// --- Project Includes ---
#include "cif/ptp/PtpConfig.hpp"
#include "cif/ptp/PtpLink.hpp"
#include "cif/ptp/PtpListener.hpp"

// --- Third-Party Includes ---
#include <asio/io_context.hpp>

// --- Standard Library Includes ---
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <vector>

// --- Namespace Declaration ---
namespace cif::ptp {

/**
 * @brief The set of configured PTP links, addressed by linkId.
 *
 * This is the transport's testable core: it owns up to @ref MAX_LINKS links
 * (keyed by their engineer-chosen @c linkId) and exposes the byte-level
 * get/put/poll operations the hosted controller reaches through the C driver
 * contract (lees_char_COM_IBER / bezet_COM_IBER / schrijf_char_COM_UBER, plus
 * init_COM_IBER / init_COM_UBER). It carries no global state and no singleton,
 * so tests can construct one from any config.
 *
 * A @c linkId that is not configured is handled gracefully: reads report empty
 * (EOB / not busy) and writes are dropped, so a controller that polls an
 * unconfigured link simply sees a permanently-down link. The first driver call
 * for an unknown linkId also emits a one-time diagnostic (it usually means the
 * controller's PTP->PORTNR and tlcfi.json's linkId disagree).
 */
class PtpLinkSet {
public:
    /// @brief End-of-buffer sentinel returned by @ref readChar when empty (spec: EOB).
    static constexpr int EOB = -1;

    /// @brief Default reconnect delay for connect-mode links (spec §1.4: 60 s).
    static constexpr std::chrono::milliseconds DEFAULT_RECONNECT_INTERVAL{60000};

    /**
     * @brief Build the link set from validated configs.
     * @param links             Per-link configuration (re-validated defensively).
     * @param io                Shared io_context all links' socket/timer ops run on.
     * @param reconnectInterval Delay between reconnect attempts (default 60 s;
     *                          tests pass a short value).
     * @param ringBytes         Usable capacity of each ring; raised to MIN_RING_BYTES.
     * @throws std::runtime_error if @p links fails @ref validatePtpLinks.
     */
    PtpLinkSet(const std::vector<PtpLinkConfig>& links,
               asio::io_context& io,
               std::chrono::milliseconds reconnectInterval = DEFAULT_RECONNECT_INTERVAL,
               std::size_t ringBytes = MIN_RING_BYTES);

    /// @brief Number of configured links.
    std::size_t linkCount() const noexcept { return links_.size(); }

    /// @brief The link for @p linkId, or nullptr if unconfigured.
    PtpLink* link(int linkId) noexcept;

    /// @brief Copies of every configured link's config (for one-off diagnostics).
    std::vector<PtpLinkConfig> linkConfigs() const;

    /// @brief Bring every configured link's transport up.
    void bringUpAll();
    /// @brief Bring every configured link's transport down.
    void bringDownAll();

    // --- Byte-level driver operations (keyed by linkId) ---

    /// @brief Reset the incoming ring for @p linkId (init_COM_IBER). No-op if unconfigured.
    void initIber(int linkId) noexcept;
    /// @brief Reset the outgoing ring for @p linkId (init_COM_UBER). No-op if unconfigured.
    void initUber(int linkId) noexcept;
    /// @brief Pop one incoming byte for @p linkId; returns 0..255, or @ref EOB if empty/unconfigured.
    int  readChar(int linkId) noexcept;
    /// @brief 1 if an incoming byte is waiting for @p linkId, else 0.
    int  busy(int linkId) noexcept;
    /// @brief Push one outgoing byte for @p linkId; returns false if full/unconfigured (dropped).
    bool writeChar(int linkId, char c) noexcept;

private:
    /// Log once (to std::cerr) the first time the controller uses an unknown linkId.
    void warnUnknownLink(int linkId) noexcept;

    /// Configured links, keyed by linkId. At most MAX_LINKS entries.
    std::map<int, std::unique_ptr<PtpLink>> links_;
    /// linkIds already warned about, so the diagnostic fires only once each.
    std::set<int> warnedUnknownIds_;
    /// One acceptor per distinct listen port; each demuxes to its links by peer IP.
    std::vector<std::unique_ptr<PtpListener>> listeners_;
};

} // namespace cif::ptp
