// settings.json (ADR-0018 §2): what is read, what is written back as read, an unreadable file left out and backed up.

#include <rv/core/settings_file.hpp>

#include "support/temp_dir.hpp"

#include <rv/core/backups.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/paths.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using rv::core::SettingsFile;
using Json = nlohmann::json;

namespace {

void write(const std::filesystem::path& path, std::string_view text) {
    REQUIRE(rv::core::writeTextFileAtomic(path, std::string(text)).has_value());
}

constexpr std::array<std::string_view, 3> kAllowed = {"enabled", "audio.master", "hero.playlist"};

} // namespace

TEST_CASE("core/settings_file: a missing file is empty and no issue; reading writes nothing", "[core][settings_file]") {
    const rv::test::TempDir dir("rv-settings");
    const std::filesystem::path path = dir.path() / "settings.json";
    const SettingsFile file = SettingsFile::load(path);
    CHECK_FALSE(file.unreadable());
    CHECK(file.section("video", kAllowed).empty());
    CHECK_FALSE(std::filesystem::exists(path));
    CHECK(rv::core::settingsFileBeside(dir.path() / "config") == path);
    CHECK(rv::core::settingsFileBeside({}).empty());
}

TEST_CASE("core/settings_file: section() gives the allowed keys alone, shaped like the section", "[core][settings_file]") {
    const rv::test::TempDir dir("rv-settings");
    const std::filesystem::path path = dir.path() / "settings.json";
    write(path, R"({"schemaVersion": 1, "video": {"enabled": false, "stray": 1,
                    "audio": {"master": 0.5, "effects": 0.1}, "rooms": {"room:snes": {}}}})");
    const SettingsFile file = SettingsFile::load(path);
    REQUIRE_FALSE(file.unreadable());
    CHECK(file.section("video", kAllowed) == Json{{"enabled", false}, {"audio", {{"master", 0.5}}}});
    CHECK(file.section("world", kAllowed).empty());
    CHECK(file.value("video", "audio.master") == Json(0.5));
    CHECK_FALSE(file.value("video", "audio.video").has_value());
}

TEST_CASE("core/settings_file: a section that is no object holds nothing", "[core][settings_file]") {
    const rv::test::TempDir dir("rv-settings");
    const std::filesystem::path path = dir.path() / "settings.json";
    write(path, R"({"schemaVersion": 1, "video": 7})");
    const SettingsFile file = SettingsFile::load(path);
    CHECK_FALSE(file.unreadable());
    CHECK(file.section("video", kAllowed).empty());
}

TEST_CASE("core/settings_file: unknown keys, sections, values and a later schema are written back as read",
          "[core][settings_file]") {
    const rv::test::TempDir dir("rv-settings");
    const std::filesystem::path path = dir.path() / "settings.json";
    const Json original = R"({"schemaVersion": 7,
                              "future": {"nested": [1, 2, {"deep": true}]},
                              "video": {"audio": {"master": 0.5, "unknown": "kept"}, "laterKey": 1.25},
                              "note": "éà"})"_json;
    write(path, original.dump());

    SettingsFile file = SettingsFile::load(path);
    file.set("video", "audio.master", 0.75);
    file.set("player", "invertY", true);
    REQUIRE(file.save().has_value());

    Json expected = original;
    expected["video"]["audio"]["master"] = 0.75;
    expected["player"] = Json{{"invertY", true}};
    const SettingsFile again = SettingsFile::load(path);
    CHECK_FALSE(again.unreadable());
    CHECK(Json::parse(again.text()) == expected);
}

TEST_CASE("core/settings_file: a new file is plain JSON with its schema, ending in a line feed", "[core][settings_file]") {
    const rv::test::TempDir dir("rv-settings");
    const std::filesystem::path path = dir.path() / "nested" / "settings.json";
    SettingsFile file = SettingsFile::load(path);
    file.set("world", "reduceMotion", true);
    REQUIRE(file.save().has_value());
    auto text = rv::core::readTextFile(path);
    REQUIRE(text.has_value());
    CHECK(text->ends_with("}\n"));
    CHECK(Json::parse(*text) == Json{{"schemaVersion", 1}, {"world", {{"reduceMotion", true}}}});
    CHECK(Json::parse(*text, nullptr, false, false).is_object()); // no comments
}

TEST_CASE("core/settings_file: remove() lets the layer below show and leaves no empty object", "[core][settings_file]") {
    const rv::test::TempDir dir("rv-settings");
    SettingsFile file = SettingsFile::load(dir.path() / "settings.json");
    file.set("video", "audio.master", 0.5);
    file.set("video", "audio.video", 0.5);
    CHECK(file.remove("video", "audio.master"));
    CHECK(file.value("video", "audio.video").has_value());
    CHECK_FALSE(file.remove("video", "audio.master"));
    CHECK(file.remove("video", "audio.video"));
    CHECK(Json::parse(file.text()) == Json{{"schemaVersion", 1}});
}

