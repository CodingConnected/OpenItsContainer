#include "cif/Param1Store.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <iostream>
#include <system_error>

namespace cif {

namespace {

/**
 * @brief Tmp suffix used for atomic-rename writes.
 */
constexpr const char* kTmpSuffix = ".tmp";

std::filesystem::path tmpPath(const std::filesystem::path& base) {
    auto p = base;
    p += kTmpSuffix;
    return p;
}

std::filesystem::path backupPath(const std::filesystem::path& base) {
    const auto stamp = std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::system_clock::now().time_since_epoch())
                           .count();
    auto p = base;
    p += ".bak.";
    p += std::to_string(stamp);
    return p;
}

} // namespace

Param1Store::Param1Store(std::filesystem::path file, std::chrono::milliseconds minInterval)
    : filePath_(std::move(file)),
      minInterval_(minInterval < kMinAllowedInterval ? kMinAllowedInterval : minInterval) {
    writer_ = std::thread([this]{ writerLoop(); });
}

Param1Store::~Param1Store() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (writer_.joinable()) {
        writer_.join();
    }
    // Final flush on the calling thread: the writer loop drains pending work
    // before exiting, but if it exits between a recordChange and its next
    // wake-up the lastWriteAt rate-cap can still defer the write. Force one
    // final attempt so we never lose an in-flight edit on shutdown.
    std::lock_guard<std::mutex> lock(mutex_);
    if (dirty_) {
        writeFile();
    }
}

void Param1Store::captureBaseline(int count, const s_int16* buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    baseline_.assign(buffer, buffer + count);
    // Forget any in-memory overrides — the on-disk file is the source of truth,
    // and loadInto() (called right after captureBaseline by CifHost) re-seeds
    // the set from it.
    overrides_.clear();
    // Do not set dirty_ here; captureBaseline alone doesn't justify a file
    // write. If loadInto reads N overrides off disk we end up with the same
    // file contents we started with — no need to rewrite.
}

Param1Store::LoadResult Param1Store::loadInto(int currentCount, s_int16* buffer) {
    std::error_code ec;
    if (!std::filesystem::exists(filePath_, ec)) {
        return LoadResult::NoFile;
    }

    nlohmann::json j;
    try {
        std::ifstream in(filePath_);
        if (!in) {
            // Exists but unopenable (permissions); treat as mismatch so we
            // archive it and warn rather than silently masking the file.
            // ifstream goes out of scope here, closing the handle before
            // archiveExistingFile() tries to rename. Windows does not allow
            // rename on a file with an open exclusive read handle.
        } else {
            in >> j;
        }
    } catch (const std::exception& ex) {
        std::cerr << "Param1Store: failed to parse '" << filePath_.string()
                  << "': " << ex.what() << " — archiving and continuing\n";
        archiveExistingFile();
        std::lock_guard<std::mutex> lock(mutex_);
        overrides_.clear();
        return LoadResult::Mismatched;
    }

    if (j.is_null()
        || !j.is_object()
        || !j.contains("schema") || j["schema"].get<int>() != kSchema
        || !j.contains("count")  || j["count"].get<int>() != currentCount
        || !j.contains("overrides") || !j["overrides"].is_object()) {
        archiveExistingFile();
        return LoadResult::Mismatched;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    overrides_.clear();
    try {
        for (auto it = j["overrides"].begin(); it != j["overrides"].end(); ++it) {
            const int idx = std::stoi(it.key());
            if (idx < 0 || idx >= currentCount) {
                // Stale index in a length-match file (shouldn't happen, but
                // belt-and-braces): archive and bail.
                overrides_.clear();
                // Release the lock briefly: archiveExistingFile() does only
                // filesystem work and does not touch shared state.
                // (std::lock_guard does not support manual release; we accept
                // a brief hold across the rename.)
                archiveExistingFile();
                return LoadResult::Mismatched;
            }
            const auto value = static_cast<s_int16>(it.value().get<int>());
            buffer[idx] = value;
            overrides_.emplace(idx, value);
        }
    } catch (const std::exception& ex) {
        std::cerr << "Param1Store: invalid override entry in '" << filePath_.string()
                  << "': " << ex.what() << " — archiving and continuing\n";
        overrides_.clear();
        archiveExistingFile();
        return LoadResult::Mismatched;
    }
    return LoadResult::LoadedOverrides;
}

void Param1Store::recordChange(int index, s_int16 newValue) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < 0 || static_cast<size_t>(index) >= baseline_.size()) {
            return;
        }
        const s_int16 base = baseline_[index];
        if (newValue == base) {
            // Revert to baseline — drop the override.
            auto it = overrides_.find(index);
            if (it != overrides_.end()) {
                overrides_.erase(it);
                dirty_ = true;
            }
        } else {
            auto [it, inserted] = overrides_.insert_or_assign(index, newValue);
            // insert_or_assign always assigns; mark dirty unless the value
            // already matched what we had recorded.
            (void)it;
            (void)inserted;
            dirty_ = true;
        }
    }
    cv_.notify_one();
}

