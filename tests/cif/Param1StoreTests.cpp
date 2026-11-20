#include <gtest/gtest.h>

#include "cif/Param1Store.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

// Unit tests for cif::Param1Store. The store does not touch the process-wide
// CIF_PARM1 buffer directly — it operates on caller-provided pointers — so each
// test creates a small local buffer and a temp file, exercises the store, and
// verifies the resulting file contents (or absence).

namespace {

using cif::Param1Store;
using nlohmann::json;
using namespace std::chrono_literals;

class Param1StoreTest : public ::testing::Test {
protected:
    std::filesystem::path tmpFile_;

    void SetUp() override {
        // Unique temp file per test so a parallel ctest run can't collide.
        std::mt19937_64 rng(std::random_device{}());
        const auto tag = std::to_string(rng());
        tmpFile_ = std::filesystem::temp_directory_path()
                   / ("param1store_test_" + tag + ".json");
        std::filesystem::remove(tmpFile_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(tmpFile_, ec);
        // Clean up any archive files the test may have produced.
        const auto parent = tmpFile_.parent_path();
        const auto stem = tmpFile_.filename().string();
        for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
            const auto name = entry.path().filename().string();
            if (name.rfind(stem + ".bak.", 0) == 0
                || name == stem + ".tmp") {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    // Read tmpFile_ as JSON (assumes it exists).
    json readFile() const {
        std::ifstream in(tmpFile_);
        json j;
        in >> j;
        return j;
    }

    // Wait until predicate() returns true or `timeout` elapses. The writer
    // thread is event-driven (CV-wake) plus a 100ms tick, so 2 s is generous.
    template <typename P>
    bool waitFor(P predicate, std::chrono::milliseconds timeout = 2000ms) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) return true;
            std::this_thread::sleep_for(20ms);
        }
        return predicate();
    }
};

// --- captureBaseline + recordChange: writes a sparse file ---

TEST_F(Param1StoreTest, NoFileWritten_WhenNothingEdited) {
    Param1Store store(tmpFile_, 50ms);
    s_int16 buf[4] = {10, 20, 30, 40};
    store.captureBaseline(4, buf);
    // No edits → no file. Give the writer thread a fair chance to wake.
    std::this_thread::sleep_for(200ms);
    EXPECT_FALSE(std::filesystem::exists(tmpFile_));
}

TEST_F(Param1StoreTest, SingleEdit_WritesSparseFile) {
    Param1Store store(tmpFile_, 50ms);
    s_int16 buf[4] = {10, 20, 30, 40};
    store.captureBaseline(4, buf);

    // Edit index 2 from 30 → 99.
    buf[2] = 99;
    store.recordChange(2, buf[2]);

    ASSERT_TRUE(waitFor([&]{ return std::filesystem::exists(tmpFile_); }));
    const auto j = readFile();
    EXPECT_EQ(j["schema"], 1);
    EXPECT_EQ(j["count"], 4);
    ASSERT_TRUE(j["overrides"].is_object());
    EXPECT_EQ(j["overrides"].size(), 1u);
    EXPECT_EQ(j["overrides"]["2"], 99);
}

TEST_F(Param1StoreTest, RevertToBaseline_DropsOverride) {
    Param1Store store(tmpFile_, 50ms);
    s_int16 buf[4] = {10, 20, 30, 40};
    store.captureBaseline(4, buf);

    buf[1] = 77;
    store.recordChange(1, buf[1]);
    ASSERT_TRUE(waitFor([&]{
        return std::filesystem::exists(tmpFile_)
            && readFile()["overrides"].contains("1");
    }));

    // Revert: value back to baseline.
    buf[1] = 20;
    store.recordChange(1, buf[1]);
    // File should disappear (no remaining overrides).
    ASSERT_TRUE(waitFor([&]{ return !std::filesystem::exists(tmpFile_); }));
}

TEST_F(Param1StoreTest, MultiChange_RebuildsOverrideSetFromBaseline) {
    Param1Store store(tmpFile_, 50ms);
    s_int16 buf[5] = {1, 2, 3, 4, 5};
    store.captureBaseline(5, buf);

    // Three indices diverge from baseline; one stays put.
    buf[0] = 100;  // diverges
    buf[2] = 300;  // diverges
    buf[4] = 5;    // unchanged
    store.recordMultiChange(5, buf);

    ASSERT_TRUE(waitFor([&]{ return std::filesystem::exists(tmpFile_); }));
    const auto j = readFile();
    EXPECT_EQ(j["overrides"].size(), 2u);
    EXPECT_EQ(j["overrides"]["0"], 100);
    EXPECT_EQ(j["overrides"]["2"], 300);
    EXPECT_FALSE(j["overrides"].contains("4"));

    // Now revert index 0 and add a new divergence at index 3.
    buf[0] = 1;    // back to baseline
    buf[3] = 999;  // new override
    store.recordMultiChange(5, buf);

    ASSERT_TRUE(waitFor([&]{
        if (!std::filesystem::exists(tmpFile_)) return false;
        const auto j2 = readFile();
        return j2["overrides"].size() == 2u
            && !j2["overrides"].contains("0")
            && j2["overrides"].contains("3");
    }));
}

// --- loadInto: file disposition on startup ---

TEST_F(Param1StoreTest, LoadInto_NoFile_LeavesBufferUntouched) {
    Param1Store store(tmpFile_, 50ms);
    s_int16 buf[3] = {1, 2, 3};
    store.captureBaseline(3, buf);
    const auto result = store.loadInto(3, buf);
    EXPECT_EQ(result, Param1Store::LoadResult::NoFile);
    EXPECT_EQ(buf[0], 1);
    EXPECT_EQ(buf[1], 2);
    EXPECT_EQ(buf[2], 3);
}

TEST_F(Param1StoreTest, LoadInto_MatchingFile_AppliesOverrides) {
    // Hand-craft a file with overrides at idx 0 and 2.
    {
        json j;
        j["schema"] = 1;
        j["count"] = 3;
        j["overrides"]["0"] = 111;
        j["overrides"]["2"] = 333;
        std::ofstream out(tmpFile_);
        out << j.dump(2);
    }

    Param1Store store(tmpFile_, 50ms);
    s_int16 buf[3] = {1, 2, 3};
    store.captureBaseline(3, buf);
    const auto result = store.loadInto(3, buf);
    EXPECT_EQ(result, Param1Store::LoadResult::LoadedOverrides);
    EXPECT_EQ(buf[0], 111);
    EXPECT_EQ(buf[1], 2);   // not overridden
    EXPECT_EQ(buf[2], 333);
}

TEST_F(Param1StoreTest, LoadInto_LengthMismatch_ArchivesAndLeavesBuffer) {
    // File written for a count-5 controller, but we now have a count-3 buffer.
    {
        json j;
        j["schema"] = 1;
        j["count"] = 5;
        j["overrides"]["0"] = 111;
        std::ofstream out(tmpFile_);
        out << j.dump(2);
    }

    Param1Store store(tmpFile_, 50ms);
    s_int16 buf[3] = {7, 8, 9};
    store.captureBaseline(3, buf);
    const auto result = store.loadInto(3, buf);
    EXPECT_EQ(result, Param1Store::LoadResult::Mismatched);
    EXPECT_EQ(buf[0], 7);
    EXPECT_EQ(buf[1], 8);
    EXPECT_EQ(buf[2], 9);
    // The original file should be gone (archived).
    EXPECT_FALSE(std::filesystem::exists(tmpFile_));
    // An archive should exist.
    const auto parent = tmpFile_.parent_path();
    const auto stem = tmpFile_.filename().string();
    bool foundArchive = false;
    for (const auto& entry : std::filesystem::directory_iterator(parent)) {
        if (entry.path().filename().string().rfind(stem + ".bak.", 0) == 0) {
            foundArchive = true;
            break;
        }
    }
    EXPECT_TRUE(foundArchive);
}

TEST_F(Param1StoreTest, LoadInto_CorruptJson_ArchivesAndLeavesBuffer) {
    {
        std::ofstream out(tmpFile_);
        out << "{not valid json";
    }

    Param1Store store(tmpFile_, 50ms);
    s_int16 buf[2] = {42, 43};
    store.captureBaseline(2, buf);
    const auto result = store.loadInto(2, buf);
    EXPECT_EQ(result, Param1Store::LoadResult::Mismatched);
    EXPECT_EQ(buf[0], 42);
    EXPECT_EQ(buf[1], 43);
}

TEST_F(Param1StoreTest, LoadInto_UnknownSchema_ArchivesAndLeavesBuffer) {
    {
        json j;
        j["schema"] = 99;
        j["count"] = 2;
        j["overrides"]["0"] = 100;
        std::ofstream out(tmpFile_);
        out << j.dump(2);
    }

    Param1Store store(tmpFile_, 50ms);
    s_int16 buf[2] = {5, 6};
    store.captureBaseline(2, buf);
    const auto result = store.loadInto(2, buf);
    EXPECT_EQ(result, Param1Store::LoadResult::Mismatched);
    EXPECT_EQ(buf[0], 5);
    EXPECT_EQ(buf[1], 6);
}

TEST_F(Param1StoreTest, LoadInto_InvalidOverrideKey_ArchivesAndLeavesBuffer) {
    {
        json j;
        j["schema"] = 1;
        j["count"] = 2;
        j["overrides"]["not-an-index"] = 100;
        std::ofstream out(tmpFile_);
        out << j.dump(2);
    }

    Param1Store store(tmpFile_, 50ms);
    s_int16 buf[2] = {5, 6};
    store.captureBaseline(2, buf);
    const auto result = store.loadInto(2, buf);
    EXPECT_EQ(result, Param1Store::LoadResult::Mismatched);
    EXPECT_EQ(buf[0], 5);
    EXPECT_EQ(buf[1], 6);
    EXPECT_FALSE(std::filesystem::exists(tmpFile_));
}

TEST_F(Param1StoreTest, LoadInto_SeedsInMemorySetForRevertSemantics) {
    // File contains an override at idx 1 (= 200).
    {
        json j;
        j["schema"] = 1;
        j["count"] = 3;
        j["overrides"]["1"] = 200;
        std::ofstream out(tmpFile_);
        out << j.dump(2);
    }

    Param1Store store(tmpFile_, 50ms);
    s_int16 buf[3] = {10, 20, 30};
    store.captureBaseline(3, buf);
    ASSERT_EQ(store.loadInto(3, buf), Param1Store::LoadResult::LoadedOverrides);
    EXPECT_EQ(buf[1], 200);

    // Now revert idx 1 to baseline. The override should drop and the file
    // should be removed (since no overrides remain).
    buf[1] = 20;
    store.recordChange(1, buf[1]);
    ASSERT_TRUE(waitFor([&]{ return !std::filesystem::exists(tmpFile_); }));
}

// --- Rate cap ---

TEST_F(Param1StoreTest, BurstOfChanges_CoalescesIntoFewWrites) {
    // 500 ms cap; in 200 ms we should see at most one file write.
    Param1Store store(tmpFile_, 500ms);
    s_int16 buf[3] = {0, 0, 0};
    store.captureBaseline(3, buf);

    // Hammer recordChange many times in tight succession.
    for (int i = 0; i < 50; ++i) {
        buf[0] = static_cast<s_int16>(i + 1);
        store.recordChange(0, buf[0]);
    }
    // Wait for at least one write to land.
    ASSERT_TRUE(waitFor([&]{ return std::filesystem::exists(tmpFile_); }, 1000ms));

    // The final value (50) should be on disk — coalescing must not drop the
    // most recent change.
    ASSERT_TRUE(waitFor([&]{
        if (!std::filesystem::exists(tmpFile_)) return false;
        return readFile()["overrides"]["0"].get<int>() == 50;
    }, 2000ms));
}

} // namespace
