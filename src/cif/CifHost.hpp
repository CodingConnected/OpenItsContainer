#pragma once

#include "fi/FiControllerApp.hpp"
#include "fi/LogLevel.hpp"
#include "cif/CifBridge.hpp"
#include "cif/CifMessageServerConfig.hpp"
#include "cif/Param1Store.hpp"

#include "cif/CIF_INC.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>

namespace cif {

class CifMessageServer;

/**
 * @brief Host for legacy CVN-C controller applications
 * 
 * This class provides a container/runtime environment for legacy C controller
 * applications that use the CVN-C interface (CIF_INC.h). It manages the control
 * loop that periodically calls the legacy controller's `applicatieprogramma()`
 * function and synchronizes state between the CIF arrays and TLC-FI protocol.
 * 
 * The host performs the following functions:
 * - **Clock synchronization**: Updates CIF_KLOK[] from system time each iteration
 * - **Control loop execution**: Calls `applicatieprogramma(0)` at 100ms intervals
 * - **State synchronization**: Processes desired output changes via CifBridge
 * - **Lifecycle management**: Initializes the controller and starts/stops the loop
 * 
 * The control loop is started automatically when the application receives control
 * (controlState = InControl) via the onControlInControl hook. The loop runs in
 * a separate thread to avoid blocking the TLC-FI protocol handling.
 * 
 * @note The legacy controller must implement the `applicatieprogramma()` function
 *       as defined in CIF_INC.h. The controller reads from CIF_WUS[]/CIF_IS[] and
 *       writes desired states to CIF_GUS[].
 * 
 * @see CIF_INC.h (CVN-C interface specification)
 * @see CifBridge (state translation layer)
 */
class CifHost {
public:
    /**
     * @brief Constructs a host for the given application and bridge.
     * 
     * @param app Reference to FiControllerApp for protocol communication.
     * @param bridge Reference to CifBridge for state translation.
     */
	explicit CifHost(tlcfi::fi::FiControllerApp& app, CifBridge& bridge);
    
    /**
     * @brief Destructor that stops the control loop and cleans up.
     */
    ~CifHost();

    /**
     * @brief Initializes the host and installs control state hooks.
     *
     * Starts the CifBridge, calls `applicatieprogramma(1)` to initialize the
     * legacy controller (CIF_INIT), and installs a hook to automatically start
     * the control loop when control is granted (InControl state).
     *
     * @note Should be called after FiControllerApp registration and meta read.
     */
    void init();

    /**
     * @brief Validates that all TLC objects are mapped in the bridge.
     *
     * Checks that every signal group, output, detector, and input discovered
     * from the TLC has a corresponding mapping in the CifBridge. This should
     * be called after the app is fully initialized (meta read + subscribed)
     * but before taking control.
     *
     * @return true if all objects are mapped, false if any are missing.
     *
     * @note On failure, call getValidationErrors() to get details.
     */
    bool validateMappings() const;

    /**
     * @brief Gets detailed validation error message.
     *
     * Returns a human-readable string listing all unmapped TLC objects.
     * Empty string if validation passed.
     *
     * @return Error message listing unmapped objects, or empty if valid.
     */
    std::string getValidationErrors() const;

    /**
     * @brief Control loop management
     * 
     * These methods start and stop the control loop that runs the legacy controller.
     * The loop is typically started automatically when control is granted, but can
     * also be started/stopped manually if needed.
     */
    
    /**
     * @brief Starts the control loop in a background thread.
     * 
     * Begins the periodic execution of `applicatieprogramma(0)` at 100ms intervals.
     * Safe to call multiple times (idempotent).
     */
    void startLoop();
    
    /**
     * @brief Stops the control loop and waits for thread completion.
     *
     * Signals the loop to stop and joins the thread. Called automatically by destructor.
     */
    void stopLoop();

