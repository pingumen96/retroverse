#pragma once

// Golden room selections (I3 step 2): tests/fixtures/world/selection/<name>.json holds the canonical text of a
// selection (world::writeRoomSelection()); a test compares what it computed with it, or rewrites it when the variable
// RV_UPDATE_GOLDEN is 1 (and fails, so that a rewrite is never mistaken for a pass: run again without it). Rewrite a
// golden only when a change of the rules is meant to change the selection, and review the difference.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>

#include <windows.h>

#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

namespace rv::test {

/// Whether RV_UPDATE_GOLDEN is "1".
inline bool updateGoldens() {
    wchar_t value[4] = {};
    const DWORD size = GetEnvironmentVariableW(L"RV_UPDATE_GOLDEN", value, 4);
    return size == 1 && value[0] == L'1';
}

/// The golden selection `name`'s file.
inline std::filesystem::path selectionGoldenPath(std::string_view name) {
    return core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "tests" / "fixtures" / "world" / "selection" /
           core::pathFromUtf8(std::string(name) + ".json");
}

/// CHECKs that `actual` is the golden selection `name`, byte for byte; with RV_UPDATE_GOLDEN=1 writes it instead.
inline void checkSelectionGolden(std::string_view name, const std::string& actual) {
    const std::filesystem::path path = selectionGoldenPath(name);
    if (updateGoldens()) {
        std::error_code made;
        std::filesystem::create_directories(path.parent_path(), made);
        REQUIRE(core::writeTextFileAtomic(path, actual).has_value());
        FAIL_CHECK("the golden selection " << core::pathToUtf8(path)
                                           << " was rewritten (RV_UPDATE_GOLDEN=1): review it");
        return;
    }
    const auto expected = core::readTextFile(path);
    REQUIRE(expected.has_value());
    CHECK(actual == *expected);
}

} // namespace rv::test
