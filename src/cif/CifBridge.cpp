#include "cif/CifBridge.hpp"
#include "fi/FiControllerApp.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace cif {

CifBridge::CifBridge(tlcfi::fi::FiControllerApp& app)
    : app_(app) {
    onLog = [](::tlcfi::fi::LogLevel level, const std::string& message) {
        using L = ::tlcfi::fi::LogLevel;
        switch (level) {
        case L::Trace:   std::cout << "[TRACE] " << message << "\n"; break;
        case L::Debug:   std::cout << "[DEBUG] " << message << "\n"; break;
        case L::Info:    std::cout << "[INFO] "  << message << "\n"; break;
        case L::Warning: std::cout << "[WARNING] " << message << "\n"; break;
        case L::Error:   std::cerr << "[ERROR] " << message << "\n"; break;
        case L::Fatal:   std::cerr << "[FATAL] " << message << "\n"; break;
        }
    };
}

void CifBridge::log(::tlcfi::fi::LogLevel level, const std::string& message) {
    if (onLog) onLog(level, message);
}

void CifBridge::start() {
    app_.onSignalGroupUpdate = [this](const ::tlcfi::tlcfi::SignalGroup& sg){ 
        handleSignalGroupUpdate(sg); 
    };
    app_.onOutputUpdate = [this](const ::tlcfi::tlcfi::Output& o){ handleOutputUpdate(o); };
    app_.onDetectorUpdate = [this](const ::tlcfi::tlcfi::Detector& d){ handleDetectorUpdate(d); };
    app_.onInputUpdate = [this](const ::tlcfi::tlcfi::Input& i){ handleInputUpdate(i); };
    app_.onSpecialVehicleEvent = [this](const ::tlcfi::tlcfi::SpecialVehicleEvent& evt){ handleSpecialVehicleEvent(evt); };
}

void CifBridge::configureFileLogging(bool enabled, std::string outputDir, std::string systemName,
                                      int maxFiles) {
    fileLoggingEnabled_ = enabled;
    fileLogOutputDir_ = std::filesystem::path(outputDir.empty() ? "." : outputDir);
    fileLogSystemName_ = sanitizeSystemName(systemName.empty() ? "SYSTEM" : systemName);
    fileLogMaxFiles_ = maxFiles;
    fileSwitchRequested_ = false;
    nextRotationRequestAt_ = std::chrono::system_clock::time_point{};
}

namespace {
// Real capacities of the shared CIF output/input arrays, used as the bounds
// check on every translated index before we write. These MUST equal the actual
// allocation sizes: CIF_IS_SWICO[]/CIF_GUS[]/CIF_WUS[] sit contiguously in the
// CVN-C layout (see CIF_INC.h), so a write past a bound silently corrupts the
// neighbouring array (e.g. an over-range CIF_IS_SWICO[] write lands in CIF_GUS[]).
//
// The sizes come straight from the authoritative CVN-C constants. These are
// `extern const s_int16` here and are resolved at link time against the
// controller's CIF_PUBLIC definitions (which size the arrays from the same
// constants). Because the arrays live inside `extern "C" {}`, the `const`
// definitions get external linkage ([dcl.link]/7), so referencing them here
// links cleanly — the earlier hardcoded 128 was neither necessary nor safe.
// They are runtime `const` (not constexpr) because they are link-time values;
// every use below is a runtime comparison, so that is fine.
const int kCifWusCapacity = CIF_PB_AANT_US_FC + CIF_PB_AANT_US_OV;
const int kCifIsCapacity  = CIF_PB_AANT_IS_D + CIF_PB_AANT_IS_OV;
} // namespace

void CifBridge::handleSignalGroupUpdate(const ::tlcfi::tlcfi::SignalGroup& sg) {
    int idx = translateSignalGroupIdToIndex(sg.id);
    if (idx < 0 || idx >= kCifWusCapacity) return;
    int value = 0;
    switch (sg.state) {
        case ::tlcfi::tlcfi::SignalGroupState::ProtectedMovementAllowed: value = CIF_GROEN; break;
        case ::tlcfi::tlcfi::SignalGroupState::ProtectedClearance: value = CIF_GEEL; break;
        case ::tlcfi::tlcfi::SignalGroupState::StopAndRemain: value = CIF_ROOD; break;
        default: value = CIF_ROOD; break;
    }
    // Runs on the TLC-FI protocol thread; serialise against the control loop.
    std::lock_guard<std::mutex> lock(cifMutex_);
    CIF_WUS[idx] = static_cast<s_int16>(value);
    CIF_WUSWIJZ = 1;

    // Clear pending request now that TLC has acknowledged
    pendingSgRequests_.erase(sg.id);
}

void CifBridge::handleOutputUpdate(const ::tlcfi::tlcfi::Output& o) {
    int idx = translateOutputIdToIndex(o.id);
    if (idx < 0 || idx >= kCifWusCapacity) return;
    int value = 0;
    if (o.state.has_value()) value = *o.state;
    std::lock_guard<std::mutex> lock(cifMutex_);
    CIF_WUS[idx] = static_cast<s_int16>(value);
    CIF_WUSWIJZ = 1;

    // Clear pending request now that TLC has acknowledged
    pendingOutputsRequests_.erase(o.id);
}

