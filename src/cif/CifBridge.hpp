#pragma once

#include "fi/FiControllerApp.hpp"
#include "fi/LogLevel.hpp"

#include "cif/CIF_INC.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace cif {

/**
 * @brief Set of controller-side item names that the operator has declared as
 *        not present in the TLC.
 *
 * Items listed here are skipped when the bridge builds its id<->index
 * mappings from the controller. As a result:
 *   - They never enter the bridge's maps, so validateMappings() will not
 *     flag them as stale (i.e. "controller has X, TLC does not").
 *   - The legacy controller's CIF_GUS[] writes for these slots are dropped
 *     silently (the existing empty-string guard in processDesiredOutputChanges
 *     handles this).
 *   - For inputs/detectors, TLC will never send updates for them, so their
 *     CIF_IS[] slots stay at their initial values.
 *
 * Use this only for genuinely controller-only items (test outputs, legacy
 * inputs that exist in the controller config but are not wired into the
 * intersection). Anything else should be a TLC configuration fix.
 */
struct IgnoredItems {
    std::set<std::string> signalGroups;
    std::set<std::string> outputs;
    std::set<std::string> detectors;
    std::set<std::string> inputs;
};

/**
 * @brief One explicit TLC-FI ↔ CIF name couple.
 *
 * By default the bridge couples objects by exact name identity (a controller
 * item named @c "SG01" is coupled to the TLC-FI object @c "SG01"). A manual
 * couple overrides that for a single item whose names differ on the two sides:
 * the controller slot named @ref cifName is coupled to the TLC-FI object named
 * @ref tlcfiName.
 */
struct ManualCouple {
    std::string tlcfiName;  ///< TLC-FI object id (must exist in the TLC meta).
    std::string cifName;    ///< Controller item name (must exist via getCifItemName).
};

/**
 * @brief Explicit TLC-FI ↔ CIF name couples, grouped by object type.
 *
 * Populated from the tlcfi.json "manuallyCoupledItems" array and handed to the
 * bridge via @ref CifBridge::setManualCouples before mappings are built. Manual
 * couples take precedence over automatic (identity) matching, and both of their
 * names must be found on their respective side or startup aborts (see
 * @ref CifBridge::getUnmatchedManualCouples for the CIF side, and the existing
 * stale check for the TLC side).
 */
struct ManualCouples {
    std::vector<ManualCouple> signalGroups;
    std::vector<ManualCouple> outputs;
    std::vector<ManualCouple> detectors;
    std::vector<ManualCouple> inputs;
};

/**
 * @brief Bridge between TLC-FI protocol and legacy CVN-C interface
 * 
 * This class translates between the modern TLC-FI protocol (using object-oriented
 * structures with string identifiers) and the legacy CVN-C interface (using static
 * C arrays with integer indices as defined in CIF_INC.h).
 * 
 * The bridge performs bidirectional translation:
 * - **TLC-FI → CIF**: When TLC-FI object updates are received, they are written
 *   to the appropriate CIF arrays (CIF_WUS[], CIF_IS[]) and change flags are set.
 * - **CIF → TLC-FI**: When the legacy controller writes desired states to CIF_GUS[],
 *   the bridge reads them and sends UpdateState notifications via FiControllerApp.
 * 
 * The bridge maintains ID-to-index mappings to translate between TLC-FI object IDs
 * (strings like "SG01", "OUT1") and CIF array indices. These mappings must be
 * populated after meta data is read from the Facilities.
 * 
 * @note The CIF arrays are shared memory between the bridge and the legacy C controller.
 *       The legacy controller reads from CIF_WUS[]/CIF_IS[] and writes to CIF_GUS[].
 * 
 * @see CIF_INC.h (CVN-C interface specification)
 */
class CifBridge {
public:
    /**
     * @brief Constructs a bridge for the given FiControllerApp.
     * 
     * @param app Reference to the FiControllerApp for sending state updates.
     */
    explicit CifBridge(tlcfi::fi::FiControllerApp& app);

    /**
     * @brief Starts the bridge and registers update handlers.
     *
     * Registers callbacks on FiControllerApp to receive object state updates.
     * After calling start(), the bridge will automatically translate incoming
     * TLC-FI updates to CIF arrays.
     */
    void start();

