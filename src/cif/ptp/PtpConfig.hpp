#pragma once

// --- Standard Library Includes ---
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// --- Namespace Declaration ---
namespace cif::ptp {

// --- Constants ---

/// Default TCP port for a PTP link (ASTRIN PTP spec §1.4: "De verbinding loopt
/// over poort 7100").
inline constexpr std::uint16_t DEFAULT_TCP_PORT = 7100;

/// Maximum number of PTP links on one VRI (spec: "maximaal 4 seriële
/// koppelingen op een VRI"). Enforced by @ref validatePtpLinks as a COUNT cap;
/// it does not constrain the value of any link's @c linkId.
inline constexpr int MAX_LINKS = 4;

/// Minimum size (bytes) of each per-link ring buffer (spec §3: "De ingaande en
/// uitgaande berichtenbuffer dienen voldoende groot (2048 bytes) te zijn").
inline constexpr std::size_t MIN_RING_BYTES = 2048;

/**
 * @brief What our container does to establish a PTP link's transport.
 *
 * The value names OUR container's action so there is no "whose side" ambiguity
 * (the ASTRIN spec's "master/slave" is overloaded — the TCP-master merely dials,
 * while koppelsignalen flow both ways regardless of who dialed).
 *   - Connect: we call connect() and dial the peer (spec: master).
 *   - Listen:  we call listen()/accept() and wait for the peer (spec: slave).
 */
enum class Mode : std::uint8_t {
    Connect, ///< We dial the peer (TCP client). Spec term: master.
    Listen,  ///< We wait for the peer (TCP server). Spec term: slave.
};

/**
 * @brief Transport carrying a PTP link.
 *
 * Only @ref Tcp is implemented; the enum exists so a future serial transport is
 * an additive change (no config-schema break). The transport is not exposed in
 * tlcfi.json yet — every link defaults to @ref Tcp.
 */
enum class TransportKind : std::uint8_t {
    Tcp,     ///< PTP over a TCP/IP socket (the only supported transport).
    // Serial, ///< (future) PTP over a real RS232 serial port.
};

/**
 * @brief Configuration of a single PTP link (POD; no asio dependency).
 *
 * Held by value inside the container's runtime options and pulled by the PTP
 * transport at first use. @c linkId is the LOCAL key the hosted controller uses
 * to address this link: it is the @c PTP->PORTNR value the controller passes to
 * lees_char_COM_IBER / schrijf_char_COM_UBER. Any integer is accepted (it is an
 * engineer-chosen id, not a slot index); it is not transmitted and the two
 * peers' ids need not match. Only its uniqueness within this VRI matters.
 */
struct PtpLinkConfig {
    int           linkId = 0;                 ///< Local link key = the controller's PTP->PORTNR.
    Mode          mode = Mode::Connect;       ///< What our container does (dial vs. wait).
    std::string   peerIp;                     ///< Connect: dial target. Listen: expected peer (demux key).
    std::uint16_t tcpPort = DEFAULT_TCP_PORT; ///< TCP port (default 7100).
    TransportKind transport = TransportKind::Tcp; ///< Internal; not parsed from config yet.
};

// --- Enum helpers ---

/// @brief Parse a "mode" string ("connect"|"listen"). Throws on anything else.
inline Mode parseMode(const std::string& s) {
    if (s == "connect") return Mode::Connect;
    if (s == "listen")  return Mode::Listen;
    throw std::runtime_error("PTP link 'mode' must be \"connect\" or \"listen\", got \"" + s + "\"");
}

/// @brief Human-readable mode name (for logs / errors).
inline const char* toString(Mode m) {
    switch (m) {
        case Mode::Connect: return "connect";
        case Mode::Listen:  return "listen";
    }
    return "?";
}

/**
 * @brief Structural validation of a set of PTP links (no IP-format check).
 *
 * Enforced (throws std::runtime_error on the first violation):
 *  - at most @ref MAX_LINKS links;
 *  - @c linkId unique across links (its value is otherwise unconstrained);
 *  - @c peerIp non-empty for BOTH modes (connect: dial target; listen: the
 *    per-link demux key used to route an accepted connection to its link).
 *
 * IP-address *format* is validated by the caller (RuntimeConfig, which already
 * has an asio-based checker) so this header stays asio-free and header-only.
 *
 * @param links The links to validate.
 * @throws std::runtime_error on any violation.
 */
inline void validatePtpLinks(const std::vector<PtpLinkConfig>& links) {
    if (links.size() > static_cast<std::size_t>(MAX_LINKS)) {
        throw std::runtime_error(
            "ptpLinks: at most " + std::to_string(MAX_LINKS) +
            " links are allowed (got " + std::to_string(links.size()) + ")");
    }
    std::set<int> seenLinkId;
    std::set<std::string> seenListenDemux; // "tcpPort|peerIp" for listen links
    for (const auto& link : links) {
        if (!seenLinkId.insert(link.linkId).second) {
            throw std::runtime_error(
                "ptpLinks: duplicate linkId " + std::to_string(link.linkId));
        }
        if (link.peerIp.empty()) {
            throw std::runtime_error(
                "ptpLinks: link linkId " + std::to_string(link.linkId) + " (mode " +
                toString(link.mode) + ") requires a non-empty 'peerIp'" +
                (link.mode == Mode::Listen ? " (the demux key for the shared listen port)" : ""));
        }
        // Listen links sharing a port are demultiplexed by peer IP, so two of
        // them on the same port with the same peerIp would be ambiguous.
        if (link.mode == Mode::Listen) {
            const std::string key = std::to_string(link.tcpPort) + "|" + link.peerIp;
            if (!seenListenDemux.insert(key).second) {
                throw std::runtime_error(
                    "ptpLinks: two listen links share peerIp " + link.peerIp +
                    " on port " + std::to_string(link.tcpPort) +
                    " — the accepted connection cannot be demultiplexed to a linkId");
            }
        }
    }
}

} // namespace cif::ptp
