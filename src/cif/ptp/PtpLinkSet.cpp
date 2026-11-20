#include "cif/ptp/PtpLinkSet.hpp"

#include <asio/ip/address.hpp>

#include <algorithm>
#include <iostream>
#include <map>
#include <system_error>

namespace cif::ptp {

PtpLinkSet::PtpLinkSet(const std::vector<PtpLinkConfig>& links,
                       asio::io_context& io,
                       std::chrono::milliseconds reconnectInterval,
                       std::size_t ringBytes) {
    validatePtpLinks(links); // defensive; RuntimeConfig also validates at load time
    const std::size_t bytes = std::max(ringBytes, MIN_RING_BYTES);
    for (const auto& cfg : links) {
        // linkId uniqueness is guaranteed by validatePtpLinks; any value is allowed.
        links_[cfg.linkId] = std::make_unique<PtpLink>(cfg, bytes, io, reconnectInterval);
    }

    // Build one listener per distinct listen port, each demuxing to its links by
    // peer IP. peerIp format was validated at config load; skip any that somehow
    // fail to parse rather than throwing on this (control-thread) path.
    std::map<std::uint16_t, std::vector<PtpListener::Peer>> byPort;
    for (auto& [id, l] : links_) {
        if (l->config().mode != Mode::Listen) continue;
        std::error_code ec;
        const auto addr = asio::ip::make_address(l->config().peerIp, ec);
        if (ec) continue;
        byPort[l->config().tcpPort].emplace_back(addr, l.get());
    }
    for (auto& [port, peers] : byPort) {
        listeners_.push_back(std::make_unique<PtpListener>(io, port, std::move(peers)));
    }
}

PtpLink* PtpLinkSet::link(int linkId) noexcept {
    auto it = links_.find(linkId);
    return it == links_.end() ? nullptr : it->second.get();
}

std::vector<PtpLinkConfig> PtpLinkSet::linkConfigs() const {
    std::vector<PtpLinkConfig> out;
    out.reserve(links_.size());
    for (const auto& [id, l] : links_) out.push_back(l->config());
    return out;
}

void PtpLinkSet::warnUnknownLink(int linkId) noexcept {
    // All driver calls arrive on the single control thread, so warnedUnknownIds_
    // has one producer and needs no lock. Guard the (throwing) set/stream ops
    // since this is invoked from noexcept driver methods.
    try {
        if (warnedUnknownIds_.insert(linkId).second) {
            std::cerr << "[ptp] controller used link id " << linkId
                      << " but no such link is configured — check tlcfi.json "
                         "'ptpLinks[].linkId' matches the controller's PTP->PORTNR"
                      << std::endl;
        }
    } catch (...) {
        // Diagnostics must never take down the control loop.
    }
}

void PtpLinkSet::bringUpAll() {
    for (auto& [id, l] : links_) {
        l->bringUp();
    }
    for (auto& listener : listeners_) {
        listener->start();
    }
}

void PtpLinkSet::bringDownAll() {
    for (auto& listener : listeners_) {
        listener->stop();
    }
    for (auto& [id, l] : links_) {
        l->bringDown();
    }
}

void PtpLinkSet::initIber(int linkId) noexcept {
    if (PtpLink* l = link(linkId)) l->iber().reset();
    else warnUnknownLink(linkId);
}

void PtpLinkSet::initUber(int linkId) noexcept {
    if (PtpLink* l = link(linkId)) l->uber().reset();
    else warnUnknownLink(linkId);
}

int PtpLinkSet::readChar(int linkId) noexcept {
    PtpLink* l = link(linkId);
    if (!l) { warnUnknownLink(linkId); return EOB; }
    char c;
    if (!l->iber().pop(c)) return EOB;
    return static_cast<unsigned char>(c); // 0..255, never collides with EOB(-1)
}

int PtpLinkSet::busy(int linkId) noexcept {
    PtpLink* l = link(linkId);
    if (!l) { warnUnknownLink(linkId); return 0; }
    return l->iber().empty() ? 0 : 1;
}

bool PtpLinkSet::writeChar(int linkId, char c) noexcept {
    PtpLink* l = link(linkId);
    if (!l) { warnUnknownLink(linkId); return false; }
    return l->uber().push(c);
}

} // namespace cif::ptp
