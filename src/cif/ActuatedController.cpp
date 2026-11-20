/**
 * @file ActuatedController.cpp
 * @brief Simplest possible actuated traffic controller using CIF interface
 *
 * This file:
 * 1. Defines CIF buffer sizes and allocates CIF arrays (via CIF_PUBLIC)
 * 2. Implements the actuated control logic in applicatieprogramma()
 *
 * The controller is completely isolated from TLC-FI. It only uses CIF buffers
 * for state storage. Communication with TLC-FI happens through CifBridge.
 *
 * State machine per signal group: Red → Green → Amber → Red
 * - Request: detector occupied during red
 * - Extend: detector occupied during green (up to maxGreen)
 * - Timing: counts applicatieprogramma() calls (each = 1 tenth of a second)
 */

#include "cif/ActuatedController.hpp"
#include "cif/CifController.h"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <algorithm>
#include <iostream>
#include <set>
#include <vector>

// --- CIF buffer size definitions ---
// These must be defined before including CIF_INC.h with CIF_PUBLIC.
//
// IMPORTANT: the container's automatic id<->index mapping reads the per-type
// object COUNTS from the CVN-C constants derived from these defines
// (CIF_PB_AANT_US_FC = CIF_AANT_US_FC, etc.). A conformant controller sizes its
// buffers to its EXACT object counts, so these four must match the runtime
// configuration that is loaded (here: examples/controller_config.json). If you
// change that JSON's number of signal groups / outputs / detectors / inputs,
// you MUST adjust these defines to match, otherwise the mapping offsets and the
// per-type counts will be wrong. (A real controller compiled for one
// intersection has no JSON and simply hard-sizes these.)
#define CIF_AANT_US_FC      7   // Signal groups (fasecycli)  -> CIF_GUS[] / CIF_WUS[]
#define CIF_AANT_US_OV      4   // Other outputs              -> CIF_GUS[] / CIF_WUS[]
#define CIF_AANT_IS_D       9   // Detectors                  -> CIF_IS[]
#define CIF_AANT_IS_OV      5   // Other inputs               -> CIF_IS[]
#define CIF_AANT_DSI        36  // DSI buffer size (36 fields per SpecialVehicleEvent)
#define CIF_AANT_GPS        10  // Program status buffer
#define CIF_AANT_WPS        10  // Program status buffer
#define CIF_AANT_KLOK       10  // Clock buffer (standard size)
#define CIF_AANT_PARM1      1   // Minimal parameter buffer
#define CIF_AANT_PARM2      1   // Minimal parameter buffer
#define CIF_MAX_UBER        4096 // Message buffer for outgoing serial messages
#define CIF_MAX_IBER        4096 // Message buffer for incoming serial messages
#define CIF_MAX_MON_IBER    1   // Minimal monitor buffer
#define CIF_MAX_MON_UBER    4096 // Minimal monitor buffer
#define CIF_MAX_RIS_UBER    4096 // Minimal RIS buffer
#define CIF_MAX_FILE_UBER   16384 // File logging ring buffer
#define CIF_MAX_FILEEXT     4   // Minimal file extension buffer

// CIF_INC.h defines several CIF_PB_* constants that the container reads across
// translation units for mapping, persistence, and ring-buffer bounds. In C a
// namespace-scope const has external linkage automatically; this file is C++,
// where such a const defaults to INTERNAL linkage. Pre-declaring every
// cross-TU constant extern forces the subsequent CIF_INC.h definitions to
// export symbols. (s_int16 == short; the typedef is not in scope yet, so
// `short` is used here. A controller written in C does not need this shim.)
extern "C" {
    extern const short CIF_PB_AANT_US_FC;
    extern const short CIF_PB_AANT_US_OV;
    extern const short CIF_PB_AANT_IS_D;
    extern const short CIF_PB_AANT_IS_OV;
    extern const short CIF_PB_AANT_PARM1;
    extern const short CIF_PB_MAX_UBER;
    extern const short CIF_PB_MAX_IBER;
    extern const short CIF_PB_MAX_MON_IBER;
    extern const short CIF_PB_MAX_MON_UBER;
    extern const short CIF_PB_MAX_FILE_UBER;
}