void CifBridge::handleDetectorUpdate(const ::tlcfi::tlcfi::Detector& d) {
    int idx = translateDetectorIdToIndex(d.id);
    if (idx < 0 || idx >= kCifIsCapacity) {
        // Rare path — indicates the detector isn't in our mapping, which is
        // almost always a config bug worth flagging immediately.
        log(::tlcfi::fi::LogLevel::Warning,
            "CifBridge: DROPPED detector update id='" + d.id
            + "' state=" + std::to_string(static_cast<int>(d.state))
            + " (translateDetectorIdToIndex=" + std::to_string(idx)
            + ", kCifIsCapacity=" + std::to_string(kCifIsCapacity) + ")");
        return;
    }
    int value = (d.state == ::tlcfi::tlcfi::DetectorState::Occupied) ? CIF_DET_BEZET : 0;
    std::lock_guard<std::mutex> lock(cifMutex_);
    CIF_IS[idx] = static_cast<s_int16>(value);
    CIF_IS_SWICO[idx] = static_cast<s_int16>(d.swico);
    CIF_ISWIJZ = 1;
}

void CifBridge::handleInputUpdate(const ::tlcfi::tlcfi::Input& i) {
    int idx = translateInputIdToIndex(i.id);
    if (idx < 0 || idx >= kCifIsCapacity) return;
    std::lock_guard<std::mutex> lock(cifMutex_);
    CIF_IS[idx] = static_cast<s_int16>(i.state);
    CIF_IS_SWICO[idx] = static_cast<s_int16>(i.swico);
    CIF_ISWIJZ = 1;
}

void CifBridge::processDesiredOutputChanges() {
    if (CIF_GUSWIJZ == 0) return;

    std::vector<std::pair<std::string, ::tlcfi::tlcfi::SignalGroupState>> sgUpdates;
    std::vector<std::pair<std::string, int>> outUpdates;

    // CIF_GUS[] is written only by the legacy controller on this (control-loop)
    // thread, so reading it needs no lock. app_.getSignalGroupState() locks
    // FiConsumerApp's state mutex; it is called BEFORE cifMutex_ is taken to
    // avoid a lock-order inversion with the TLC-FI handlers (stateMutex_ ->
    // cifMutex_). cifMutex_ guards only the pending-request maps here.
    for (size_t idx = 0; idx < indexToSignalGroupId_.size(); ++idx) {
        if (indexToSignalGroupId_[idx].empty()) continue;
        const auto& sgId = indexToSignalGroupId_[idx];
        int val = CIF_GUS[static_cast<int>(idx)];
        ::tlcfi::tlcfi::SignalGroupState desiredState = ::tlcfi::tlcfi::SignalGroupState::StopAndRemain;
        if (val == CIF_GROEN) desiredState = ::tlcfi::tlcfi::SignalGroupState::ProtectedMovementAllowed;
        else if (val == CIF_GEEL) desiredState = ::tlcfi::tlcfi::SignalGroupState::ProtectedClearance;

        auto currentState = app_.getSignalGroupState(sgId);

        std::lock_guard<std::mutex> lock(cifMutex_);
        // Skip if we already have a pending request for this exact state
        auto pendingIt = pendingSgRequests_.find(sgId);
        if (pendingIt != pendingSgRequests_.end() && pendingIt->second == desiredState) {
            continue;
        }
        // Only send update if desired state differs from TLC's current state
        if (currentState != desiredState) {
            log(::tlcfi::fi::LogLevel::Trace,
                "CifBridge: SG " + sgId
                + " CIF_GUS=" + std::to_string(val)
                + " current=" + std::to_string(static_cast<int>(currentState))
                + " desired=" + std::to_string(static_cast<int>(desiredState)));
            sgUpdates.emplace_back(sgId, desiredState);
            pendingSgRequests_[sgId] = desiredState;
        }
    }
    for (size_t idx = 0; idx < indexToOutputId_.size(); ++idx) {
        if (indexToOutputId_[idx].empty()) continue;
        int cifIndex = signalGroupCount_ + static_cast<int>(idx);
        int val = CIF_GUS[cifIndex];
        const auto& outId = indexToOutputId_[idx];

        // Fetch TLC's current output state BEFORE taking cifMutex_ — getOutput()
        // locks FiConsumerApp's stateMutex_, and the TLC-FI handlers take
        // stateMutex_ -> cifMutex_, so cifMutex_ must not be held across this call.
        auto current = app_.getOutput(outId);

        std::lock_guard<std::mutex> lock(cifMutex_);
        // Skip if we already have a pending request for this exact state
        auto pendingIt = pendingOutputsRequests_.find(outId);
        if (pendingIt != pendingOutputsRequests_.end() && pendingIt->second == val) {
            continue;
        }
        // Skip if TLC already reports this output at the desired state
        if (current.state.has_value() && *current.state == val) {
            continue;
        }

        outUpdates.emplace_back(outId, val);
        pendingOutputsRequests_[outId] = val;
    }

    CIF_GUSWIJZ = 0;

    if (!sgUpdates.empty()) app_.requestSignalGroupStates(sgUpdates);
    if (!outUpdates.empty()) app_.requestOutputStates(outUpdates);
}

// Placeholder translations
int CifBridge::translateSignalGroupIdToIndex(const std::string& id) const {
    auto it = signalGroupIdToIndex_.find(id);
    return it == signalGroupIdToIndex_.end() ? -1 : it->second;
}
int CifBridge::translateOutputIdToIndex(const std::string& id) const {
    auto it = outputIdToIndex_.find(id);
    return it == outputIdToIndex_.end() ? -1 : it->second + signalGroupCount_;
}
int CifBridge::translateDetectorIdToIndex(const std::string& id) const {
    auto it = detectorIdToIndex_.find(id);
    return it == detectorIdToIndex_.end() ? -1 : it->second;
}
int CifBridge::translateInputIdToIndex(const std::string& id) const {
    auto it = inputIdToIndex_.find(id);
    return it == inputIdToIndex_.end() ? -1 : it->second + detectorCount_;
}

