// WorldConfig (rv/world/world_config.hpp, ADR-0010 §2): the shipped preset, the user's world.json overriding it per
// key (a broken file left out whole with an issue), the default seed derived from the library id, and the canonical
// form read back exactly.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <string>
#include <string_view>

#include <rv/core/file_io.hpp>
#include <rv/core/seed.hpp>
#include <rv/core/text.hpp>
#include <rv/world/world_config.hpp>

#include "support/temp_dir.hpp"

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::EndsWith;
using rv::world::PlacementInclude;
using rv::world::WorldConfig;

namespace {

const std::filesystem::path kAssets = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);

void writeFile(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    REQUIRE(rv::core::writeTextFileAtomic(path, text).has_value());
}

/// An asset root holding the shipped preset and, when given, another one.
std::filesystem::path assetRootWith(const rv::test::TempDir& dir, std::string_view extraId = {},
                                    std::string_view extraText = {}) {
    const std::filesystem::path root = dir.path() / "assets";
    std::filesystem::create_directories(root / "world" / "presets");
    std::filesystem::copy_file(kAssets / "world" / "presets" / "retro_museum_cozy.json",
                               root / "world" / "presets" / "retro_museum_cozy.json");
    if (!extraId.empty()) {
        writeFile(root / "world" / "presets" / rv::core::pathFromUtf8(std::string(extraId) + ".json"), extraText);
    }
    return root;
}

constexpr std::string_view kGalleryPreset = R"({
  "schemaVersion": 1, "id": "minimal_gallery", "name": "MinimalGallery",
  "decorationDensityPermille": 100, "growthReservePermille": 200,
  "mainPassageWidthMm": 1600, "minClearWidthMm": 1300, "placement": {"include": "all"}
})";

} // namespace

TEST_CASE("world/world_config: the shipped preset", "[world][world_config]") {
    auto preset = rv::world::loadPreset(kAssets, rv::world::kDefaultPreset);
    REQUIRE(preset.has_value());
    // ADR-0010 §2: decoration density 0.4, growth reserve 0.15, the main passage 1.4 m, the hard minimum 1.2 m.
    WorldConfig expected;
    CHECK(*preset == expected);
    CHECK(preset->preset == "retro_museum_cozy");
    CHECK(preset->decorationDensityPermille == 400);
    CHECK(preset->growthReservePermille == 150);
    CHECK(preset->mainPassageWidthMm == 1400);
    CHECK(preset->minClearWidthMm == 1200);
    CHECK(preset->include == PlacementInclude::Covered);
    CHECK_FALSE(preset->seed.has_value());
    CHECK_FALSE(preset->reduceMotion);

    auto missing = rv::world::loadPreset(kAssets, "no_such_preset");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == rv::ErrorCode::NotFound);
    CHECK(rv::world::loadPreset(kAssets, "../presets/x").error().code == rv::ErrorCode::InvalidArgument);
}

TEST_CASE("world/world_config: the default seed derives from the library id", "[world][world_config]") {
    const WorldConfig config;
    // ADR-0010 §7 and docs/i3/step-0b.md: deriveSeed("world", "esde") = 0xa1fac9cfdf9c5474.
    CHECK(rv::world::worldSeed(config, rv::catalog::LibraryId{}) == 0xa1fac9cfdf9c5474ull);
    CHECK(rv::world::worldSeed(config, *rv::catalog::LibraryId::parse("home")) ==
          rv::core::deriveSeed("world", "home"));
    WorldConfig seeded;
    seeded.seed = 102938;
    CHECK(rv::world::worldSeed(seeded, rv::catalog::LibraryId{}) == 102938);
}

TEST_CASE("world/world_config: world.json overrides the preset per key", "[world][world_config]") {
    const rv::test::TempDir dir;
    const std::filesystem::path assets = assetRootWith(dir, "minimal_gallery", kGalleryPreset);
    const std::filesystem::path config = dir.path() / "config";

    SECTION("no world.json: the default preset, no issue") {
        auto loaded = rv::world::loadWorldConfig(assets, config);
        REQUIRE(loaded.has_value());
        CHECK(loaded->config == WorldConfig{});
        CHECK(loaded->issues.empty());
    }
    SECTION("a few keys") {
        writeFile(config / "world.json", R"(// my world
{
  "seed": 102938,              // the room I like
  "growthReservePermille": 250,
  "placement": {"include": "all"},
  "reduceMotion": true
})");
        auto loaded = rv::world::loadWorldConfig(assets, config);
        REQUIRE(loaded.has_value());
        CHECK(loaded->issues.empty());
        WorldConfig expected;
        expected.seed = 102938;
        expected.growthReservePermille = 250;
        expected.include = PlacementInclude::All;
        expected.reduceMotion = true;
        CHECK(loaded->config == expected);
    }
    SECTION("another preset, then the user's keys over it") {
        writeFile(config / "world.json", R"({"preset": "minimal_gallery", "decorationDensityPermille": 0,
                                            "seed": null})");
        auto loaded = rv::world::loadWorldConfig(assets, config);
        REQUIRE(loaded.has_value());
        CHECK(loaded->issues.empty());
        CHECK(loaded->config.preset == "minimal_gallery");
        CHECK(loaded->config.decorationDensityPermille == 0);
        CHECK(loaded->config.growthReservePermille == 200);
        CHECK(loaded->config.mainPassageWidthMm == 1600);
        CHECK(loaded->config.include == PlacementInclude::All);
        CHECK_FALSE(loaded->config.seed.has_value());
    }
}