// Allocate all CIF arrays
#define CIF_PUBLIC
#include "cif/CIF_INC.h"

namespace {

// --- Internal controller state ---

enum class Phase {
    Red,
    Green,
    Amber
};

struct SignalGroupState {
    Phase phase = Phase::Red;
    int timer = 0;           // Ticks in current phase (1 tick = 100ms = 0.1s)
    bool request = false;
    bool servedThisCycle = false;  // True if this SG has had green since last cycle reset
    int priorityVehicleCount = 0;  ///< Number of priority vehicles waiting/in-progress
    int waitingTime = 0;           ///< Time since request started (tenths of seconds)
    bool hasPriorityRequest = false; ///< True if priority vehicle is waiting
};

// Intergreen timer state: tracks remaining intergreen time for each conflict
// Key: index into g_config.conflicts, Value: remaining ticks
struct IntergreenState {
    int remainingTime = 0;  // Remaining intergreen time in ticks (0 = inactive)
};

// Global state
cif::IntersectionConfig g_config;
SignalGroupState g_sgState[CIF_AANT_US_FC];
std::vector<IntergreenState> g_intergreenTimers;

/**
 * @brief Find detector index in CIF_IS[] by detector ID
 * @return CIF_IS index, or -1 if not found
 */
int findDetectorIndex(const std::string& id) {
    for (const auto& det : g_config.detectors) {
        if (det.id == id) return det.index;
    }
    return -1;
}

/**
 * @brief Find signal group config by ID
 * @return Pointer to signal group config, or nullptr if not found
 */
const cif::SignalGroupConfig* findSignalGroup(const std::string& id) {
    for (const auto& sg : g_config.signalGroups) {
        if (sg.id == id) return &sg;
    }
    return nullptr;
}

/**
 * @brief Check if a signal group can become green based on conflict matrix
 *
 * A signal group is blocked from becoming green if any of the following is true:
 * - A conflicting "from" signal group is currently green
 * - An intergreen timer from a conflicting "from" signal group is still running
 *
 * @param toId The signal group ID that wants to become green
 * @return true if the signal group can become green, false if blocked
 */
bool canBecomeGreen(const std::string& toId) {
    for (size_t i = 0; i < g_config.conflicts.size(); ++i) {
        const auto& conflict = g_config.conflicts[i];
        if (conflict.toId != toId) continue;

        // Check if the "from" signal group is green
        const auto* fromSg = findSignalGroup(conflict.fromId);
        if (fromSg && fromSg->index >= 0 && fromSg->index < CIF_AANT_US_FC) {
            if (g_sgState[fromSg->index].phase == Phase::Green) {
                return false;  // Blocked: from-signalgroup is green
            }
        }

        // Check if intergreen timer is still running
        if (i < g_intergreenTimers.size() && g_intergreenTimers[i].remainingTime > 0) {
            return false;  // Blocked: intergreen timer still running
        }
    }
    return true;
}

/**
 * @brief Start intergreen timers for all conflicts where fromId is the "from" signal group
 * @param fromId The signal group that just ended green
 */
void startIntergreenTimers(const std::string& fromId) {
    for (size_t i = 0; i < g_config.conflicts.size(); ++i) {
        const auto& conflict = g_config.conflicts[i];
        if (conflict.fromId == fromId && i < g_intergreenTimers.size()) {
            g_intergreenTimers[i].remainingTime = conflict.intergreen;
        }
    }
}

/**
 * @brief Decrement all active intergreen timers
 */
void tickIntergreenTimers() {
    for (auto& timer : g_intergreenTimers) {
        if (timer.remainingTime > 0) {
            timer.remainingTime--;
        }
    }
}

/**
 * @brief Check if any detector belonging to a signal group is occupied
 */
bool anyDetectorOccupied(const cif::SignalGroupConfig& sg) {
    for (const auto& detId : sg.detectorIds) {
        int idx = findDetectorIndex(detId);
        if (idx >= 0 && (CIF_IS[idx] & CIF_DET_BEZET)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Check if there are any unserved signal groups with pending requests
 *
 * Used for fairness: a signal group that has been served this cycle should
 * wait for other requesting signal groups before going green again.
 *
 * @return true if at least one signal group has a request but hasn't been served
 */
bool hasUnservedRequests() {
    for (const auto& sg : g_config.signalGroups) {
        if (sg.index >= 0 && sg.index < CIF_AANT_US_FC) {
            const auto& state = g_sgState[sg.index];
            if (state.request && !state.servedThisCycle && state.phase == Phase::Red) {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Reset the servedThisCycle flag for all signal groups
 *
 * Called when all requesting signal groups have been served (or none are requesting),
 * allowing a new cycle to begin.
 */
void resetServedFlags() {
    for (int i = 0; i < CIF_AANT_US_FC; ++i) {
        g_sgState[i].servedThisCycle = false;
    }
}

/**
 * @brief Check if a signal group is allowed to become green based on fairness
 *
 * A signal group should wait if:
 * - It has already been served this cycle, AND
 * - There are other signal groups with requests that haven't been served yet
 *
 * Signal groups with priority vehicles should always be allowed
 * (subject to conflict checks).
 *
 * @param sgIndex The index of the signal group wanting to go green
 * @return true if the signal group is allowed to go green
 */
bool fairnessAllowsGreen(int sgIndex) {
    const auto& state = g_sgState[sgIndex];

    // Signal groups with priority vehicles should always be allowed
    // (subject to conflict checks)
    if (state.hasPriorityRequest) {
        return true;
    }

    // If not served this cycle, always allow (subject to conflict checks)
    if (!state.servedThisCycle) {
        return true;
    }

    // If served this cycle, only allow if no unserved requests exist
    return !hasUnservedRequests();
}

s_int8* advanceFileUberPtr(s_int8* ptr) {
    ++ptr;
    if (ptr >= (CIF_FILE_UBER + CIF_PB_MAX_FILE_UBER)) {
        ptr = CIF_FILE_UBER;
    }
    return ptr;
}

/**
 * @brief Returns the number of free bytes in a CIF ring buffer.
 *
 * One slot is always kept free so that readPtr == writePtr unambiguously
 * means "empty". Callers must not write more than this many bytes, otherwise
 * the write pointer would lap the read pointer and corrupt unread data.
 *
 * @param readPtr  Current read pointer into the ring.
 * @param writePtr Current write pointer into the ring.
 * @param capacity Total ring capacity in bytes.
 */
std::size_t ringFreeSpace(const s_int8* readPtr, const s_int8* writePtr, std::size_t capacity) {
    if (capacity < 2) {
        return 0;
    }
    std::size_t used = (writePtr >= readPtr)
        ? static_cast<std::size_t>(writePtr - readPtr)
        : capacity - static_cast<std::size_t>(readPtr - writePtr);
    return capacity - 1 - used;
}

bool writeFileUberLine(const std::string& line, bool forceSwitchPoint) {
    if (CIF_PB_MAX_FILE_UBER <= 1) {
        return false;
    }

    s_int8* writePtr = CIF_FILE_UBER_SCHRIJF;
    s_int8* recordStart = writePtr;

    // Keep the latest safe file-switch point whenever no pointer is active,
    // or immediately when PB asked for a new switch point.
    if (forceSwitchPoint || CIF_FILE_UBER_FILEWISSEL == nullptr) {
        CIF_FILE_UBER_FILEWISSEL = recordStart;
    }

    for (char c : line) {
        s_int8* next = advanceFileUberPtr(writePtr);
        if (next == CIF_FILE_UBER_LEES) {
            return false; // Ring full, retry in a next cycle.
        }
        *writePtr = static_cast<s_int8>(c);
        writePtr = next;
    }

    CIF_FILE_UBER_SCHRIJF = writePtr;
    return true;
}

} // anonymous namespace

namespace cif {

IntersectionConfig loadConfigFromJson(const std::string& json) {
    auto j = nlohmann::json::parse(json);
    IntersectionConfig config;

    if (j.contains("signalGroups")) {
        for (const auto& sgJson : j["signalGroups"]) {
            SignalGroupConfig sg;
            sg.id = sgJson.at("id").get<std::string>();
            sg.index = sgJson.at("index").get<int>();
            sg.minGreen = sgJson.value("minGreen", 60);
            sg.maxGreen = sgJson.value("maxGreen", 120);
            sg.amber = sgJson.value("amber", 30);
            sg.minRed = sgJson.value("minRed", 20);
            sg.direction = sgJson.value("direction", -1);
            sg.priorityMaxGreen = sgJson.value("priorityMaxGreen", 180);
            if (sgJson.contains("detectors")) {
                sg.detectorIds = sgJson["detectors"].get<std::vector<std::string>>();
            }

            if (sg.index < 0 || sg.index >= CIF_AANT_US_FC) {
                throw std::runtime_error(
                    "Signal group index " + std::to_string(sg.index) +
                    " out of range [0, " + std::to_string(CIF_AANT_US_FC - 1) + "]");
            }
            config.signalGroups.push_back(std::move(sg));
        }
    }

    if (j.contains("detectors")) {
        for (const auto& detJson : j["detectors"]) {
            DetectorConfig det;
            det.id = detJson.at("id").get<std::string>();
            det.index = detJson.at("index").get<int>();

            if (det.index < 0 || det.index >= CIF_AANT_IS_D) {
                throw std::runtime_error(
                    "Detector index " + std::to_string(det.index) +
                    " out of range [0, " + std::to_string(CIF_AANT_IS_D - 1) + "]");
            }
            config.detectors.push_back(std::move(det));
        }
    }

    if (j.contains("inputs")) {
        for (const auto& inJson : j["inputs"]) {
            InputConfig inp;
            inp.id = inJson.at("id").get<std::string>();
            inp.index = inJson.at("index").get<int>();

            if (inp.index < 0 || inp.index >= CIF_AANT_IS_OV) {
                throw std::runtime_error(
                    "Input index " + std::to_string(inp.index) +
                    " out of range [0, " + std::to_string(CIF_AANT_IS_OV - 1) + "]");
            }

            config.inputs.push_back(std::move(inp));
        }
    }

    if (j.contains("outputs")) {
        for (const auto& outJson : j["outputs"]) {
            OutputConfig out;
            out.id = outJson.at("id").get<std::string>();
            out.index = outJson.at("index").get<int>();

            if (out.index < 0 || out.index >= CIF_AANT_US_OV) {
                throw std::runtime_error(
                    "Output index " + std::to_string(out.index) +
                    " out of range [0, " + std::to_string(CIF_AANT_US_OV - 1) + "]");
            }

            config.outputs.push_back(std::move(out));
        }
    }

    if (j.contains("conflicts")) {
        for (const auto& conflictJson : j["conflicts"]) {
            ConflictConfig conflict;
            conflict.fromId = conflictJson.at("from").get<std::string>();
            conflict.toId = conflictJson.at("to").get<std::string>();
            conflict.intergreen = conflictJson.at("intergreen").get<int>();

            if (conflict.intergreen < 0) {
                throw std::runtime_error(
                    "Intergreen time must be non-negative, got " +
                    std::to_string(conflict.intergreen));
            }

            config.conflicts.push_back(std::move(conflict));
        }
    }

    return config;
}

void setIntersectionConfig(const IntersectionConfig& config) {
    // The compiled CIF buffer capacities (the CIF_AANT_* defines at the top of
    // this file) must be able to hold the loaded configuration. For correct
    // automatic-mapping counts/offsets they should EQUAL the configured object
    // counts — see the note on those defines.
    if (static_cast<int>(config.signalGroups.size()) > CIF_PB_AANT_US_FC ||
        static_cast<int>(config.outputs.size())      > CIF_PB_AANT_US_OV ||
        static_cast<int>(config.detectors.size())    > CIF_PB_AANT_IS_D  ||
        static_cast<int>(config.inputs.size())       > CIF_PB_AANT_IS_OV) {
        throw std::runtime_error(
            "Configuration exceeds compiled CIF buffer capacity; adjust the "
            "CIF_AANT_* defines in ActuatedController.cpp to match the config.");
    }

    // Validate signal group indices are unique and sequential (0..n-1)
    std::set<std::string> sgIds;
    std::set<int> sgIndices;
    for (const auto& sg : config.signalGroups) {
        sgIds.insert(sg.id);
        if (sgIndices.find(sg.index) != sgIndices.end()) {
            std::cerr << "FATAL: Duplicate signal group index " << sg.index
                      << " for signal group '" << sg.id << "'" << std::endl;
            throw std::runtime_error("Duplicate signal group index: " + std::to_string(sg.index));
        }
        sgIndices.insert(sg.index);
    }
    // Check indices are sequential 0..n-1
    int expectedIndex = 0;
    for (int idx : sgIndices) {
        if (idx != expectedIndex) {
            std::cerr << "FATAL: Signal group indices not sequential - expected "
                      << expectedIndex << " but found " << idx << std::endl;
            throw std::runtime_error("Signal group indices not sequential: expected " +
                std::to_string(expectedIndex) + " but found " + std::to_string(idx));
        }
        ++expectedIndex;
    }

    // Validate detector indices are unique and sequential
    std::set<int> detIndices;
    for (const auto& det : config.detectors) {
        if (detIndices.find(det.index) != detIndices.end()) {
            std::cerr << "FATAL: Duplicate detector index " << det.index
                      << " for detector '" << det.id << "'" << std::endl;
            throw std::runtime_error("Duplicate detector index: " + std::to_string(det.index));
        }
        detIndices.insert(det.index);
    }
    expectedIndex = 0;
    for (int idx : detIndices) {
        if (idx != expectedIndex) {
            std::cerr << "FATAL: Detector indices not sequential - expected "
                      << expectedIndex << " but found " << idx << std::endl;
            throw std::runtime_error("Detector indices not sequential: expected " +
                std::to_string(expectedIndex) + " but found " + std::to_string(idx));
        }
        ++expectedIndex;
    }

    // Check that all conflict IDs reference valid signal groups
    for (const auto& conflict : config.conflicts) {
        if (sgIds.find(conflict.fromId) == sgIds.end()) {
            std::cerr << "FATAL: Conflict references unknown signal group '"
                      << conflict.fromId << "' (from)" << std::endl;
            throw std::runtime_error("Conflict matrix references unknown signal group: " + conflict.fromId);
        }
        if (sgIds.find(conflict.toId) == sgIds.end()) {
            std::cerr << "FATAL: Conflict references unknown signal group '"
                      << conflict.toId << "' (to)" << std::endl;
            throw std::runtime_error("Conflict matrix references unknown signal group: " + conflict.toId);
        }
    }

    // Check that conflict matrix is symmetrical (if A→B exists, B→A must also exist)
    std::set<std::pair<std::string, std::string>> conflictPairs;
    for (const auto& conflict : config.conflicts) {
        conflictPairs.insert({conflict.fromId, conflict.toId});
    }
    for (const auto& conflict : config.conflicts) {
        auto reverse = std::make_pair(conflict.toId, conflict.fromId);
        if (conflictPairs.find(reverse) == conflictPairs.end()) {
            std::cerr << "FATAL: Conflict matrix not symmetrical - found "
                      << conflict.fromId << "→" << conflict.toId
                      << " but missing " << conflict.toId << "→" << conflict.fromId << std::endl;
            throw std::runtime_error("Conflict matrix not symmetrical: missing " +
                conflict.toId + "→" + conflict.fromId);
        }
    }

    g_config = config;
    // Initialize intergreen timers for each conflict
    g_intergreenTimers.clear();
    g_intergreenTimers.resize(config.conflicts.size());
}

const IntersectionConfig& getIntersectionConfig() {
    return g_config;
}

} // namespace cif

// --- DSI processing for priority vehicles ---

namespace {

/**
 * @brief Process DSI buffer for priority vehicle announcements
 *
 * Called each cycle to check for new DSI data and update priority counts.
 * - Checkin (type=1): Increment priority count for matching direction
 * - Checkout (type=2): Decrement priority count for matching direction
 *
 * DSI buffer field indices:
 * - CIF_DSI_DIR (index 6): Direction number - maps to signal group
 * - CIF_DSI_TYPE (index 18): Announcement type (1=checkin, 2=checkout)
 */
void processDsiBuffer() {
    if (CIF_DSIWIJZ == 0) {
        return;  // No new DSI data
    }

    int type = CIF_DSI[CIF_DSI_TYPE];
    int dir = CIF_DSI[CIF_DSI_DIR];

    // Find signal group matching this direction
    for (const auto& sg : g_config.signalGroups) {
        if (sg.direction == dir && sg.index >= 0 && sg.index < CIF_AANT_US_FC) {
            auto& state = g_sgState[sg.index];
            if (type == 1) { // Checkin
                state.priorityVehicleCount++;
                state.hasPriorityRequest = true;
            } else if (type == 2) { // Checkout
                if (state.priorityVehicleCount > 0) {
                    state.priorityVehicleCount--;
                }
                if (state.priorityVehicleCount == 0) {
                    state.hasPriorityRequest = false;
                    state.waitingTime = 0;
                }
            }
            break;
        }
    }

    CIF_DSIWIJZ = 0;  // Clear the flag
}

} // anonymous namespace

// --- Mapping contract (see cif/CifController.h) ---

// Exposes this controller's object names so the container can auto-map TLC-FI
// objects to CIF indices. Resolves by the configured `index` field, which is
// the same index this controller uses internally into the CIF arrays. The
// container queries indices 0..CIF_PB_AANT_*-1 per type.
extern "C" const char* getCifItemName(CifObjectType type, int index) {
    auto byIndex = [](const auto& items, int idx) -> const char* {
        for (const auto& item : items) {
            if (item.index == idx) return item.id.c_str();
        }
        return nullptr;
    };
    switch (type) {
        case CIF_OBJ_SIGNALGROUP: return byIndex(g_config.signalGroups, index);
        case CIF_OBJ_DETECTOR:    return byIndex(g_config.detectors, index);
        case CIF_OBJ_INPUT:       return byIndex(g_config.inputs, index);
        case CIF_OBJ_OUTPUT:      return byIndex(g_config.outputs, index);
    }
    return nullptr;
}

// --- applicatieprogramma() implementation ---

extern "C" s_int16 applicatieprogramma(s_int16 init) {
    if (init == CIF_INIT) {
        // Initialize: all signal groups to red, clear internal state
        // Note: We do NOT set CIF_GUSWIJZ here because the TLC already has
        // the signal groups in StopAndRemain. We only signal changes when
        // the controller actually transitions a signal group during operation.
        for (int i = 0; i < CIF_AANT_US_FC; ++i) {
            CIF_GUS[i] = CIF_ROOD;
            g_sgState[i] = {};
        }
        for (int i = 0; i < CIF_AANT_IS_D; ++i) {
            CIF_IS[i] = 0;
        }
        for (int i = 0; i < CIF_AANT_IS_D + CIF_AANT_IS_OV; ++i) {
            CIF_IS_SWICO[i] = CIF_IS_SWICO_GEEN;
        }

        // Initialize standard UBER/IBER and MON rings for standalone/test use.
        CIF_IBER_LEES = CIF_IBER;
        CIF_IBER_SCHRIJF = CIF_IBER;
        CIF_UBER_LEES = CIF_UBER;
        CIF_UBER_SCHRIJF = CIF_UBER;
        CIF_MON_IBER_LEES = CIF_MON_IBER;
        CIF_MON_IBER_SCHRIJF = CIF_MON_IBER;
        CIF_MON_UBER_LEES = CIF_MON_UBER;
        CIF_MON_UBER_SCHRIJF = CIF_MON_UBER;

        // Initialize FILE_UBER ring and file-switch state.
        CIF_FILE_UBER_LEES = CIF_FILE_UBER;
        CIF_FILE_UBER_SCHRIJF = CIF_FILE_UBER;
        CIF_FILE_UBER_FILEWISSEL = nullptr;
        CIF_PB_FILE_UBER_FILEWISSEL = 0;
        CIF_FILE_UBER_STATUS = 1;
        CIF_FILE_UBER_FILEEXT[0] = 'v';
        CIF_FILE_UBER_FILEEXT[1] = 'l';
        CIF_FILE_UBER_FILEEXT[2] = 'g';
        CIF_FILE_UBER_FILEEXT[3] = '\0';

        // Don't set CIF_GUSWIJZ - TLC already has these states
        return CIF_GEEN_FOUT;
    }

    // Normal cycle

    // --- MON logging to logging server: we write dummy messages every second ---
    {
        static int count = 0;
        if (count++ % 10 == 0) {
            std::string msg = "Time: " + std::to_string(CIF_KLOK[0]) + "-" + std::to_string(CIF_KLOK[1]) + "-" + std::to_string(CIF_KLOK[2]) + " " + std::to_string(CIF_KLOK[4]) + ":" + std::to_string(CIF_KLOK[5]) + ":" + std::to_string(CIF_KLOK[6]) + "\r\n";
            s_int8* monStart = CIF_MON_UBER;
            s_int8* monEnd = CIF_MON_UBER + CIF_PB_MAX_MON_UBER;
            s_int8* writePtr = CIF_MON_UBER_SCHRIJF;
            // Only write if the whole line fits; otherwise drop it rather than
            // lapping the read pointer and corrupting unread data.
            if (ringFreeSpace(CIF_MON_UBER_LEES, writePtr,
                              static_cast<std::size_t>(CIF_PB_MAX_MON_UBER)) >= msg.size()) {
                for (char c : msg) {
                    *writePtr = static_cast<s_int8>(c);
                    ++writePtr;
                    if (writePtr >= monEnd) writePtr = monStart;
                }
                CIF_MON_UBER_SCHRIJF = writePtr;
            }
            // else: MON_UBER ring full (no consumer draining it) — drop line.
        }
    }

    // --- FILE logging: write one timestamp line per second ---
    {
        static int fileCount = 0;
        if (fileCount++ % 10 == 0) {
            bool pbRequestedSwitch = (CIF_PB_FILE_UBER_FILEWISSEL != 0);
            std::string msgPrefix = pbRequestedSwitch ? "SWITCH " : "TIME ";
            std::string msg = msgPrefix
                + std::to_string(CIF_KLOK[0]) + "-"
                + std::to_string(CIF_KLOK[1]) + "-"
                + std::to_string(CIF_KLOK[2]) + " "
                + std::to_string(CIF_KLOK[4]) + ":"
                + std::to_string(CIF_KLOK[5]) + ":"
                + std::to_string(CIF_KLOK[6]) + "\r\n";

            if (writeFileUberLine(msg, pbRequestedSwitch) && pbRequestedSwitch) {
                CIF_PB_FILE_UBER_FILEWISSEL = 0;
            }
        }
    }

    // --- IBER → UBER echo: read incoming messages and respond with "OK <message>" ---
    {
        s_int8* iberStart = CIF_IBER;
        s_int8* iberEnd = CIF_IBER + CIF_PB_MAX_IBER;
        s_int8* readPtr = CIF_IBER_LEES;

        while (readPtr != CIF_IBER_SCHRIJF) {
            // Read bytes until \r terminator or catching up to write pointer
            std::string msg;
            bool foundTerminator = false;
            while (readPtr != CIF_IBER_SCHRIJF) {
                char c = static_cast<char>(*readPtr);
                ++readPtr;
                if (readPtr >= iberEnd) readPtr = iberStart;

                if (c == '\r') {
                    foundTerminator = true;
                    break;
                }
                msg += c;
            }

            if (foundTerminator && !msg.empty()) {
                // Write "OK " + message + "\r\n" into UBER
                std::string response = "OK " + msg + "\r\n";
                s_int8* uberStart = CIF_UBER;
                s_int8* uberEnd = CIF_UBER + CIF_PB_MAX_UBER;
                s_int8* writePtr = CIF_UBER_SCHRIJF;

                // Only echo if the whole response fits; otherwise drop it
                // rather than lapping the read pointer and corrupting data.
                if (ringFreeSpace(CIF_UBER_LEES, writePtr,
                                  static_cast<std::size_t>(CIF_PB_MAX_UBER)) >= response.size()) {
                    for (char c : response) {
                        *writePtr = static_cast<s_int8>(c);
                        ++writePtr;
                        if (writePtr >= uberEnd) writePtr = uberStart;
                    }
                    CIF_UBER_SCHRIJF = writePtr;
                }
                // else: UBER ring full — drop this echo response.
            }
        }
        CIF_IBER_LEES = readPtr;
    }

    bool changed = false;

    // Tick all intergreen timers at the start of each cycle
    tickIntergreenTimers();

    // Process DSI buffer for priority vehicle announcements
    processDsiBuffer();

    // Create a list of signal groups sorted by priority and waiting time
    std::vector<const cif::SignalGroupConfig*> sortedGroups;
    for (const auto& sg : g_config.signalGroups) {
        sortedGroups.push_back(&sg);
    }

    // Sort: priority first, then by waiting time
    std::sort(sortedGroups.begin(), sortedGroups.end(),
        [](const auto* a, const auto* b) {
            const auto& stateA = g_sgState[a->index];
            const auto& stateB = g_sgState[b->index];

            // Priority groups first
            if (stateA.hasPriorityRequest != stateB.hasPriorityRequest) {
                return stateA.hasPriorityRequest;
            }
            // Then by waiting time (longest wait first)
            return stateA.waitingTime > stateB.waitingTime;
        });

    for (const auto* sg : sortedGroups) {
        if (sg->index < 0 || sg->index >= CIF_AANT_US_FC) continue;

        auto& state = g_sgState[sg->index];
        bool detOccupied = anyDetectorOccupied(*sg);

        state.timer++;

        // Track waiting time for signal groups with requests
        if (state.request && state.phase == Phase::Red) {
            state.waitingTime++;
        }

        switch (state.phase) {
            case Phase::Red:
                // Set request if detector is occupied
                if (detOccupied) {
                    state.request = true;
                }

                // Transition to green if:
                // - There is a request
                // - No conflicts block it (intergreen timers, conflicting green)
                // - Fairness allows it (hasn't been served while others are waiting)
                if (state.request && state.timer >= sg->minRed && canBecomeGreen(sg->id) && fairnessAllowsGreen(sg->index)) {
                    state.phase = Phase::Green;
                    state.timer = 0;
                    state.waitingTime = 0;  // Reset waiting time when going green
                    CIF_GUS[sg->index] = CIF_GROEN;
                    changed = true;
                }
                break;

            case Phase::Green: {
                // Determine effective max green based on priority
                int effectiveMaxGreen = sg->maxGreen;
                if (state.priorityVehicleCount > 0) {
                    effectiveMaxGreen = std::max(effectiveMaxGreen, sg->priorityMaxGreen);
                }

                bool extend = detOccupied && (state.timer < effectiveMaxGreen);

                if (state.timer >= effectiveMaxGreen) {
                    // Max green reached: force amber
                    state.phase = Phase::Amber;
                    state.timer = 0;
                    state.servedThisCycle = true;  // Mark as served for fairness
                    CIF_GUS[sg->index] = CIF_GEEL;
                    startIntergreenTimers(sg->id);  // Start intergreen timers
                    changed = true;
                } else if (state.timer >= sg->minGreen && !extend) {
                    // Min green reached and no extension: go amber
                    state.phase = Phase::Amber;
                    state.timer = 0;
                    state.servedThisCycle = true;  // Mark as served for fairness
                    CIF_GUS[sg->index] = CIF_GEEL;
                    startIntergreenTimers(sg->id);  // Start intergreen timers
                    changed = true;
                }
                break;
            }

            case Phase::Amber:
                if (state.timer >= sg->amber) {
                    // Amber time elapsed: go red
                    state.phase = Phase::Red;
                    state.timer = 0;
                    state.request = false;
                    CIF_GUS[sg->index] = CIF_ROOD;
                    changed = true;
                }
                break;
        }
    }

    // Reset served flags when all requesting signal groups have been served
    // This starts a new fairness cycle
    if (!hasUnservedRequests()) {
        resetServedFlags();
    }

    if (changed) {
        CIF_GUSWIJZ = CIF_GESCHREVEN;
    }

    return CIF_GEEN_FOUT;
}