std::string CifBridge::translateIndexToSignalGroupId(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= indexToSignalGroupId_.size()) return {};
    return indexToSignalGroupId_[static_cast<size_t>(index)];
}
std::string CifBridge::translateIndexToOutputId(int index) const {
    if (index < 0 || static_cast<size_t>(index) >= indexToOutputId_.size()) return {};
    return indexToOutputId_[static_cast<size_t>(index)];
}

// Public interface to populate ID-to-index mappings
void CifBridge::setSignalGroupMapping(std::string signalGroupId, int index) {
    if (ignoredItems_.signalGroups.count(signalGroupId) != 0) {
        log(::tlcfi::fi::LogLevel::Info,
            "CifBridge: ignoring controller-only signal group '" + signalGroupId + "' (declared absent in TLC)");
        matchedIgnoredSignalGroups_.insert(signalGroupId);
        return;
    }
    insertSignalGroupMapping(std::move(signalGroupId), index);
}

void CifBridge::insertSignalGroupMapping(std::string signalGroupId, int index) {
    if (signalGroupIdToIndex_.find(signalGroupId) != signalGroupIdToIndex_.end()) {
        log(::tlcfi::fi::LogLevel::Warning, "CifBridge: SignalGroup mapping already exists for " + signalGroupId);
        return;
    }

    log(::tlcfi::fi::LogLevel::Debug,
        "CifBridge: Setting signal group mapping for " + signalGroupId + " to index " + std::to_string(index));
    std::lock_guard<std::mutex> lock(mapMutex_);
    if (indexToSignalGroupId_.size() <= static_cast<size_t>(index)) {
        indexToSignalGroupId_.resize(index + 1);
    }
    indexToSignalGroupId_[index] = signalGroupId;
    signalGroupIdToIndex_[std::move(signalGroupId)] = index;
}

void CifBridge::setOutputMapping(std::string outputId, int index) {
    if (ignoredItems_.outputs.count(outputId) != 0) {
        log(::tlcfi::fi::LogLevel::Info,
            "CifBridge: ignoring controller-only output '" + outputId + "' (declared absent in TLC)");
        matchedIgnoredOutputs_.insert(outputId);
        return;
    }
    insertOutputMapping(std::move(outputId), index);
}

void CifBridge::insertOutputMapping(std::string outputId, int index) {
    if (outputIdToIndex_.find(outputId) != outputIdToIndex_.end()) {
        log(::tlcfi::fi::LogLevel::Warning, "CifBridge: Output mapping already exists for " + outputId);
        return;
    }

    log(::tlcfi::fi::LogLevel::Debug,
        "CifBridge: Setting output mapping for " + outputId + " to index " + std::to_string(index));
    std::lock_guard<std::mutex> lock(mapMutex_);
    if (indexToOutputId_.size() <= static_cast<size_t>(index)) {
        indexToOutputId_.resize(index + 1);
    }
    indexToOutputId_[index] = outputId;
    outputIdToIndex_[std::move(outputId)] = index;
}

void CifBridge::setDetectorMapping(std::string detectorId, int index) {
    if (ignoredItems_.detectors.count(detectorId) != 0) {
        log(::tlcfi::fi::LogLevel::Info,
            "CifBridge: ignoring controller-only detector '" + detectorId + "' (declared absent in TLC)");
        matchedIgnoredDetectors_.insert(detectorId);
        return;
    }
    insertDetectorMapping(std::move(detectorId), index);
}

void CifBridge::insertDetectorMapping(std::string detectorId, int index) {
    if (detectorIdToIndex_.find(detectorId) != detectorIdToIndex_.end()) {
        log(::tlcfi::fi::LogLevel::Warning, "CifBridge: Detector mapping already exists for " + detectorId);
        return;
    }

    log(::tlcfi::fi::LogLevel::Debug,
        "CifBridge: Setting detector mapping for " + detectorId + " to index " + std::to_string(index));
    std::lock_guard<std::mutex> lock(mapMutex_);
    detectorIdToIndex_[std::move(detectorId)] = index;
}

void CifBridge::setInputMapping(std::string inputId, int index) {
    if (ignoredItems_.inputs.count(inputId) != 0) {
        log(::tlcfi::fi::LogLevel::Info,
            "CifBridge: ignoring controller-only input '" + inputId + "' (declared absent in TLC)");
        matchedIgnoredInputs_.insert(inputId);
        return;
    }
    insertInputMapping(std::move(inputId), index);
}

void CifBridge::insertInputMapping(std::string inputId, int index) {
    if (inputIdToIndex_.find(inputId) != inputIdToIndex_.end()) {
        log(::tlcfi::fi::LogLevel::Warning, "CifBridge: Input mapping already exists for " + inputId);
        return;
    }

    log(::tlcfi::fi::LogLevel::Debug,
        "CifBridge: Setting input mapping for " + inputId + " to index " + std::to_string(index));
    std::lock_guard<std::mutex> lock(mapMutex_);
    inputIdToIndex_[std::move(inputId)] = index;
}

void CifBridge::setIgnoredItems(IgnoredItems items) {
    ignoredItems_ = std::move(items);
    matchedIgnoredSignalGroups_.clear();
    matchedIgnoredOutputs_.clear();
    matchedIgnoredDetectors_.clear();
    matchedIgnoredInputs_.clear();
}

