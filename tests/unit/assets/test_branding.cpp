// The logo's files under assets/branding (docs/branding.md). scripts/branding.py writes them (and --check compares
// them with its geometry); these cases keep what the registry and the build rely on: every file registered as an
// original asset, and an icon whose directory names the sizes Windows asks for in the formats rc.exe embeds.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>

namespace {

const std::filesystem::path kAssets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);

/// The little-endian unsigned integer of `count` bytes at `at`.
std::uint32_t readLe(const std::vector<std::byte>& bytes, std::size_t at, std::size_t count) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        value |= std::to_integer<std::uint32_t>(bytes.at(at + i)) << (8 * i);
    }
    return value;
}

} // namespace

TEST_CASE("assets/branding: every logo file is an original asset with a registry entry", "[assets][branding]") {
    auto registry = rv::core::readJsonFile(kAssets / "registry" / "assets.json");
    REQUIRE(registry.has_value());

    std::vector<std::string> files;
    for (const auto& file : std::filesystem::directory_iterator(kAssets / "branding")) {
        if (file.is_regular_file()) {
            files.push_back("branding/" + rv::core::pathToUtf8(file.path().filename()));
        }
    }
    REQUIRE(std::find(files.begin(), files.end(), "branding/retroverse.ico") != files.end());
    REQUIRE(std::find(files.begin(), files.end(), "branding/retroverse-lockup.svg") != files.end());
    for (const std::string& file : files) {
        INFO(file);
        const nlohmann::json* entry = nullptr;
        for (const nlohmann::json& candidate : registry->at("assets")) {
            if (candidate.at("path") == file) {
                entry = &candidate;
            }
        }
        REQUIRE(entry != nullptr);
        CHECK(entry->at("kind") == "branding");
        CHECK(entry->at("provenance") == "retroverse-original");
    }
}

TEST_CASE("assets/branding: the icon holds 16 to 256 px, 256 as PNG and the others as 32-bit bitmaps",
          "[assets][branding]") {
    auto bytes = rv::core::readBinaryFile(kAssets / "branding" / "retroverse.ico");
    REQUIRE(bytes.has_value());
    const std::vector<std::byte>& ico = *bytes;

    constexpr std::array<std::uint32_t, 8> kSizes{16, 20, 24, 32, 40, 48, 64, 256};
    REQUIRE(ico.size() > 6 + 16 * kSizes.size());
    CHECK(readLe(ico, 0, 2) == 0); // reserved
    CHECK(readLe(ico, 2, 2) == 1); // an icon, not a cursor
    REQUIRE(readLe(ico, 4, 2) == kSizes.size());

    for (std::size_t k = 0; k < kSizes.size(); ++k) {
        const std::uint32_t size = kSizes[k];
        const std::size_t entry = 6 + 16 * k;
        INFO(size);
        CHECK(readLe(ico, entry, 1) == size % 256); // width and height; 0 stands for 256
        CHECK(readLe(ico, entry + 1, 1) == size % 256);
        CHECK(readLe(ico, entry + 6, 2) == 32); // bits per pixel
        const std::uint32_t length = readLe(ico, entry + 8, 4);
        const std::uint32_t offset = readLe(ico, entry + 12, 4);
        REQUIRE(std::uint64_t{offset} + length <= ico.size());
        if (size == 256) {
            constexpr std::array<std::uint32_t, 8> kPngSignature{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
            for (std::size_t i = 0; i < kPngSignature.size(); ++i) {
                CHECK(readLe(ico, offset + i, 1) == kPngSignature[i]);
            }
        } else {
            CHECK(readLe(ico, offset, 4) == 40); // BITMAPINFOHEADER
            CHECK(readLe(ico, offset + 4, 4) == size);
            CHECK(readLe(ico, offset + 8, 4) == size * 2); // the color rows, then the mask's
            CHECK(readLe(ico, offset + 14, 2) == 32);
            const std::uint32_t maskStride = (size + 31) / 32 * 4;
            CHECK(length == 40 + size * size * 4 + maskStride * size);
        }
    }
}
