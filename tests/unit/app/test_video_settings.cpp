// video.json (I5 step 1, ADR-0016 §4): the shipped defaults, the user's file over them key by key (never written), a
// user file that does not load left out whole, the rooms' settings over their kind's.

#include <rv/app/video_settings.hpp>

#include "support/temp_dir.hpp"

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>

using rv::app::PlaylistMode;
using rv::app::ScreenKind;
using rv::app::VideoSettings;
using Json = nlohmann::json;

namespace {

const std::filesystem::path& assetRoot() {
    static const std::filesystem::path root = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return root;
}

/// The shipped document, read as the app reads it.
Json shipped() {
    auto document = rv::core::readJsonFile(assetRoot() / "config" / "video.json");
    REQUIRE(document.has_value());
    return *document;
}

void writeUser(const rv::test::TempDir& dir, std::string_view text) {
    REQUIRE(rv::core::writeTextFileAtomic(dir.path() / "video.json", std::string(text)).has_value());
}

} // namespace

TEST_CASE("app/video_settings: the shipped defaults hold every key, at ADR-0016's values", "[app][video_settings]") {
    auto parsed = rv::app::parseVideoSettings(shipped(), "video.json");
    REQUIRE(parsed.has_value());
    // The built-in defaults are the shipped ones (a broken installation falls back to them).
    CHECK(*parsed == VideoSettings{});
    CHECK(parsed->enabled);
    CHECK(parsed->maxDecoders == 2);
    CHECK(parsed->maxRows == 480);
    CHECK(parsed->viewerMaxRows == 720);
    CHECK(parsed->proximityMeters == 12.0);
    CHECK(parsed->crtEffect == rv::app::CrtEffect::Light);
    CHECK(parsed->hero.playlist == PlaylistMode::Shuffle);
    CHECK(parsed->hero.previewSeconds == 30.0);
    CHECK(parsed->hero.showOnFocus);
    CHECK(parsed->rooms.empty());

    // Every key is required in a whole document: one missing is named.
    Json missing = shipped();
    missing["hero"].erase("previewDuration");
    auto refused = rv::app::parseVideoSettings(missing, "video.json");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::ParseError);
    CHECK(refused.error().message.starts_with("/hero/previewDuration"));
}

TEST_CASE("app/video_settings: the user's file overrides key by key, a room's keys its kind's", "[app][video_settings]") {
    const rv::test::TempDir dir("rv-video-settings");
    // Missing: the defaults, no issue.
    auto none = rv::app::loadVideoSettings(assetRoot(), dir.path());
    REQUIRE(none.has_value());
    CHECK(none->issues.empty());
    CHECK(none->settings == VideoSettings{});

    writeUser(dir, R"({
  // comments are allowed
  "hero": {"previewDuration": 2, "playlist": "sequential"},
  "maxDecoders": 1,
  "rooms": {
    "room:snes": {"hero": {"playlist": "explicit", "games": ["g000000000000a004", "g000000000000a001"],
                           "showOnFocus": false}},
    "m0_testroom": {"hero": {"previewDuration": 5.5}}
  }
})");
    auto loaded = rv::app::loadVideoSettings(assetRoot(), dir.path());
    REQUIRE(loaded.has_value());
    CHECK(loaded->issues.empty());
    const VideoSettings& settings = loaded->settings;
    CHECK(settings.maxDecoders == 1);
    CHECK(settings.maxRows == 480); // kept
    CHECK(settings.hero.previewSeconds == 2.0);
    CHECK(settings.hero.playlist == PlaylistMode::Sequential);
    CHECK(settings.hero.showOnFocus); // kept
    CHECK(settings.secondary.previewSeconds == 30.0);

    const rv::app::ScreenSettings snes = settings.screen(ScreenKind::Hero, "room:snes");
    CHECK(snes.playlist == PlaylistMode::Explicit);
    CHECK(snes.games == std::vector<std::string>{"g000000000000a004", "g000000000000a001"});
    CHECK_FALSE(snes.showOnFocus);
    CHECK(snes.previewSeconds == 2.0); // the kind's, from the user's file
    const rv::app::ScreenSettings test = settings.screen(ScreenKind::Hero, "m0_testroom");
    CHECK(test.previewSeconds == 5.5);
    CHECK(test.playlist == PlaylistMode::Sequential);
    CHECK(settings.screen(ScreenKind::Hero, "room:nes") == settings.hero);
    CHECK(settings.screen(ScreenKind::Secondary, "room:snes") == settings.secondary);
    // Read, never written.
    auto text = rv::core::readTextFile(dir.path() / "video.json");
    REQUIRE(text.has_value());
    CHECK(text->starts_with("{\n  // comments are allowed"));
}