TEST_CASE("core/settings_file: an unreadable file is left out whole, backed up, replaced at the next write",
          "[core][settings_file]") {
    const rv::test::TempDir dir("rv-settings");
    const std::filesystem::path path = dir.path() / "settings.json";
    const auto broken = GENERATE_COPY(std::string("{\"schemaVersion\": 1, \"video\": "), std::string("[1, 2]"),
                                      std::string("// a comment\n{\"schemaVersion\": 1}"), std::string("{\"a\": 1e400}"));
    write(path, broken);

    SettingsFile file = SettingsFile::load(path);
    REQUIRE(file.unreadable());
    CHECK(file.section("video", kAllowed).empty());
    // Reading alone left it where it was.
    CHECK(rv::core::readTextFile(path).value() == broken);

    REQUIRE(file.backUpUnreadable().has_value());
    const std::vector<std::filesystem::path> kept = rv::core::listBackupsOf(path, dir.path() / "backups");
    REQUIRE(kept.size() == 1);
    CHECK(rv::core::readTextFile(kept.front()).value() == broken);

    file.set("world", "reduceMotion", true);
    REQUIRE(file.save().has_value());
    const SettingsFile again = SettingsFile::load(path);
    CHECK_FALSE(again.unreadable());
    CHECK(again.value("world", "reduceMotion") == Json(true));
}

TEST_CASE("core/settings_file: an unreadable file that cannot be backed up is never replaced", "[core][settings_file]") {
    const rv::test::TempDir dir("rv-settings");
    const std::filesystem::path path = dir.path() / "settings.json";
    write(path, "{ broken");
    write(dir.path() / "backups", "a file where the backups' folder should be");
    SettingsFile file = SettingsFile::load(path);
    REQUIRE(file.unreadable());
    CHECK_FALSE(file.backUpUnreadable().has_value());
    file.set("world", "reduceMotion", true);
    const rv::Status saved = file.save();
    REQUIRE_FALSE(saved.has_value());
    CHECK(saved.error().code == rv::ErrorCode::Conflict);
    CHECK(rv::core::readTextFile(path).value() == "{ broken");
    // Once it could be kept aside, the file is replaced.
    std::filesystem::remove(dir.path() / "backups");
    REQUIRE(file.backUpUnreadable().has_value());
    CHECK(file.save().has_value());
}

TEST_CASE("core/settings_file: a loader reading the file never stops its replacement", "[core][settings_file]") {
    const rv::test::TempDir dir("rv-settings");
    const std::filesystem::path path = dir.path() / "settings.json";
    SettingsFile file = SettingsFile::load(path);
    file.set("world", "reduceMotion", true);
    REQUIRE(file.save().has_value());
    std::atomic<bool> done{false};
    std::atomic<int> unreadable{0};
    std::thread reader([&] {
        while (!done) {
            unreadable += SettingsFile::load(path).unreadable() ? 1 : 0;
        }
    });
    int failed = 0;
    std::string lastError;
    for (int index = 0; index < 200; ++index) {
        file.set("video", "audio.master", index / 1000.0);
        const auto saved = file.save();
        if (!saved) {
            ++failed;
            lastError = saved.error().message;
        }
    }
    done = true;
    reader.join();
    INFO(lastError);
    CHECK(failed == 0);
    CHECK(unreadable == 0);
}

TEST_CASE("core/settings_file: a readable file has nothing to back up", "[core][settings_file]") {
    const rv::test::TempDir dir("rv-settings");
    const std::filesystem::path path = dir.path() / "settings.json";
    write(path, R"({"schemaVersion": 1})");
    REQUIRE(SettingsFile::load(path).backUpUnreadable().has_value());
    CHECK_FALSE(std::filesystem::exists(dir.path() / "backups"));
}

TEST_CASE("core/settings_file: acceptedOverlay() costs a refused value that value alone", "[core][settings_file]") {
    const Json base{{"master", 1}, {"audio", {{"video", 1}, {"mode", "on"}}}};
    const auto accepts = [](const Json& document) { return document["master"].is_number(); };

    std::vector<std::string> refused;
    const Json whole = rv::core::acceptedOverlay(base, Json{{"audio", {{"video", 0.5}}}}, accepts, &refused);
    CHECK(whole["audio"]["video"] == Json(0.5));
    CHECK(whole["audio"]["mode"] == Json("on"));
    CHECK(refused.empty());

    const Json one = rv::core::acceptedOverlay(base, Json{{"master", "loud"}, {"audio", {{"video", 0.5}}}}, accepts, &refused);
    CHECK(one["master"] == Json(1));
    CHECK(one["audio"]["video"] == Json(0.5));
    CHECK(refused == std::vector<std::string>{"master"});
}

TEST_CASE("core/settings_file: shadowedKeys() names what a hand-written value lost to", "[core][settings_file]") {
    const Json overlay{{"audio", {{"master", 0.5}, {"video", 1.0}}}, {"enabled", false}};
    const Json written{{"audio", {{"master", 0.25}, {"video", 1.0}}}, {"crtEffect", "off"}};
    CHECK(rv::core::shadowedKeys(overlay, written) == std::vector<std::string>{"audio.master"});
}

TEST_CASE("core/paths: settings.json lies in the user data directory, never under config", "[core][paths]") {
    const rv::test::TempDir dir("rv-settings");
    rv::core::PathEnvironment environment;
    environment.executableDir = dir.path();
    rv::core::PathOptions options;
    options.assetRootOverride = dir.path();
    options.userDataOverride = dir.path() / "data";
    const auto paths = rv::core::Paths::resolve(options, environment);
    REQUIRE(paths.has_value());
    CHECK(paths->settingsFile() == paths->userDataRoot() / "settings.json");
    CHECK_FALSE(rv::core::pathIsWithin(paths->settingsFile(), paths->configDir()));
    CHECK(rv::core::settingsFileBeside(paths->configDir()) == paths->settingsFile());
}