void CifBridge::getUnmatchedIgnoredItems(
    std::vector<std::string>& signalGroups,
    std::vector<std::string>& outputs,
    std::vector<std::string>& detectors,
    std::vector<std::string>& inputs) const
{
    signalGroups.clear();
    outputs.clear();
    detectors.clear();
    inputs.clear();

    auto collectUnmatched = [](const std::set<std::string>& declared,
                               const std::set<std::string>& matched,
                               std::vector<std::string>& out) {
        for (const auto& name : declared) {
            if (matched.count(name) == 0) out.push_back(name);
        }
    };
    collectUnmatched(ignoredItems_.signalGroups, matchedIgnoredSignalGroups_, signalGroups);
    collectUnmatched(ignoredItems_.outputs,      matchedIgnoredOutputs_,      outputs);
    collectUnmatched(ignoredItems_.detectors,    matchedIgnoredDetectors_,    detectors);
    collectUnmatched(ignoredItems_.inputs,       matchedIgnoredInputs_,       inputs);
}

void CifBridge::setManualCouples(ManualCouples couples) {
    manualCouples_ = std::move(couples);

    // Precompute per-type cifName→tlcfiName lookups and the set of tlcfiName
    // targets. The targets let ingestType detect a controller item whose name
    // is shadowed by a manual couple (the "us01 also exists literally" case).
    auto index = [](const std::vector<ManualCouple>& couples,
                    std::map<std::string, std::string>& cifToTlcfi,
                    std::set<std::string>& tlcfiKeys) {
        cifToTlcfi.clear();
        tlcfiKeys.clear();
        for (const auto& c : couples) {
            cifToTlcfi[c.cifName] = c.tlcfiName;
            tlcfiKeys.insert(c.tlcfiName);
        }
    };
    index(manualCouples_.signalGroups, manualSignalGroupCifToTlcfi_, manualSignalGroupTlcfiKeys_);
    index(manualCouples_.outputs,      manualOutputCifToTlcfi_,      manualOutputTlcfiKeys_);
    index(manualCouples_.detectors,    manualDetectorCifToTlcfi_,    manualDetectorTlcfiKeys_);
    index(manualCouples_.inputs,       manualInputCifToTlcfi_,       manualInputTlcfiKeys_);

    matchedManualSignalGroups_.clear();
    matchedManualOutputs_.clear();
    matchedManualDetectors_.clear();
    matchedManualInputs_.clear();
    unmappedControllerSignalGroups_.clear();
    unmappedControllerOutputs_.clear();
    unmappedControllerDetectors_.clear();
    unmappedControllerInputs_.clear();
}

void CifBridge::ingestType(
    const std::vector<std::pair<std::string, int>>& scanned,
    const std::map<std::string, std::string>& manualCifToTlcfi,
    const std::set<std::string>& manualTlcfiKeys,
    const std::set<std::string>& ignored,
    const char* typeLabel,
    std::set<std::string>& matchedManual,
    std::set<std::string>& matchedIgnored,
    std::vector<std::string>& unmappedControllerItems,
    void (CifBridge::*rawInsert)(std::string, int))
{
    matchedManual.clear();
    matchedIgnored.clear();
    unmappedControllerItems.clear();

    // Pass 1 — manual couples first, so they win over identity matches
    // regardless of the order the controller reports its items in. The item is
    // coupled under its tlcfiName (the map key is always the TLC-FI id). Use the
    // RAW inserter: the key is a TLC-FI name and must not be filtered against the
    // controller-only (CIF-name) set even if the two strings coincide.
    for (const auto& [cifName, index] : scanned) {
        auto it = manualCifToTlcfi.find(cifName);
        if (it != manualCifToTlcfi.end()) {
            matchedManual.insert(cifName);
            (this->*rawInsert)(it->second, index);
        }
    }

    // Pass 2 — automatic (identity) matches and controller-only exclusions.
    std::set<std::string> seenAuto;
    for (const auto& [cifName, index] : scanned) {
        if (manualCifToTlcfi.count(cifName) != 0) {
            // Already coupled in pass 1 under its tlcfiName. Must NOT be
            // re-inserted under its own name (that key would be stale).
            continue;
        }
        if (ignored.count(cifName) != 0) {
            // Controller-only: intentionally excluded from the mapping. Checked
            // before the collision test so declaring a shadowed item as
            // controller-only resolves the collision.
            log(::tlcfi::fi::LogLevel::Info,
                std::string("CifBridge: ignoring controller-only ") + typeLabel + " '" + cifName +
                "' (declared absent in TLC)");
            matchedIgnored.insert(cifName);
            continue;
        }
        if (manualTlcfiKeys.count(cifName) != 0 || seenAuto.count(cifName) != 0) {
            // Shadowed by a manual couple's tlcfiName target, or a duplicate
            // controller name: this slot couples to nothing. Fatal upstream.
            log(::tlcfi::fi::LogLevel::Warning,
                std::string("CifBridge: controller ") + typeLabel + " '" + cifName +
                "' is not coupled to any TLC object (shadowed by a manual couple or a duplicate name)");
            unmappedControllerItems.push_back(cifName);
            continue;
        }
        (this->*rawInsert)(cifName, index);
        seenAuto.insert(cifName);
    }
}