    /**
     * @brief Returns the mutex serialising access to the shared CIF arrays.
     *
     * The control loop must hold this mutex while the legacy controller
     * (applicatieprogramma) runs, so the TLC-FI update handlers — which run on
     * the protocol thread and write CIF_WUS[]/CIF_IS[] — cannot mutate those
     * arrays mid-cycle. The message server locks the same mutex around its
     * CIF_IBER/CIF_UBER ring access.
     */
    std::mutex& cifMutex() { return cifMutex_; }

    /**
     * @brief Processes desired output changes from the legacy controller.
     * 
     * Polls the CIF_GUS[] array (desired signal group/output states written by
     * the legacy controller) and sends UpdateState notifications via FiControllerApp
     * for any changed states. Resets CIF_GUSWIJZ flag after processing.
     * 
     * This method should be called periodically (e.g., every control loop iteration)
     * to forward the legacy controller's desired state changes to the Facilities.
     * 
     * @note Only processes changes if CIF_GUSWIJZ flag is set, indicating the
     *       legacy controller has written new desired states.
     */
    void processDesiredOutputChanges();

    /**
     * @brief ID-to-index translation methods
     * 
     * These methods translate TLC-FI object IDs (strings) to CIF array indices.
     * Returns -1 if the ID is not found in the mapping.
     */
    
    /// @brief Translates a signal group ID to its CIF_GUS[] index.
    int translateSignalGroupIdToIndex(const std::string& id) const;
    
    /// @brief Translates an output ID to its CIF_GUS[] index.
    /// @note Output indices are offset by the number of signal groups.
    int translateOutputIdToIndex(const std::string& id) const;
    
    /// @brief Translates a detector ID to its CIF_IS[] index.
    int translateDetectorIdToIndex(const std::string& id) const;
    
    /// @brief Translates an input ID to its CIF_IS[] index.
    /// @note Input indices are offset by the number of detectors.
    int translateInputIdToIndex(const std::string& id) const;

    /**
     * @brief Index-to-ID translation methods
     * 
     * These methods translate CIF array indices back to TLC-FI object IDs.
     * Returns empty string if index is out of range or not mapped.
     */
    
    /// @brief Translates a CIF_GUS[] index to signal group ID.
    std::string translateIndexToSignalGroupId(int index) const;
    
    /// @brief Translates a CIF_GUS[] index to output ID.
    std::string translateIndexToOutputId(int index) const;

    /**
     * @brief Mapping configuration methods
     * 
     * These methods populate the ID-to-index mappings. Should be called after
     * meta data is read from the Facilities, using the discovered object IDs
     * and assigning them to CIF array indices.
     */
    
    /// @brief Sets the mapping for a signal group ID to a CIF_GUS[] index.
    void setSignalGroupMapping(std::string signalGroupId, int index);
    
    /// @brief Sets the mapping for an output ID to a CIF_GUS[] index.
    void setOutputMapping(std::string outputId, int index);
    
    /// @brief Sets the mapping for a detector ID to a CIF_IS[] index.
    void setDetectorMapping(std::string detectorId, int index);
    
    /// @brief Sets the mapping for an input ID to a CIF_IS[] index.
    void setInputMapping(std::string inputId, int index);

    /**
     * @brief Declares controller-side items that are absent in the TLC.
     *
     * Must be called BEFORE buildMappingsFromController() (i.e. before
     * CifHost::init()), otherwise the items will already be in the maps and
     * filtering has no effect.
     *
     * @param items Set of controller-only names, grouped by object type.
     */
    void setIgnoredItems(IgnoredItems items);

    /**
     * @brief Returns ignore-list entries that never matched a controller item.
     *
     * Useful as a typo guard: an entry listed in @ref setIgnoredItems but
     * never seen during mapping building probably indicates a misspelled name.
     */
    void getUnmatchedIgnoredItems(
        std::vector<std::string>& signalGroups,
        std::vector<std::string>& outputs,
        std::vector<std::string>& detectors,
        std::vector<std::string>& inputs) const;