TEST_CASE("app/video_settings: a user file that does not load is left out whole", "[app][video_settings]") {
    const rv::test::TempDir dir("rv-video-settings");
    struct Case {
        const char* text;
        rv::ErrorCode code;
        const char* where;
    };
    const Case cases[] = {
        {R"({"hero": {"previewDuration": 0.5}, "maxDecoders": 1})", rv::ErrorCode::ValidationFailed,
         "/hero/previewDuration"},
        {R"({"maxDecoders": 3})", rv::ErrorCode::ValidationFailed, "/maxDecoders"},
        {R"({"hero": {"playlist": "random"}})", rv::ErrorCode::ValidationFailed, "/hero/playlist"},
        {R"({"hero": {"crtPreset": "fancy"}})", rv::ErrorCode::ValidationFailed, "/hero/crtPreset"},
        {R"({"heroes": {}})", rv::ErrorCode::ParseError, "/heroes"},
        {R"({"rooms": {"room:snes": {"hero": {"games": "g000000000000a004"}}}})", rv::ErrorCode::ParseError,
         "/rooms/room:snes/hero/games"},
        {R"({"rooms": {"room:snes": {"tv": {}}}})", rv::ErrorCode::ParseError, "/rooms/room:snes/tv"},
        {R"({"hero": {"games": []}})", rv::ErrorCode::ParseError, "/hero/games"},
        {R"({"enabled": "yes"})", rv::ErrorCode::ParseError, "/enabled"},
        {R"({"schemaVersion": 2})", rv::ErrorCode::Unsupported, "/schemaVersion"},
        {R"([1, 2])", rv::ErrorCode::ParseError, ""},
        {R"({"hero": )", rv::ErrorCode::ParseError, ""},
    };
    for (const Case& c : cases) {
        INFO(c.text);
        writeUser(dir, c.text);
        auto loaded = rv::app::loadVideoSettings(assetRoot(), dir.path());
        REQUIRE(loaded.has_value());
        CHECK(loaded->settings == VideoSettings{});
        REQUIRE(loaded->issues.size() == 1);
        CHECK(loaded->issues.front().code == c.code);
        CHECK(loaded->issues.front().message.find(c.where) != std::string::npos);
    }
    // An empty crtPreset is the kind's own.
    writeUser(dir, R"({"hero": {"crtPreset": ""}, "cabinet": {"crtPreset": "arcade"}})");
    auto presets = rv::app::loadVideoSettings(assetRoot(), dir.path());
    REQUIRE(presets.has_value());
    CHECK(presets->issues.empty());
    CHECK(presets->settings.cabinet.crtPreset == "arcade");
}

TEST_CASE("app/video_settings: documents merge member by member", "[app][video_settings]") {
    const Json base = Json::parse(R"({"a": 1, "o": {"x": 1, "y": {"z": 2}}, "list": [1, 2]})");
    const Json merged = rv::app::mergeVideoDocuments(base, Json::parse(R"({"o": {"y": {"w": 3}}, "list": [3]})"));
    CHECK(merged == Json::parse(R"({"a": 1, "o": {"x": 1, "y": {"z": 2, "w": 3}}, "list": [3]})"));
}