void CifBridge::buildMappings(
    const std::vector<std::pair<std::string, int>>& signalGroups,
    const std::vector<std::pair<std::string, int>>& outputs,
    const std::vector<std::pair<std::string, int>>& detectors,
    const std::vector<std::pair<std::string, int>>& inputs)
{
    ingestType(signalGroups, manualSignalGroupCifToTlcfi_, manualSignalGroupTlcfiKeys_,
               ignoredItems_.signalGroups, "signal group",
               matchedManualSignalGroups_, matchedIgnoredSignalGroups_,
               unmappedControllerSignalGroups_, &CifBridge::insertSignalGroupMapping);
    ingestType(outputs, manualOutputCifToTlcfi_, manualOutputTlcfiKeys_,
               ignoredItems_.outputs, "output",
               matchedManualOutputs_, matchedIgnoredOutputs_,
               unmappedControllerOutputs_, &CifBridge::insertOutputMapping);
    ingestType(detectors, manualDetectorCifToTlcfi_, manualDetectorTlcfiKeys_,
               ignoredItems_.detectors, "detector",
               matchedManualDetectors_, matchedIgnoredDetectors_,
               unmappedControllerDetectors_, &CifBridge::insertDetectorMapping);
    ingestType(inputs, manualInputCifToTlcfi_, manualInputTlcfiKeys_,
               ignoredItems_.inputs, "input",
               matchedManualInputs_, matchedIgnoredInputs_,
               unmappedControllerInputs_, &CifBridge::insertInputMapping);
}

void CifBridge::getUnmatchedManualCouples(
    std::vector<ManualCouple>& signalGroups,
    std::vector<ManualCouple>& outputs,
    std::vector<ManualCouple>& detectors,
    std::vector<ManualCouple>& inputs) const
{
    signalGroups.clear();
    outputs.clear();
    detectors.clear();
    inputs.clear();

    auto collect = [](const std::vector<ManualCouple>& declared,
                      const std::set<std::string>& matched,
                      std::vector<ManualCouple>& out) {
        for (const auto& c : declared) {
            if (matched.count(c.cifName) == 0) out.push_back(c);
        }
    };
    collect(manualCouples_.signalGroups, matchedManualSignalGroups_, signalGroups);
    collect(manualCouples_.outputs,      matchedManualOutputs_,      outputs);
    collect(manualCouples_.detectors,    matchedManualDetectors_,    detectors);
    collect(manualCouples_.inputs,       matchedManualInputs_,       inputs);
}

void CifBridge::getUnmappedControllerItems(
    std::vector<std::string>& signalGroups,
    std::vector<std::string>& outputs,
    std::vector<std::string>& detectors,
    std::vector<std::string>& inputs) const
{
    signalGroups = unmappedControllerSignalGroups_;
    outputs      = unmappedControllerOutputs_;
    detectors    = unmappedControllerDetectors_;
    inputs       = unmappedControllerInputs_;
}

void CifBridge::setSignalGroupCount(int count) {
    log(::tlcfi::fi::LogLevel::Debug, "CifBridge: Setting signal group count to " + std::to_string(count));
    signalGroupCount_ = count;
}

void CifBridge::setDetectorCount(int count) {
    log(::tlcfi::fi::LogLevel::Debug, "CifBridge: Setting detector count to " + std::to_string(count));
    detectorCount_ = count;
}

bool CifBridge::validateMappings() const {
    std::vector<std::string> unmappedSg, unmappedOut, unmappedDet, unmappedIn;
    getUnmappedObjects(unmappedSg, unmappedOut, unmappedDet, unmappedIn);

    std::vector<std::string> staleSg, staleOut, staleDet, staleIn;
    getStaleObjects(staleSg, staleOut, staleDet, staleIn);

    return unmappedSg.empty() && unmappedOut.empty() && unmappedDet.empty() && unmappedIn.empty()
        && staleSg.empty() && staleOut.empty() && staleDet.empty() && staleIn.empty();
}

void CifBridge::getUnmappedObjects(
    std::vector<std::string>& unmappedSignalGroups,
    std::vector<std::string>& unmappedOutputs,
    std::vector<std::string>& unmappedDetectors,
    std::vector<std::string>& unmappedInputs) const
{
    unmappedSignalGroups.clear();
    unmappedOutputs.clear();
    unmappedDetectors.clear();
    unmappedInputs.clear();

    // Get all object IDs from the TLC
    auto tlcSignalGroups = app_.getSignalGroupIds();
    auto tlcOutputs = app_.getOutputIds();
    auto tlcDetectors = app_.getDetectorIds();
    auto tlcInputs = app_.getInputIds();

    // Check signal groups
    for (const auto& id : tlcSignalGroups) {
        if (signalGroupIdToIndex_.find(id) == signalGroupIdToIndex_.end()) {
            unmappedSignalGroups.push_back(id);
        }
    }

    // Check outputs
    for (const auto& id : tlcOutputs) {
        if (outputIdToIndex_.find(id) == outputIdToIndex_.end()) {
            unmappedOutputs.push_back(id);
        }
    }

    // Check detectors
    for (const auto& id : tlcDetectors) {
        if (detectorIdToIndex_.find(id) == detectorIdToIndex_.end()) {
            unmappedDetectors.push_back(id);
        }
    }

    // Check inputs
    for (const auto& id : tlcInputs) {
        if (inputIdToIndex_.find(id) == inputIdToIndex_.end()) {
            unmappedInputs.push_back(id);
        }
    }
}

void CifBridge::handleSpecialVehicleEvent(const ::tlcfi::tlcfi::SpecialVehicleEvent& evt) {
    std::lock_guard<std::mutex> lock(dsiMutex_);
    dsiQueue_.push(evt);
}

namespace {

/**
 * @brief Converts decimal degrees to degrees/minutes/seconds/hundredths.
 */
void decimalDegreesToDms(double dd, int& degrees, int& minutes, int& seconds, int& hundredths) {
    double abs_dd = std::abs(dd);
    degrees = static_cast<int>(abs_dd);
    double remainder = (abs_dd - degrees) * 60.0;
    minutes = static_cast<int>(remainder);
    remainder = (remainder - minutes) * 60.0;
    seconds = static_cast<int>(remainder);
    hundredths = static_cast<int>((remainder - seconds) * 100.0);
    if (dd < 0) degrees = -degrees;
}

} // anonymous namespace