    /**
     * @brief Declares explicit TLC-FI ↔ CIF name couples.
     *
     * Must be called BEFORE @ref buildMappings() (i.e. before CifHost::init()).
     * During mapping building each listed CIF item is coupled under its
     * @c tlcfiName instead of its @c cifName, and a manual couple takes
     * precedence over an automatic (identity) match regardless of the order the
     * controller reports its items in.
     *
     * @param couples Manual couples, grouped by object type.
     */
    void setManualCouples(ManualCouples couples);

    /**
     * @brief Builds the id↔index mappings from the controller's scanned items.
     *
     * Each vector holds the @c (cifName, index) pairs the host read from the
     * controller via getCifItemName(), one vector per object type. This is the
     * single ingestion entry point used by CifHost. It applies manual couples
     * first (so they win over identity matches), then automatic matches, while
     * skipping @ref setIgnoredItems entries. Controller items that end up
     * coupled to nothing (a name shadowed by a manual couple target, or a
     * duplicate controller name) are collected for @ref getUnmappedControllerItems.
     *
     * @param signalGroups (cifName, index) pairs for signal groups.
     * @param outputs      (cifName, index) pairs for outputs.
     * @param detectors    (cifName, index) pairs for detectors.
     * @param inputs       (cifName, index) pairs for inputs.
     */
    void buildMappings(
        const std::vector<std::pair<std::string, int>>& signalGroups,
        const std::vector<std::pair<std::string, int>>& outputs,
        const std::vector<std::pair<std::string, int>>& detectors,
        const std::vector<std::pair<std::string, int>>& inputs);

    /**
     * @brief Returns manual couples whose @c cifName was not found on the controller.
     *
     * A manual couple must match a real controller item; an unmatched @c cifName
     * is a fatal configuration error (the CIF side of the couple is missing).
     * Only meaningful after @ref buildMappings has run.
     */
    void getUnmatchedManualCouples(
        std::vector<ManualCouple>& signalGroups,
        std::vector<ManualCouple>& outputs,
        std::vector<ManualCouple>& detectors,
        std::vector<ManualCouple>& inputs) const;

    /**
     * @brief Returns controller items that ended up coupled to nothing.
     *
     * After @ref buildMappings, every controller item should be coupled exactly
     * once — automatically, via a manual couple, or explicitly excluded through
     * @ref setIgnoredItems. Any leftover (a name shadowed by a manual couple's
     * @c tlcfiName target, or a duplicate controller name) is reported here and
     * is a fatal configuration error. This complements the stale check in
     * @ref getStaleObjects, which can only see items that made it into the map.
     */
    void getUnmappedControllerItems(
        std::vector<std::string>& signalGroups,
        std::vector<std::string>& outputs,
        std::vector<std::string>& detectors,
        std::vector<std::string>& inputs) const;

    /**
     * @brief Sets the count of signal groups (used as offset for outputs in CIF_GUS[]).
     * @param count Number of signal groups configured
     */
    void setSignalGroupCount(int count);

    /**
     * @brief Sets the count of detectors (used as offset for inputs in CIF_IS[]).
     * @param count Number of detectors configured
     */
    void setDetectorCount(int count);

    /**
     * @brief Validates that all TLC objects are mapped.
     *
     * Checks that every signal group, output, detector, and input from the TLC
     * has a corresponding mapping in this bridge. This ensures complete coverage
     * of all objects in the controlled intersection.
     *
     * @return true if all objects are mapped, false if any are missing.
     */
    bool validateMappings() const;

    /**
     * @brief Gets lists of unmapped object IDs.
     *
     * Returns the IDs of TLC objects that don't have mappings in this bridge.
     * Useful for error reporting.
     *
     * @param unmappedSignalGroups Output: signal group IDs without mappings
     * @param unmappedOutputs Output: output IDs without mappings
     * @param unmappedDetectors Output: detector IDs without mappings
     * @param unmappedInputs Output: input IDs without mappings
     */
    void getUnmappedObjects(
        std::vector<std::string>& unmappedSignalGroups,
        std::vector<std::string>& unmappedOutputs,
        std::vector<std::string>& unmappedDetectors,
        std::vector<std::string>& unmappedInputs) const;