    /**
     * @brief Enable a TCP message server for CIF IBER/UBER ring buffers.
     *
     * When set to a non-zero port, a CifMessageServer will be created during
     * init() to expose the IBER/UBER message interface over TCP. If port is 0
     * (default), no message server is created and CifHost behaves exactly as before.
     *
     * Buffer sizes are taken from the runtime constants @c CIF_PB_MAX_IBER
     * and @c CIF_PB_MAX_UBER (defined by whichever controller is linked in,
     * via CIF.INC). Hardcoding mismatched sizes here against the linked
     * controller's actual ring causes pollDataOut to wrap the read pointer
     * at the wrong byte and spin indefinitely.
     *
     * @param port TCP port to listen on, or 0 to disable.
     * @param bindAddress IP address to bind (default loopback). Exposing a
     *        non-loopback address allows remote CCOL clients; use only on a
     *        trusted network.
     */
    void setMessageServerConfig(unsigned short port,
                                const std::string& bindAddress = "127.0.0.1",
                                bool echoInput = false,
                                const std::string& welcomeMessage = {});

    /**
     * @brief Set the configuration for the logging server.
     *
     * Buffer sizes are taken from @c CIF_PB_MAX_MON_IBER /
     * @c CIF_PB_MAX_MON_UBER for the same reason as @ref setMessageServerConfig.
     *
     * @param port TCP port to listen on, or 0 to disable.
     * @param bindAddress IP address to bind (default loopback).
     */
    void setLoggingServerConfig(unsigned short port,
                              const std::string& bindAddress = "127.0.0.1");

    /**
     * @brief Declares controller-side items the operator has marked as absent
     *        in the TLC.
     *
     * Forwarded to the bridge. MUST be called before @ref init() — once init()
     * runs, the mapping table is already built and filtering no longer applies.
     *
     * @param items Set of controller-only names, grouped by object type.
     */
    void setIgnoredItems(IgnoredItems items);

    /**
     * @brief Declares explicit TLC-FI ↔ CIF name couples for items whose names
     *        differ on the two sides.
     *
     * Forwarded to the bridge. MUST be called before @ref init() — once init()
     * runs, the mapping table is already built. Both names of every couple must
     * be found on their side or init() aborts the process.
     *
     * @param couples Manual couples, grouped by object type.
     */
    void setManualCouples(ManualCouples couples);

    /// @brief Logging callback — invoked for all log messages from this component.
    ::tlcfi::fi::LogCallback onLog;

    /**
     * @brief Resets the CIF bridge state for reconnection.
     *
     * Clears all ID-to-index mappings and pending requests in the CifBridge.
     * This should be called when disconnecting to ensure stale mappings don't
     * persist across reconnections. The control loop must be stopped before
     * calling this method.
     *
     * @note This is called automatically by CifHost during cleanup,
     *       but can also be called manually if needed.
     */
    void resetBridge();

    /**
     * @brief Enable CIF_PARM1 persistence to @p file with a min write interval.
     *
     * Must be called before @ref init() — the store is consulted right after
     * the first CIF_INIT to apply persisted overrides on top of the
     * controller's defaults. Passing nothing leaves the feature disabled.
     *
     * @param file Path to the JSON file (created on first write).
     * @param minInterval Floor between consecutive disk writes (clamped to ≥ 50 ms).
     */
    void setParam1StoreFile(std::filesystem::path file,
                            std::chrono::milliseconds minInterval);

private:
    void log(::tlcfi::fi::LogLevel level, const std::string& message);

    /**
     * @brief Clock synchronization
     * 
     * Updates the CIF_KLOK[] array from system time. The legacy controller uses
     * this array for time-based logic.
     */
    
    /**
     * @brief Sets CIF_KLOK[] from system time.
     *
     * Populates the CIF clock array with year, month, day, hour, minute, second,
     * and various counters from the system clock time point. The seconds/tenths
     * counters CIF_KLOK[7]/[8] advance from their last value, so they survive
     * the loop being momentarily paused. Call @ref resetKlokCounters to zero
     * them at InControl boundaries.
     *
     * @param tp System clock time point to convert.
     */
    void setClockFromSystem(std::chrono::system_clock::time_point tp);