void CifBridge::prepareDsiBuffer() {
    std::lock_guard<std::mutex> lock(dsiMutex_);
    if (dsiQueue_.empty()) {
        CIF_DSIWIJZ = 0;
        return;
    }

    auto evt = dsiQueue_.front();
    dsiQueue_.pop();

    // Clear buffer (36 fields: CIF_DSI_LUS through CIF_DSI_RES2)
    for (int i = 0; i <= CIF_DSI_RES2; ++i) {
        CIF_DSI[i] = 0;
    }

    // Set defaults for fields with non-zero defaults
    CIF_DSI[CIF_DSI_TSTP] = CIF_DSI_TSTP_DEF;
    CIF_DSI[CIF_DSI_LSS] = CIF_DSI_LSS_DEF;
    CIF_DSI[CIF_DSI_TSS] = CIF_DSI_TSS_DEF;

    // Map SpecialVehicleEvent fields to CIF_DSI[]
    if (evt.virtualLoop.has_value())     CIF_DSI[CIF_DSI_LUS]  = static_cast<s_int16>(*evt.virtualLoop);
    if (evt.vehType.has_value())         CIF_DSI[CIF_DSI_VTG]  = static_cast<s_int16>(*evt.vehType);
    if (evt.lineNr.has_value())          CIF_DSI[CIF_DSI_LYN]  = static_cast<s_int16>(*evt.lineNr);
    if (evt.serviceNr.has_value())       CIF_DSI[CIF_DSI_WDNST] = static_cast<s_int16>(*evt.serviceNr);
    if (evt.companyNr.has_value())       CIF_DSI[CIF_DSI_BEDR] = static_cast<s_int16>(*evt.companyNr);
    if (evt.vehId.has_value())           CIF_DSI[CIF_DSI_NUM]  = static_cast<s_int16>(*evt.vehId);
    if (evt.directionSG.has_value())     CIF_DSI[CIF_DSI_DIR]  = static_cast<s_int16>(*evt.directionSG);
    if (evt.status.has_value())          CIF_DSI[CIF_DSI_STAT] = static_cast<s_int16>(*evt.status);
    if (evt.priorityClass.has_value())   CIF_DSI[CIF_DSI_PRI]  = static_cast<s_int16>(static_cast<int>(*evt.priorityClass));
    if (evt.punctuality.has_value())     CIF_DSI[CIF_DSI_STP]  = static_cast<s_int16>(static_cast<int>(*evt.punctuality));
    if (evt.punctualityTime.has_value()) CIF_DSI[CIF_DSI_TSTP] = static_cast<s_int16>(*evt.punctualityTime);
    if (evt.length.has_value())          CIF_DSI[CIF_DSI_LEN]  = static_cast<s_int16>(*evt.length);
    if (evt.speed.has_value())           CIF_DSI[CIF_DSI_SPD]  = static_cast<s_int16>(*evt.speed);
    if (evt.distToStopLine.has_value())  CIF_DSI[CIF_DSI_LSS]  = static_cast<s_int16>(*evt.distToStopLine);
    if (evt.timeToStopLine.has_value())  CIF_DSI[CIF_DSI_TSS]  = static_cast<s_int16>(*evt.timeToStopLine);
    if (evt.journeyNr.has_value())       CIF_DSI[CIF_DSI_RIT]  = static_cast<s_int16>(*evt.journeyNr);
    if (evt.journeyCat.has_value())      CIF_DSI[CIF_DSI_RITC] = static_cast<s_int16>(*evt.journeyCat);
    if (evt.routePT.has_value())         CIF_DSI[CIF_DSI_ROUT] = static_cast<s_int16>(*evt.routePT);
    if (evt.type.has_value())            CIF_DSI[CIF_DSI_TYPE] = static_cast<s_int16>(static_cast<int>(*evt.type));
    if (evt.activationPointNr.has_value()) CIF_DSI[CIF_DSI_OVC] = static_cast<s_int16>(*evt.activationPointNr);

    // Location: convert decimal degrees to DMS
    if (evt.location.has_value()) {
        int deg, min, sec, hsec;
        decimalDegreesToDms(evt.location->latitude, deg, min, sec, hsec);
        CIF_DSI[CIF_DSI_XGRAD] = static_cast<s_int16>(deg);
        CIF_DSI[CIF_DSI_XMIN]  = static_cast<s_int16>(min);
        CIF_DSI[CIF_DSI_XSEC]  = static_cast<s_int16>(sec);
        CIF_DSI[CIF_DSI_XHSEC] = static_cast<s_int16>(hsec);

        decimalDegreesToDms(evt.location->longitude, deg, min, sec, hsec);
        CIF_DSI[CIF_DSI_YGRAD] = static_cast<s_int16>(deg);
        CIF_DSI[CIF_DSI_YMIN]  = static_cast<s_int16>(min);
        CIF_DSI[CIF_DSI_YSEC]  = static_cast<s_int16>(sec);
        CIF_DSI[CIF_DSI_YHSEC] = static_cast<s_int16>(hsec);
    }

    // DateTime fields
    if (evt.dateTime.has_value()) {
        if (evt.dateTime->y.has_value())   CIF_DSI[CIF_DSI_JAAR] = static_cast<s_int16>(*evt.dateTime->y);
        if (evt.dateTime->m.has_value())   CIF_DSI[CIF_DSI_MND]  = static_cast<s_int16>(*evt.dateTime->m);
        if (evt.dateTime->d.has_value())   CIF_DSI[CIF_DSI_DAG]  = static_cast<s_int16>(*evt.dateTime->d);
        if (evt.dateTime->h.has_value())   CIF_DSI[CIF_DSI_UUR]  = static_cast<s_int16>(*evt.dateTime->h);
        if (evt.dateTime->min.has_value()) CIF_DSI[CIF_DSI_MIN]  = static_cast<s_int16>(*evt.dateTime->min);
        if (evt.dateTime->s.has_value())   CIF_DSI[CIF_DSI_SEC]  = static_cast<s_int16>(*evt.dateTime->s);
    }

    // Reserve fields
    if (evt.reserve23.has_value()) CIF_DSI[CIF_DSI_RES1] = static_cast<s_int16>(*evt.reserve23);
    if (evt.reserve24.has_value()) CIF_DSI[CIF_DSI_RES2] = static_cast<s_int16>(*evt.reserve24);

    CIF_DSIWIJZ = 1;
}