    /**
     * @brief Handles a SpecialVehicleEvent by queuing it for DSI delivery.
     *
     * Called from the onSpecialVehicleEvent callback. Thread-safe.
     *
     * @param evt The special vehicle event to queue.
     */
    void handleSpecialVehicleEvent(const ::tlcfi::tlcfi::SpecialVehicleEvent& evt);

    /**
     * @brief Prepares the CIF_DSI[] buffer for the next applicatieprogramma() call.
     *
     * Pops one event from the DSI queue (if any), writes its fields to CIF_DSI[],
     * and sets CIF_DSIWIJZ accordingly. Should be called before each
     * applicatieprogramma() call.
     */
    void prepareDsiBuffer();

    /**
     * @brief Synchronizes detector and input states from TLC to CIF_IS[].
     *
     * Reads the current state of all detectors and inputs from the TLC and
     * writes them to CIF_IS[]. This should be called when starting the
     * controller loop to ensure the CIF_IS[] buffer reflects the actual
     * state of detectors/inputs before the first applicatieprogramma() call.
     */
    void synchronizeInputStates();

    /**
     * @brief Resets runtime bridge state for reconnection.
     *
     * Clears pending requests. ID-to-index mappings persist since they
     * are configuration set once at startup. Use validateMappings() after
     * reconnection to verify the TLC still matches the configured mappings.
     */
    void reset();

    /**
     * @brief Configures optional file-based logging from CIF_FILE_UBER.
     *
     * When enabled, pollFileVlog() drains CIF_FILE_UBER ring data to rotating
     * files in outputDir with name pattern <SYSTEM>_yyyyMMdd_HHmmss.vlg.
     *
     * @param enabled   Enables/disables file logging.
     * @param outputDir Directory for log files when enabled.
     * @param systemName Controller/system name used in filenames.
     * @param maxFiles  Maximum number of .vlg files to keep (0 = unlimited).
     *                  The oldest files are removed after each file is finalized.
     */
    void configureFileLogging(bool enabled, std::string outputDir, std::string systemName,
                              int maxFiles = 0);

    /**
     * @brief Polls CIF_FILE_UBER and writes buffered bytes to file sink.
     *
     * Also handles file rotation requests/signals using
     * CIF_PB_FILE_UBER_FILEWISSEL and CIF_FILE_UBER_FILEWISSEL.
     * Intended to be called once per control loop iteration.
     */
    void pollFileVlog();

    /**
     * @brief Gets lists of configured objects not present in the TLC.
     *
     * Returns the IDs of objects that have mappings in this bridge but
     * are not present in the TLC's meta data. This detects stale mappings
     * after a TLC upgrade/reconfiguration.
     *
     * @param staleSignalGroups Output: mapped signal group IDs not in TLC
     * @param staleOutputs Output: mapped output IDs not in TLC
     * @param staleDetectors Output: mapped detector IDs not in TLC
     * @param staleInputs Output: mapped input IDs not in TLC
     */
    void getStaleObjects(
        std::vector<std::string>& staleSignalGroups,
        std::vector<std::string>& staleOutputs,
        std::vector<std::string>& staleDetectors,
        std::vector<std::string>& staleInputs) const;

    /**
     * @brief Sanitizes a system name for use in filesystem-safe filenames.
     *
     * Keeps alphanumerics, '_' and '-'; drops everything else. Returns
     * "SYSTEM" if the result is empty. Exposed so other components (e.g.
     * the CIF_PARM1 persistence file) derive the same name as the file
     * VLOG writer from the configured system name.
     */
    static std::string sanitizeSystemName(const std::string& name);

    /// @brief Logging callback — invoked for all log messages from this component.
    ::tlcfi::fi::LogCallback onLog;

private:
    void log(::tlcfi::fi::LogLevel level, const std::string& message);
    /**
     * @brief TLC-FI update handlers
     * 
     * These methods are called when TLC-FI object updates are received.
     * They translate the updates to CIF array values and set change flags.
     */
    
    /// @brief Handles SignalGroup state update from TLC-FI.
    /// Writes state to CIF_WUS[] and sets CIF_WUSWIJZ flag.
    void handleSignalGroupUpdate(const ::tlcfi::tlcfi::SignalGroup& sg);
    
    /// @brief Handles Output state update from TLC-FI.
    /// Writes state to CIF_WUS[] and sets CIF_WUSWIJZ flag.
    void handleOutputUpdate(const ::tlcfi::tlcfi::Output& o);
    
