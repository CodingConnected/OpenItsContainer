#pragma once

// --- Standard Library Includes ---
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

// --- Project Includes ---
#include "cif/CIF_INC.h"

// --- Namespace Declaration ---
namespace cif {

/**
 * @brief Async, sparse-overrides persistence for the CVN-C CIF_PARM1[] buffer.
 *
 * Persists operator/applic edits to CIF_PARM1 between host restarts, matching
 * the "Cif parm" reference doc (§1.1–§1.3). The file format stores only the
 * indices that differ from the controller's CIF_INIT defaults — when nothing
 * has been edited, no file is written at all.
 *
 * Ownership model:
 *  - `CifHost` constructs one of these once a path is configured.
 *  - After every CIF_INIT, CifHost calls @ref captureBaseline (passing the
 *    freshly-initialised CIF_PARM1) then @ref loadInto (applying the persisted
 *    overrides on top).
 *  - Each control-loop cycle, CifHost inspects CIF_PARM1WIJZAP / CIF_PARM1WIJZPB
 *    and forwards single-index edits via @ref recordChange or MEER-style bulk
 *    changes via @ref recordMultiChange.
 *
 * File I/O runs on a dedicated background writer thread. The control loop
 * never blocks on disk. I/O errors are logged to stderr and otherwise ignored
 * — losing a parm-file update is preferable to disrupting the live control
 * loop.
 *
 * Thread safety: all public methods are safe to call from any thread.
 *
 * @see "Cif parm" §1.1 (startup behaviour), §1.2 (run-time behaviour),
 *      §1.3 (high-frequency writer note — see also the TVGAMAX/THDAMAX
 *      knowledge note).
 */
class Param1Store {
public:
    /**
     * @brief Result of @ref loadInto.
     */
    enum class LoadResult {
        NoFile,           ///< No persistence file existed; buffer left untouched.
        LoadedOverrides,  ///< File loaded; overrides applied to buffer.
        Mismatched,       ///< File present but schema/count rejected; archived as .bak.<ts>; buffer untouched.
    };

    /**
     * @brief Construct a store backed by @p file with a minimum write interval.
     *
     * Starts the background writer thread immediately. The file is not touched
     * until @ref loadInto / @ref recordChange / @ref recordMultiChange is
     * called.
     *
     * @param file Absolute or working-directory-relative path to the JSON file.
     * @param minInterval Floor between consecutive disk writes (clamped to ≥ 50 ms).
     */
    Param1Store(std::filesystem::path file, std::chrono::milliseconds minInterval);

    /**
     * @brief Stops the writer thread (flushing any pending change) and joins it.
     */
    ~Param1Store();

    Param1Store(const Param1Store&) = delete;
    Param1Store& operator=(const Param1Store&) = delete;
    Param1Store(Param1Store&&) = delete;
    Param1Store& operator=(Param1Store&&) = delete;

    /**
     * @brief Capture the freshly-initialised CIF_PARM1 buffer as the baseline.
     *
     * Called by CifHost right after each `applicatieprogramma(CIF_INIT)`, with
     * the buffer in its post-init state. Subsequent edits are reckoned against
     * this baseline: an override is recorded when the buffer value differs from
     * the baseline at that index, and the override is dropped when the value
     * returns to baseline.
     *
     * Resets the internal edited-index set so a previous session's overrides
     * are forgotten — the file-on-disk is the source of truth across restarts;
     * @ref loadInto immediately afterwards re-seeds the set from the file.
     *
     * @param count Number of CIF_PARM1 entries (== CIF_PB_AANT_PARM1).
     * @param buffer Pointer to CIF_PARM1[0]; must remain valid for `count` entries.
     */
    void captureBaseline(int count, const s_int16* buffer);

    /**
     * @brief Apply persisted overrides on top of the current buffer.
     *
     * Reads the file at construction-time path. On schema/count match, copies
     * each `overrides[idx] → buffer[idx]` and seeds the internal edited-index
     * set so subsequent revert-to-baseline edits correctly drop the entry.
     * On mismatch (length differs, schema unknown, parse error, missing
     * `overrides`), the file is renamed to `<path>.bak.<unix-ts>` and the
     * buffer is left untouched.
     *
     * Caller must hold the CIF arrays mutex (the buffer is shared with the
     * control loop).
     *
     * @param currentCount Current CIF_PARM1 buffer size (CIF_PB_AANT_PARM1).
     * @param buffer Pointer to CIF_PARM1[0]; written if `LoadedOverrides`.
     * @return The disposition; useful for caller-side logging.
     */
    LoadResult loadInto(int currentCount, s_int16* buffer);

    /**
     * @brief Record a single-index edit signalled by CIF_PARM1WIJZ* >= 0.
     *
     * If `newValue` equals the captured baseline at `index`, the override (if
     * any) is dropped — a revert returns the file to the "no override for
     * this index" state, and removing the last override deletes the file.
     *
     * Non-blocking. Disk write is asynchronous, subject to the write-rate cap.
     *
     * @param index 0-based index into CIF_PARM1.
     * @param newValue Current value at that index.
     */
    void recordChange(int index, s_int16 newValue);

    /**
     * @brief Record a CIF_MEER_PARMWIJZ event (multiple indices changed).
     *
     * Rebuilds the override set by walking the buffer and comparing each entry
     * to the captured baseline. Any index that now differs is an override; any
     * index that now matches baseline drops out.
     *
     * @param count Number of CIF_PARM1 entries.
     * @param buffer Pointer to CIF_PARM1[0].
     */
    void recordMultiChange(int count, const s_int16* buffer);

private:
    /**
     * @brief Body of the background writer thread.
     */
    void writerLoop();

    /**
     * @brief Write the current `overrides_` to disk (or remove the file if empty).
     *
     * Caller holds `mutex_`. Performs tmp + rename for crash safety. Logs and
     * swallows std::exception.
     */
    void writeFile();

    /**
     * @brief Archive a rejected file to `<path>.bak.<unix-ts>`.
     *
     * Used for length-mismatch / parse-error files. Best-effort: failures are
     * logged but the load proceeds.
     */
    void archiveExistingFile();

    std::filesystem::path filePath_;
    std::chrono::milliseconds minInterval_;

    mutable std::mutex mutex_;                 ///< Guards baseline_, overrides_, dirty_, lastWriteAt_, stop_.
    std::condition_variable cv_;               ///< Writer thread sleeps on this.
    std::vector<s_int16> baseline_;            ///< Snapshot taken at the last captureBaseline() call.
    std::map<int, s_int16> overrides_;         ///< index → current value, for indices that differ from baseline.
    bool dirty_ = false;                       ///< overrides_ has changed since the last successful writeFile.
    std::chrono::steady_clock::time_point lastWriteAt_{}; ///< Time of the last successful write (or remove).
    bool stop_ = false;                        ///< Set by the destructor to wake the writer.
    std::thread writer_;                       ///< Background writer.

    static constexpr int kSchema = 1;
    static constexpr std::chrono::milliseconds kMinAllowedInterval{50};
    static constexpr std::chrono::milliseconds kTick{100};
};

} // namespace cif