std::string CifBridge::sanitizeSystemName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (std::isalnum(c) != 0 || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        }
    }
    if (out.empty()) {
        out = "SYSTEM";
    }
    return out;
}

std::string CifBridge::buildFileStem(std::chrono::system_clock::time_point now) const {
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << fileLogSystemName_ << "_" << std::put_time(&tm, "%Y%m%d_%H%M%S");
    return oss.str();
}

void CifBridge::finalizeActiveFile() {
    if (fileLogStream_.is_open()) {
        fileLogStream_.flush();
        fileLogStream_.close();
    }
    if (!activeTempFilePath_.empty() && !activeFinalFilePath_.empty()) {
        std::error_code ec;
        std::filesystem::rename(activeTempFilePath_, activeFinalFilePath_, ec);
        if (ec) {
            log(::tlcfi::fi::LogLevel::Error,
                "CifBridge: failed to finalize vlog file '"
                + activeTempFilePath_.string() + "' -> '"
                + activeFinalFilePath_.string() + "': " + ec.message());
        } else if (fileLogMaxFiles_ > 0) {
            pruneOldVlogFiles();
        }
    }
    activeTempFilePath_.clear();
    activeFinalFilePath_.clear();
}

void CifBridge::pruneOldVlogFiles() {
    const std::string prefix = fileLogSystemName_ + "_";
    std::vector<std::filesystem::path> vlgFiles;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(fileLogOutputDir_, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const auto& p = entry.path();
        if (p.extension() != ".vlg") continue;
        const std::string fname = p.filename().string();
        if (fname.substr(0, prefix.size()) == prefix) {
            vlgFiles.push_back(p);
        }
    }

    if (static_cast<int>(vlgFiles.size()) <= fileLogMaxFiles_) {
        return;
    }

    // Sort ascending by filename (timestamp embedded → oldest first)
    std::sort(vlgFiles.begin(), vlgFiles.end());

    const int toRemove = static_cast<int>(vlgFiles.size()) - fileLogMaxFiles_;
    for (int i = 0; i < toRemove; ++i) {
        std::filesystem::remove(vlgFiles[static_cast<std::size_t>(i)], ec);
        if (ec) {
            log(::tlcfi::fi::LogLevel::Warning,
                "CifBridge: failed to remove old vlog file '"
                + vlgFiles[static_cast<std::size_t>(i)].string() + "': " + ec.message());
        }
    }
}

void CifBridge::rotateFile(std::chrono::system_clock::time_point now) {
    finalizeActiveFile();

    std::error_code ec;
    std::filesystem::create_directories(fileLogOutputDir_, ec);
    if (ec) {
        log(::tlcfi::fi::LogLevel::Error,
            "CifBridge: failed to create vlog output directory '"
            + fileLogOutputDir_.string() + "': " + ec.message());
        return;
    }

    const std::string stem = buildFileStem(now);
    activeFinalFilePath_ = fileLogOutputDir_ / (stem + ".vlg");
    activeTempFilePath_ = fileLogOutputDir_ / (stem + ".tmp");
    fileLogStream_.open(activeTempFilePath_, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!fileLogStream_.is_open()) {
        log(::tlcfi::fi::LogLevel::Error,
            "CifBridge: failed to open vlog temp file '" + activeTempFilePath_.string() + "'");
        activeTempFilePath_.clear();
        activeFinalFilePath_.clear();
        return;
    }

    nextRotationRequestAt_ = now + std::chrono::minutes(5);
}

void CifBridge::ensureFileLogStreamOpen() {
    if (!fileLoggingEnabled_ || fileLogStream_.is_open()) {
        return;
    }
    rotateFile(std::chrono::system_clock::now());
}

void CifBridge::requestFileSwitchIfDue(std::chrono::system_clock::time_point now) {
    if (!fileLoggingEnabled_) {
        return;
    }
    if (nextRotationRequestAt_.time_since_epoch().count() == 0) {
        nextRotationRequestAt_ = now + std::chrono::minutes(5);
        return;
    }
    if (now >= nextRotationRequestAt_ && !fileSwitchRequested_) {
        CIF_PB_FILE_UBER_FILEWISSEL = 1;
        fileSwitchRequested_ = true;
    }
}