    /// @brief Handles Detector state update from TLC-FI.
    /// Writes state to CIF_IS[] and sets CIF_ISWIJZ flag.
    void handleDetectorUpdate(const ::tlcfi::tlcfi::Detector& d);
    
    /// @brief Handles Input state update from TLC-FI.
    /// Writes state to CIF_IS[] and sets CIF_ISWIJZ flag.
    void handleInputUpdate(const ::tlcfi::tlcfi::Input& i);

private:
    /**
     * @brief Ensures file stream is open if there is data to write.
     */
    void ensureFileLogStreamOpen();

    /**
     * @brief Requests a file switch when the 5-minute interval elapses.
     */
    void requestFileSwitchIfDue(std::chrono::system_clock::time_point now);

    /**
     * @brief Rotates file at current read position boundary.
     */
    void rotateFile(std::chrono::system_clock::time_point now);

    /**
     * @brief Finalizes currently open temporary file by renaming to .vlg.
     */
    void finalizeActiveFile();

    /**
     * @brief Removes the oldest .vlg files so at most fileLogMaxFiles_ remain.
     *
     * Only called when fileLogMaxFiles_ > 0. Lists all <SYSTEM>_*.vlg files in
     * fileLogOutputDir_, sorts them by name (ascending = oldest first, because
     * filenames embed a timestamp), and removes the leading entries.
     */
    void pruneOldVlogFiles();

    /**
     * @brief Builds output filename stem from system + timestamp.
     */
    std::string buildFileStem(std::chrono::system_clock::time_point now) const;

private:
    tlcfi::fi::FiControllerApp& app_;                                    ///< Reference to FiControllerApp for sending updates.
	mutable std::mutex mapMutex_;                             ///< Mutex protecting ID-to-index mappings.
	std::mutex cifMutex_;                                     ///< Serialises shared CIF array (CIF_WUS/CIF_IS/flags) and pending-map access across threads.
	
	/**
	 * @brief ID-to-index mappings
	 * 
	 * Maps TLC-FI object IDs (strings) to CIF array indices.
	 */
	std::map<std::string, int> signalGroupIdToIndex_;         ///< Signal group ID → CIF_GUS[] index.
	std::map<std::string, int> outputIdToIndex_;              ///< Output ID → CIF_GUS[] index (offset by signal groups).
	std::map<std::string, int> detectorIdToIndex_;            ///< Detector ID → CIF_IS[] index.
	std::map<std::string, int> inputIdToIndex_;                ///< Input ID → CIF_IS[] index (offset by detectors).

	/**
	 * @brief Index-to-ID reverse mappings
	 *
	 * Maps type-local indices back to TLC-FI object IDs for efficient
	 * lookup when processing CIF_GUS[] changes.
	 */
	std::vector<std::string> indexToSignalGroupId_;            ///< Type-local index → signal group ID.
	std::vector<std::string> indexToOutputId_;                ///< Type-local index → output ID.

	/**
	 * @brief Counts used for array offset calculation
	 *
	 * In CVN-C, signal groups and outputs share CIF_GUS[] (signal groups first),
	 * and detectors and inputs share CIF_IS[] (detectors first).
	 */
	int signalGroupCount_ = 0;   ///< Number of signal groups (offset for outputs in CIF_GUS[])
	int detectorCount_ = 0;      ///< Number of detectors (offset for inputs in CIF_IS[])

	/**
	 * @brief Pending requested states for signal groups and outputs
	 *
	 * Tracks states we have requested but not yet received acknowledgment for.
	 * Prevents sending duplicate requests before TLC acknowledges.
	 * Cleared when handleSignalGroupUpdate receives a state update.
	 */
	std::map<std::string, ::tlcfi::tlcfi::SignalGroupState> pendingSgRequests_;
    std::map<std::string, int> pendingOutputsRequests_;

    /**
     * @brief Controller-side items the operator has declared absent in the TLC.
     *
     * See @ref setIgnoredItems. Names listed here are skipped during mapping
     * building. The corresponding @c matchedIgnored* sets record which names
     * actually matched a controller item, so unmatched entries (likely typos)
     * can be reported back to the operator.
     */
    IgnoredItems ignoredItems_;
    std::set<std::string> matchedIgnoredSignalGroups_;
    std::set<std::string> matchedIgnoredOutputs_;
    std::set<std::string> matchedIgnoredDetectors_;
    std::set<std::string> matchedIgnoredInputs_;