    /**
     * @brief Writes CIF_WPS[CIF_PROG_CONTROL] derived 1:1 from the TLC-FI
     *        ControlState, and sets CIF_WPSWIJZ.
     *
     * This is the *external* control axis — who is driving the controller.
     * The CCOL controller reads this to know whether the procesbesturing
     * (us) has declared it InControl / Offline / etc.
     *
     * Caller must hold @ref CifBridge::cifMutex.
     *
     * @param tlcfiControlState Integer value of @ref tlcfi::tlcfi::ControlState.
     */
    void setProgControl(int tlcfiControlState);

    /**
     * @brief Writes CIF_WPS[CIF_PROG_STATUS] and sets CIF_WPSWIJZ.
     *
     * This is the *operational* state axis — what mode the controller is
     * operating in (regulating, blinking, all-red, off, fault, switching
     * on/off). It is orthogonal to PROG_CONTROL: the controller can be
     * InControl while in a standby/blinking state. CCOL's `REG` macro is
     * exactly `CIF_WPS[CIF_PROG_STATUS] == CIF_STAT_REG`.
     *
     * In a physical automaat this comes from external signals; in our
     * hosted environment it is set by host policy (current default: REG
     * while InControl, GEDOOFD otherwise).
     *
     * Caller must hold @ref CifBridge::cifMutex.
     *
     * @param progStatus One of the CIF_STAT_* constants.
     */
    void setProgStatus(s_int16 progStatus);

    /**
     * @brief Reads CIF_GPS[] state-change requests from the controller and
     *        clears CIF_GPSWIJZ.
     *
     * The CCOL controller writes CIF_GPS[CIF_PROG_CONTROL] when it wants a
     * state transition (e.g. it wants to leave control). For now we only
     * log this; we do not propagate it back to TLC-FI because the operator
     * always wants control.
     *
     * Caller must hold @ref CifBridge::cifMutex.
     */
    void pollGpsStateRequest();

    /**
     * @brief Reads CIF_PARM1WIJZAP / CIF_PARM1WIJZPB and forwards changes to
     *        the param1 store, then clears both flags.
     *
     * Implements the runtime half of the "Cif parm" §1.2 contract: single-
     * index changes (flag >= 0) become @ref Param1Store::recordChange calls;
     * MEER (-2) bulk changes become @ref Param1Store::recordMultiChange;
     * INIT_PARM (-3) and GEEN_PARMWIJZ (-1) are ignored. No-op if no store
     * is configured.
     *
     * Caller must hold @ref CifBridge::cifMutex.
     */
    void pollParam1Changes();

    /**
     * @brief Capture the post-CIF_INIT baseline and re-apply persisted
     *        CIF_PARM1 overrides.
     *
     * Called from @ref init() after the first CIF_INIT and from the
     * onControlInControl re-INIT path: every re-INIT clobbers CIF_PARM1 with
     * fresh controller defaults, so without this the operator's tuning is
     * lost on every InControl transition. Also clears CIF_PARM1WIJZ* so the
     * just-loaded values do not immediately re-fire as edits. No-op if no
     * store is configured.
     *
     * Caller must hold @ref CifBridge::cifMutex.
     */
    void reapplyParam1Overrides();

    /**
     * @brief Zeroes CIF_KLOK[7] (seconds counter), CIF_KLOK[8] (tenths counter)
     *        and the internal tenths accumulator.
     *
     * Called when the host transitions into InControl so that the controller
     * sees a fresh time base for the new control session. The counters still
     * wrap at 32767 during operation per CVN spec; this only restarts them.
     */
    void resetKlokCounters();
    
    /**
     * @brief Control loop implementation
     * 
     * Runs the periodic control loop that:
     * - Updates CIF_KLOK[] from system time
     * - Calls `applicatieprogramma(0)` to execute the legacy controller
     * - Processes desired output changes via bridge
     * - Maintains 100ms timing with backlog handling
     */
    
    /**
     * @brief Main control loop executed in background thread.
     *
     * Runs at 100ms intervals:
     * 1. Updates CIF_KLOK[] from system time
     * 2. Calls `applicatieprogramma(0)` to execute legacy controller logic
     * 3. Processes CIF_GUS[] changes via bridge.processDesiredOutputChanges()
     * 4. Sleeps until next iteration, handling timing backlog if needed
     *
     * The loop continues until running_ is set to false.
     */
    void runLoop();