void CifBridge::pollFileVlog() {
    // CIF_FILE_UBER must be drained every cycle regardless of whether we are
    // actually writing files. CCOL's V-Log writer (CCOLVLOG.lib) keeps pushing
    // bytes into this ring on every cycle that has activity; if nothing
    // consumes them, the ring fills and CCOL stalls inside applicatieprogramma
    // while we still hold cifMutex_, freezing the loop. The two failure modes
    // we are fixing here:
    //   1. fileLoggingEnabled_ == false → previously returned early, never
    //      drained.
    //   2. fileLoggingEnabled_ == true but fileLogStream_ failed to open →
    //      previously broke out of the loop on the first byte, never drained.
    auto now = std::chrono::system_clock::now();
    if (fileLoggingEnabled_) {
        requestFileSwitchIfDue(now);
    }

    s_int8* start = CIF_FILE_UBER;
    s_int8* end = CIF_FILE_UBER + CIF_PB_MAX_FILE_UBER;
    s_int8* readPtr = CIF_FILE_UBER_LEES;
    const s_int8* writePtr = CIF_FILE_UBER_SCHRIJF;

    while (readPtr != writePtr) {
        if (CIF_FILE_UBER_FILEWISSEL != nullptr && readPtr == CIF_FILE_UBER_FILEWISSEL) {
            if (fileLoggingEnabled_) {
                rotateFile(now);
            }
            // Acknowledge the file-switch request even when logging is off,
            // so CCOL does not wait on us.
            CIF_FILE_UBER_FILEWISSEL = nullptr;
            CIF_PB_FILE_UBER_FILEWISSEL = 0;
            fileSwitchRequested_ = false;
        }

        if (fileLoggingEnabled_) {
            ensureFileLogStreamOpen();
            if (fileLogStream_.is_open()) {
                fileLogStream_.put(static_cast<char>(*readPtr));
            }
            // Stream not open (rotation/open failed): silently discard the
            // byte so the ring still drains.
        }

        ++readPtr;
        if (readPtr >= end) {
            readPtr = start;
        }
    }

    CIF_FILE_UBER_LEES = readPtr;
}

void CifBridge::synchronizeInputStates() {
    // Two-phase: gather states first (app_.getDetector/getInput lock
    // FiConsumerApp's state mutex), then write CIF_IS[] under cifMutex_. This
    // avoids holding cifMutex_ across a stateMutex_ acquisition, which would
    // invert the lock order used by the TLC-FI handlers (stateMutex_ ->
    // cifMutex_) and risk a deadlock.
    std::vector<std::pair<int, int>> writes;      // (CIF_IS index, value)
    std::vector<std::pair<int, int>> swicoWrites; // (CIF_IS_SWICO index, value)

    auto detectorIds = app_.getDetectorIds();
    for (const auto& detId : detectorIds) {
        int idx = translateDetectorIdToIndex(detId);
        if (idx < 0 || idx >= kCifIsCapacity) continue;  // Skip unmapped/out-of-range
        auto det = app_.getDetector(detId);
        if (!det.id.empty()) {
            int value = (det.state == ::tlcfi::tlcfi::DetectorState::Occupied) ? CIF_DET_BEZET : 0;
            writes.emplace_back(idx, value);
            swicoWrites.emplace_back(idx, static_cast<int>(det.swico));
        }
    }

    auto inputIds = app_.getInputIds();
    for (const auto& inId : inputIds) {
        int idx = translateInputIdToIndex(inId);
        if (idx < 0 || idx >= kCifIsCapacity) continue;  // Skip unmapped/out-of-range
        auto input = app_.getInput(inId);
        if (!input.id.empty()) {
            writes.emplace_back(idx, static_cast<int>(input.state));
            swicoWrites.emplace_back(idx, static_cast<int>(input.swico));
        }
    }

    std::lock_guard<std::mutex> lock(cifMutex_);
    for (const auto& [idx, value] : writes) {
        CIF_IS[idx] = static_cast<s_int16>(value);
    }
    for (const auto& [idx, value] : swicoWrites) {
        CIF_IS_SWICO[idx] = static_cast<s_int16>(value);
    }
    // Set the change flag to indicate CIF_IS[] has been updated
    CIF_ISWIJZ = 1;
}

void CifBridge::reset() {
    {
        std::lock_guard<std::mutex> lock(cifMutex_);
        pendingSgRequests_.clear();
        pendingOutputsRequests_.clear();
    }
    fileSwitchRequested_ = false;
    CIF_PB_FILE_UBER_FILEWISSEL = 0;
    finalizeActiveFile();

    // Clear DSI queue to prevent stale events after reconnection
    {
        std::lock_guard<std::mutex> dsiLock(dsiMutex_);
        std::queue<::tlcfi::tlcfi::SpecialVehicleEvent>().swap(dsiQueue_);
    }
}

void CifBridge::getStaleObjects(
    std::vector<std::string>& staleSignalGroups,
    std::vector<std::string>& staleOutputs,
    std::vector<std::string>& staleDetectors,
    std::vector<std::string>& staleInputs) const
{
    staleSignalGroups.clear();
    staleOutputs.clear();
    staleDetectors.clear();
    staleInputs.clear();

    auto tlcSignalGroups = app_.getSignalGroupIds();
    auto tlcOutputs = app_.getOutputIds();
    auto tlcDetectors = app_.getDetectorIds();
    auto tlcInputs = app_.getInputIds();

    auto contains = [](const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };

    for (const auto& [id, _] : signalGroupIdToIndex_) {
        if (!contains(tlcSignalGroups, id)) staleSignalGroups.push_back(id);
    }
    for (const auto& [id, _] : outputIdToIndex_) {
        if (!contains(tlcOutputs, id)) staleOutputs.push_back(id);
    }
    for (const auto& [id, _] : detectorIdToIndex_) {
        if (!contains(tlcDetectors, id)) staleDetectors.push_back(id);
    }
    for (const auto& [id, _] : inputIdToIndex_) {
        if (!contains(tlcInputs, id)) staleInputs.push_back(id);
    }
}

} // namespace cif