TEST_CASE("world/world_config: a broken world.json is left out whole with an issue", "[world][world_config]") {
    const rv::test::TempDir dir;
    const std::filesystem::path assets = assetRootWith(dir);
    const std::filesystem::path config = dir.path() / "config";
    struct Case {
        std::string_view text;
        std::string_view message;
        std::string_view line; ///< ":<line>" the issue's context ends with, empty for none
    };
    const Case cases[] = {
        {"{\n  \"seed\": 1,\n  \"growthReservePermille\": 0.15\n}", "/growthReservePermille: expected an integer",
         ":3"},
        {"{\n  \"seed\": 1,\n  \"decorationDensity\": 400\n}", "/decorationDensity: unknown key", ":3"},
        {"{\n  \"seed\": 1\n  \"reduceMotion\": true\n}", "syntax error", ":3"},
        {"{\n  \"seed\": 1,\n  \"seed\": 2\n}", "/seed: the key appears twice", ":3"},
        {"{\n  \"placement\": {\"include\": \"some\"}\n}", "/placement/include: expected one of covered, all", ":2"},
        {"{\n  \"seed\": -5\n}", "/seed: expected an integer from 0", ":2"},
        {"{\n  \"mainPassageWidthMm\": 900\n}", "minClearWidthMm: expected 600 to the main passage's width", ""},
        {"{\n  \"preset\": \"no_such_preset\"\n}", "the preset \"no_such_preset\" that world.json names", ""},
        {"[1, 2]", "expected an object", ":1"},
        // Lines are counted through comments and after a byte order mark.
        {"/* my\n   world */ {\n  // the seed\n  \"seed\": \"x\"\n}", "/seed: expected an integer", ":4"},
        {"\xEF\xBB\xBF{\n  \"reduceMotion\": 1\n}", "/reduceMotion: expected a boolean", ":2"},
    };
    for (const Case& test : cases) {
        INFO(test.text);
        writeFile(config / "world.json", test.text);
        auto loaded = rv::world::loadWorldConfig(assets, config);
        REQUIRE(loaded.has_value());
        CHECK(loaded->config == WorldConfig{});
        REQUIRE(loaded->issues.size() == 1);
        CHECK_THAT(loaded->issues.front().message, ContainsSubstring(std::string(test.message)));
        if (!test.line.empty()) {
            CHECK_THAT(loaded->issues.front().context, EndsWith("world.json" + std::string(test.line)));
        }
    }
}

TEST_CASE("world/world_config: presets are checked like world.json", "[world][world_config]") {
    const rv::test::TempDir dir;
    auto broken = rv::world::parsePreset(R"({"schemaVersion": 1, "id": "x", "name": "X",
      "decorationDensityPermille": 400, "growthReservePermille": 150, "mainPassageWidthMm": 1400,
      "placement": {"include": "covered"}})",
                                         "x.json");
    REQUIRE_FALSE(broken.has_value());
    CHECK_THAT(broken.error().message, ContainsSubstring("/minClearWidthMm: missing"));

    auto newer = rv::world::parsePreset(R"({"schemaVersion": 2})", "x.json");
    REQUIRE_FALSE(newer.has_value());
    CHECK_THAT(newer.error().message, ContainsSubstring("/schemaVersion"));

    // A preset whose id is not its file's name, and a default preset that does not load: the installation is broken.
    const std::filesystem::path assets = assetRootWith(dir, "renamed", std::string(kGalleryPreset));
    auto renamed = rv::world::loadPreset(assets, "renamed");
    REQUIRE_FALSE(renamed.has_value());
    CHECK_THAT(renamed.error().message, ContainsSubstring("is not its file's name"));
    std::filesystem::remove(assets / "world" / "presets" / "retro_museum_cozy.json");
    CHECK_FALSE(rv::world::loadWorldConfig(assets, dir.path() / "config").has_value());
}

TEST_CASE("world/world_config: the canonical form round-trips and refuses", "[world][world_config]") {
    WorldConfig config;
    config.seed = 18446744073709551615ull;
    config.include = PlacementInclude::All;
    config.reduceMotion = true;
    auto text = rv::world::writeWorldConfig(config);
    REQUIRE(text.has_value());
    CHECK(*text == "{\"decorationDensityPermille\":400,\"growthReservePermille\":150,\"mainPassageWidthMm\":1400,"
                   "\"minClearWidthMm\":1200,\"placement\":{\"include\":\"all\"},\"preset\":\"retro_museum_cozy\","
                   "\"reduceMotion\":true,\"seed\":18446744073709551615}\n");
    auto read = rv::world::readWorldConfig(*text, "config.json");
    REQUIRE(read.has_value());
    CHECK(*read == config);
    CHECK(*rv::world::writeWorldConfig(*read) == *text);

    const WorldConfig defaults;
    auto defaultText = rv::world::writeWorldConfig(defaults);
    REQUIRE(defaultText.has_value());
    CHECK_THAT(*defaultText, ContainsSubstring("\"seed\":null"));
    CHECK(*rv::world::readWorldConfig(*defaultText, "config.json") == defaults);

    // Values out of range, written or read.
    WorldConfig narrow;
    narrow.minClearWidthMm = 1500;
    auto refused = rv::world::writeWorldConfig(narrow);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::InvalidArgument);
    CHECK(refused.error().context == "minClearWidthMm");
    WorldConfig badPreset;
    badPreset.preset = "Retro Museum";
    CHECK_FALSE(rv::world::writeWorldConfig(badPreset).has_value());

    std::string tooWide = *defaultText;
    tooWide.replace(tooWide.find("\"minClearWidthMm\":1200"), 22, "\"minClearWidthMm\":1500");
    auto wide = rv::world::readWorldConfig(tooWide, "config.json");
    REQUIRE_FALSE(wide.has_value());
    CHECK_THAT(wide.error().message, ContainsSubstring("/minClearWidthMm"));
    CHECK_FALSE(rv::world::readWorldConfig("{\"preset\": \"x\"}\n", "config.json").has_value());
}
