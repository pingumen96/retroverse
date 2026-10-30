// xr.json (I10 step 3, ADR-0017 §8): the shipped defaults, the user's file over them key by key (never written), a user
// file that does not load left out whole.

#include <rv/xr/xr_settings.hpp>

#include "support/temp_dir.hpp"

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>

using rv::xr::XrSettings;
using Json = nlohmann::json;

namespace {

const std::filesystem::path& assetRoot() {
    static const std::filesystem::path root = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return root;
}

Json shipped() {
    auto document = rv::core::readJsonFile(assetRoot() / "config" / "xr.json");
    REQUIRE(document.has_value());
    return *document;
}

void writeUser(const rv::test::TempDir& dir, std::string_view text) {
    REQUIRE(rv::core::writeTextFileAtomic(dir.path() / "xr.json", std::string(text)).has_value());
}

} // namespace

TEST_CASE("xr/settings: the shipped defaults hold every key, at ADR-0017's values", "[xr][xr_settings]") {
    auto parsed = rv::xr::parseXrSettings(shipped(), "xr.json");
    REQUIRE(parsed.has_value());
    CHECK(*parsed == XrSettings{});
    CHECK_FALSE(parsed->vr);
    CHECK(parsed->comfort.turn == rv::xr::TurnMode::Snap);
    CHECK(parsed->comfort.snapDegrees == 30.0f);
    CHECK(parsed->posture == rv::xr::Posture::Standing);

    Json missing = shipped();
    missing["comfort"].erase("vignette");
    auto refused = rv::xr::parseXrSettings(missing, "xr.json");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.starts_with("/comfort/vignette"));

    Json unknown = shipped();
    unknown["handedness"] = "left";
    CHECK_FALSE(rv::xr::parseXrSettings(unknown, "xr.json").has_value());
}

TEST_CASE("xr/settings: the user's file overrides key by key; a bad one is left out whole", "[xr][xr_settings]") {
    const rv::test::TempDir dir("rv-xr-settings");
    auto none = rv::xr::loadXrSettings(assetRoot(), dir.path());
    REQUIRE(none.has_value());
    CHECK(none->issues.empty());
    CHECK(none->settings == XrSettings{});

    writeUser(dir, R"({
  // comments are allowed
  "display": {"mode": "vr"},
  "posture": "seated",
  "comfort": {"turn": "smooth", "vignette": 0.4}
})");
    auto loaded = rv::xr::loadXrSettings(assetRoot(), dir.path());
    REQUIRE(loaded.has_value());
    CHECK(loaded->issues.empty());
    CHECK(loaded->settings.vr);
    CHECK(loaded->settings.posture == rv::xr::Posture::Seated);
    CHECK(loaded->settings.comfort.turn == rv::xr::TurnMode::Smooth);
    CHECK(loaded->settings.comfort.vignette == 0.4f);
    CHECK(loaded->settings.comfort.snapDegrees == 30.0f); // kept
    CHECK(loaded->settings.renderScale == 1.0);

    writeUser(dir, R"({"renderScale": 3, "posture": "seated"})");
    auto refused = rv::xr::loadXrSettings(assetRoot(), dir.path());
    REQUIRE(refused.has_value());
    REQUIRE(refused->issues.size() == 1);
    CHECK(refused->issues.front().message.starts_with("/renderScale"));
    CHECK(refused->settings == XrSettings{}); // the whole file left out

    writeUser(dir, "[1, 2]");
    auto array = rv::xr::loadXrSettings(assetRoot(), dir.path());
    REQUIRE(array.has_value());
    CHECK(array->issues.size() == 1);
}