void Param1Store::recordMultiChange(int count, const s_int16* buffer) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (baseline_.empty() || static_cast<int>(baseline_.size()) != count) {
            return;
        }
        std::map<int, s_int16> next;
        for (int i = 0; i < count; ++i) {
            if (buffer[i] != baseline_[i]) {
                next.emplace(i, buffer[i]);
            }
        }
        if (next != overrides_) {
            overrides_.swap(next);
            dirty_ = true;
        }
    }
    cv_.notify_one();
}

void Param1Store::writerLoop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        cv_.wait_for(lock, kTick, [this]{ return stop_ || dirty_; });
        if (stop_) {
            // Drain any pending write before exiting.
            if (dirty_) {
                writeFile();
            }
            return;
        }
        if (!dirty_) {
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastWriteAt_ < minInterval_) {
            // Rate-capped: keep dirty, keep waiting.
            continue;
        }
        writeFile();
    }
}

void Param1Store::writeFile() {
    // Caller holds mutex_.
    try {
        if (overrides_.empty()) {
            std::error_code ec;
            std::filesystem::remove(filePath_, ec);
            // Ignore ec: a missing file is the desired post-state either way.
        } else {
            nlohmann::json j;
            j["schema"] = kSchema;
            j["count"] = static_cast<int>(baseline_.size());
            nlohmann::json ov = nlohmann::json::object();
            for (const auto& [idx, value] : overrides_) {
                ov[std::to_string(idx)] = static_cast<int>(value);
            }
            j["overrides"] = std::move(ov);

            const auto tmp = tmpPath(filePath_);
            {
                std::ofstream out(tmp, std::ios::out | std::ios::trunc);
                if (!out) {
                    throw std::runtime_error("cannot open tmp file for writing: " + tmp.string());
                }
                out << j.dump(2);
                if (!out) {
                    throw std::runtime_error("write failed to tmp file: " + tmp.string());
                }
            }

            std::error_code ec;
            std::filesystem::rename(tmp, filePath_, ec);
            if (ec) {
                // rename failed (e.g. cross-device); fall back to copy+remove.
                std::filesystem::copy_file(tmp, filePath_,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                std::filesystem::remove(tmp);
                if (ec) {
                    throw std::runtime_error("rename to '" + filePath_.string() + "' failed: " + ec.message());
                }
            }
        }
        dirty_ = false;
        lastWriteAt_ = std::chrono::steady_clock::now();
    } catch (const std::exception& ex) {
        std::cerr << "Param1Store: write to '" << filePath_.string()
                  << "' failed: " << ex.what() << "\n";
        // Keep dirty_ set so we retry on the next tick. Update lastWriteAt_
        // anyway, so a persistently failing target doesn't tight-loop on
        // failures faster than minInterval_.
        lastWriteAt_ = std::chrono::steady_clock::now();
    }
}

void Param1Store::archiveExistingFile() {
    std::error_code ec;
    const auto bak = backupPath(filePath_);
    std::filesystem::rename(filePath_, bak, ec);
    if (ec) {
        std::cerr << "Param1Store: failed to archive '" << filePath_.string()
                  << "' as '" << bak.string() << "': " << ec.message() << "\n";
    } else {
        std::cerr << "Param1Store: archived '" << filePath_.string()
                  << "' as '" << bak.string() << "'\n";
    }
}

} // namespace cif