    /**
     * @brief Explicit TLC-FI ↔ CIF name couples the operator declared.
     *
     * See @ref setManualCouples. For each type the bridge keeps a
     * @c cifName→tlcfiName lookup and the set of @c tlcfiName targets (used to
     * detect controller names shadowed by a couple). @c matchedManual* records
     * which @c cifName values actually matched a controller item, and
     * @c unmappedControllerItems* collects the leftovers found during
     * @ref buildMappings.
     */
    ManualCouples manualCouples_;
    std::map<std::string, std::string> manualSignalGroupCifToTlcfi_;
    std::map<std::string, std::string> manualOutputCifToTlcfi_;
    std::map<std::string, std::string> manualDetectorCifToTlcfi_;
    std::map<std::string, std::string> manualInputCifToTlcfi_;
    std::set<std::string> manualSignalGroupTlcfiKeys_;
    std::set<std::string> manualOutputTlcfiKeys_;
    std::set<std::string> manualDetectorTlcfiKeys_;
    std::set<std::string> manualInputTlcfiKeys_;
    std::set<std::string> matchedManualSignalGroups_;
    std::set<std::string> matchedManualOutputs_;
    std::set<std::string> matchedManualDetectors_;
    std::set<std::string> matchedManualInputs_;
    std::vector<std::string> unmappedControllerSignalGroups_;
    std::vector<std::string> unmappedControllerOutputs_;
    std::vector<std::string> unmappedControllerDetectors_;
    std::vector<std::string> unmappedControllerInputs_;

    /**
     * @brief Raw id→index inserters (no controller-only filtering).
     *
     * These perform the actual map/reverse-index insertion with the
     * already-exists guard, but — unlike the public setXMapping methods — do
     * NOT consult @ref ignoredItems_. @ref ingestType uses them for the manual
     * couple pass, where the map key is a TLC-FI name that must never be
     * filtered against the controller-only (CIF-name) set even when the two
     * strings happen to coincide. The public setXMapping methods delegate here
     * after their ignore check.
     */
    void insertSignalGroupMapping(std::string signalGroupId, int index);
    void insertOutputMapping(std::string outputId, int index);
    void insertDetectorMapping(std::string detectorId, int index);
    void insertInputMapping(std::string inputId, int index);

    /**
     * @brief Ingests one object type's scanned items (manual-first, then auto).
     *
     * Shared implementation behind @ref buildMappings. @p rawInsert is the
     * type-specific raw inserter; ignore filtering and match/orphan bookkeeping
     * are handled here so the manual pass is never blocked by the CIF-name
     * controller-only set.
     */
    void ingestType(
        const std::vector<std::pair<std::string, int>>& scanned,
        const std::map<std::string, std::string>& manualCifToTlcfi,
        const std::set<std::string>& manualTlcfiKeys,
        const std::set<std::string>& ignored,
        const char* typeLabel,
        std::set<std::string>& matchedManual,
        std::set<std::string>& matchedIgnored,
        std::vector<std::string>& unmappedControllerItems,
        void (CifBridge::*rawInsert)(std::string, int));

	/**
	 * @brief DSI event queue for SpecialVehicleEvents
	 *
	 * Events are queued as they arrive and delivered one per control cycle
	 * to the CIF_DSI[] buffer.
	 */
	std::queue<::tlcfi::tlcfi::SpecialVehicleEvent> dsiQueue_;
	std::mutex dsiMutex_;

    bool fileLoggingEnabled_ = false;
    std::filesystem::path fileLogOutputDir_;
    std::string fileLogSystemName_ = "SYSTEM";
    int fileLogMaxFiles_ = 0;
    std::ofstream fileLogStream_;
    std::filesystem::path activeTempFilePath_;
    std::filesystem::path activeFinalFilePath_;
    std::chrono::system_clock::time_point nextRotationRequestAt_{};
    bool fileSwitchRequested_ = false;
};

} // namespace cif