    /**
     * @brief Background drain loop for CIF_UBER / CIF_MON_UBER.
     *
     * CCOL calls uber_puts()/waitterm() inside applicatieprogramma() while
     * the control loop holds @ref CifBridge::cifMutex. If the outbound rings
     * fill mid-cycle, CCOL system functions (dump, V-Log monitor with
     * VLOGMODE 1/2) spin waiting for the host to advance CIF_UBER_LEES —
     * and we can't, because our control thread is the one stuck inside
     * applicatieprogramma. Draining must therefore happen concurrently
     * from a separate thread so that CIF_UBER_LEES can advance while the
     * C controller is still running. The two pointer pairs form a classic
     * single-producer/single-consumer ring (CCOL writes only _SCHRIJF, the
     * drain thread writes only _LEES), so no mutex is required on the
     * pointers themselves on x86_64.
     *
     * Loops while running_ is true, ticking every few milliseconds. When
     * a message server / logging server is configured, the corresponding
     * pollDataOut() also broadcasts complete messages to connected clients;
     * otherwise the drain just advances LEES = SCHRIJF (discard).
     */
    void runDrainLoop();

    /**
     * @brief (Re)initialises the host-owned CIF ring-buffer pointers.
     *
     * Called by init(), and again after each CIF_INIT call — the legacy
     * controller's CIF_INIT path resets these pointers for standalone use,
     * which would otherwise clobber the host/message-server configuration.
     */
    void initRingPointers();

    /**
     * @brief Builds the bridge id<->index mappings from the hosted controller.
     *
     * Reads the per-type object counts from the CVN-C interface constants
     * (CIF_PB_AANT_US_FC / _US_OV / _IS_D / _IS_OV) and queries the controller's
     * getCifItemName() for each index, populating CifBridge. Called first in
     * init(); validateMappings() at meta time then cross-checks the result
     * against the TLC's reported objects.
     */
    void buildMappingsFromController();

private:
	tlcfi::fi::FiControllerApp& app_;        ///< Reference to FiControllerApp for protocol communication.
	CifBridge& bridge_;            ///< Reference to CifBridge for state translation.
	std::atomic<bool> running_{false};  ///< Flag controlling the control loop execution.
	std::thread loopThread_;       ///< Thread running the control loop.
	std::thread drainThread_;      ///< Thread continuously draining CIF_UBER / CIF_MON_UBER concurrent with applicatieprogramma().
	s_int16 klokTenths_ = 0;       ///< Tenths-of-second accumulator that drives CIF_KLOK[7] increments.
	/// Last CIF_GPS values we have already reported. CCOL flips CIF_GPSWIJZ
	/// every cycle even when nothing changed, so we suppress duplicate logs
	/// by comparing against this cache. Accessed only under CifBridge::cifMutex.
	bool   gpsCacheValid_ = false;
	s_int16 lastGpsStatus_ = 0;
	s_int16 lastGpsControl_ = 0;
	s_int16 lastGpsFout_ = 0;
    std::optional<CifMessageServerConfig> messageServerConfig_;  ///< Configuration for message server.
    std::optional<CifMessageServerConfig> loggingServerConfig_;  ///< Configuration for logging server.
	std::unique_ptr<CifMessageServer> messageServer_;  ///< Optional TCP message server.
    std::unique_ptr<CifMessageServer> loggingServer_;  ///< Optional TCP logging server.

    /// Path to the CIF_PARM1 persistence file (empty = feature disabled).
    /// Set via @ref setParam1StoreFile before @ref init().
    std::filesystem::path param1StorePath_;
    /// Minimum interval between disk writes for the param1 store.
    std::chrono::milliseconds param1StoreInterval_{1000};
    /// Active CIF_PARM1 store. Constructed in @ref init() when a path is set.
    /// Lives as a unique_ptr so its writer thread is destroyed strictly before
    /// the rest of CifHost during teardown.
    std::unique_ptr<Param1Store> param1Store_;
};

} // namespace cif


