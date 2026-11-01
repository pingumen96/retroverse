// The settings' layers (ADR-0018 §2, §3): the shipped defaults, the user's config files, settings.json over them; what a
// row writes; Ripristina; comments of the user's files untouched; a stray or bad value never reaching a strict loader.

#include <rv/app/player_settings.hpp>
#include <rv/app/setting_rows.hpp>
#include <rv/app/settings_store.hpp>
#include <rv/app/video_settings.hpp>

#include "support/temp_dir.hpp"

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/settings_file.hpp>
#include <rv/core/text.hpp>
#include <rv/input/action_map.hpp>
#include <rv/world/room_look.hpp>
#include <rv/world/world_config.hpp>
#include <rv/xr/xr_settings.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using rv::app::SettingRow;
using rv::app::SettingsLayers;
using rv::app::SettingsStore;
using Json = nlohmann::json;

namespace {

const std::filesystem::path& assetRoot() {
    static const std::filesystem::path root = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return root;
}

/// A user data directory: <dir>/config and <dir>/settings.json, as the app has them.
struct UserData {
    rv::test::TempDir dir{"rv-settings-store"};
    std::filesystem::path config = dir.path() / "config";
    std::filesystem::path settings = dir.path() / "settings.json";

    UserData() { std::filesystem::create_directories(config); }

    void writeConfig(std::string_view name, std::string_view text) const {
        REQUIRE(rv::core::writeTextFileAtomic(config / std::string(name), std::string(text)).has_value());
    }
    void writeSettings(std::string_view text) const {
        REQUIRE(rv::core::writeTextFileAtomic(settings, std::string(text)).has_value());
    }
    [[nodiscard]] Json readSettings() const {
        auto text = rv::core::readTextFile(settings);
        REQUIRE(text.has_value());
        return Json::parse(*text);
    }
    [[nodiscard]] rv::app::LoadedVideoSettings video() const {
        auto loaded = rv::app::loadVideoSettings(assetRoot(), config);
        REQUIRE(loaded.has_value());
        return *loaded;
    }
    [[nodiscard]] rv::app::LoadedPlayerSettings player() const {
        auto loaded = rv::app::loadPlayerSettings(assetRoot(), config);
        REQUIRE(loaded.has_value());
        return *loaded;
    }
    [[nodiscard]] rv::world::LoadedWorldConfig world() const {
        auto loaded = rv::world::loadWorldConfig(assetRoot(), config);
        REQUIRE(loaded.has_value());
        return *loaded;
    }
    [[nodiscard]] rv::xr::LoadedXrSettings xr() const {
        auto loaded = rv::xr::loadXrSettings(assetRoot(), config);
        REQUIRE(loaded.has_value());
        return *loaded;
    }
    /// The store as the app makes it.
    [[nodiscard]] SettingsLayers layers() const {
        SettingsLayers below;
        below.video = video().belowSettings;
        below.world = world().belowSettings;
        below.player = player().belowSettings;
        below.xr = xr().belowSettings;
        return below;
    }
};

const std::vector<SettingRow>& rows() {
    static const std::vector<std::string> profiles = rv::world::lightingProfileIds(assetRoot());
    static const std::vector<SettingRow> table = rv::app::settingRows(profiles);
    return table;
}

const SettingRow& rowOf(const SettingsStore& store, std::string_view id) {
    const SettingRow* row = store.row(id);
    REQUIRE(row != nullptr);
    return *row;
}

} // namespace

TEST_CASE("app/settings: the layers' order: defaults, then config, then settings.json", "[app][settings]") {
    const UserData data;
    CHECK(data.video().settings.audio.master == 1.0);

    data.writeConfig("video.json", R"({"audio": {"master": 0.5, "video": 0.25}})");
    CHECK(data.video().settings.audio.master == 0.5);

    data.writeSettings(R"({"schemaVersion": 1, "video": {"audio": {"master": 0.75}}})");
    const rv::app::LoadedVideoSettings loaded = data.video();
    CHECK(loaded.settings.audio.master == 0.75);
    // A key the screen never touched is decided by the hand-written file as before.
    CHECK(loaded.settings.audio.video == 0.25);
    // The layer below stays known, for Ripristina.
    CHECK(loaded.belowSettings["audio"]["master"] == Json(0.5));
    CHECK(loaded.issues.empty());
    CHECK(loaded.refusedKeys.empty());
}

TEST_CASE("app/settings: every loader takes its section", "[app][settings]") {
    const UserData data;
    data.writeConfig("world.json", R"({"lightingProfile": "afternoon", "reduceMotion": false})");
    data.writeConfig("xr.json", R"({"comfort": {"turn": "smooth"}, "posture": "seated"})");
    data.writeConfig("player.json", R"({"mouseSensitivity": 2})");
    data.writeSettings(R"({"schemaVersion": 1,
        "world": {"lightingProfile": "evening"},
        "xr": {"comfort": {"turn": "snap", "vignette": 0.5}},
        "player": {"horizontalFovDeg": 90, "invertY": true}})");

    const rv::world::LoadedWorldConfig world = data.world();
    CHECK(world.config.lightingProfile == "evening");
    CHECK(world.belowSettings["lightingProfile"] == Json("afternoon"));
    CHECK(rv::world::roomLightingProfile(assetRoot(), data.config).name == "evening");

    const rv::xr::LoadedXrSettings xr = data.xr();
    CHECK(xr.settings.comfort.turn == rv::xr::TurnMode::Snap);
    CHECK(xr.settings.comfort.vignette == 0.5f);
    CHECK(xr.settings.posture == rv::xr::Posture::Seated); // the hand-written key the screen did not touch

    const rv::app::LoadedPlayerSettings player = data.player();
    CHECK(player.settings.horizontalFovDeg == 90.0);
    CHECK(player.settings.invertY);
    CHECK(player.settings.mouseSensitivity == 2.0);
}

TEST_CASE("app/settings: with no file every loader gives what it gave before", "[app][settings]") {
    const UserData data;
    CHECK(data.video().settings == rv::app::VideoSettings{});
    const rv::app::PlayerSettings player = data.player().settings;
    // FOV stays the scene's until the user sets it: no golden moves.
    CHECK_FALSE(player.horizontalFovDeg.has_value());
    CHECK(player == rv::app::PlayerSettings{});
    CHECK(data.world().config.lightingProfile == rv::world::kDefaultLightingProfile);
    CHECK_FALSE(data.world().config.reduceMotion);
    CHECK_FALSE(std::filesystem::exists(data.settings));
}

TEST_CASE("app/settings: a stray key never reaches a strict loader", "[app][settings]") {
    const UserData data;
    data.writeSettings(R"({"schemaVersion": 1,
        "video": {"stray": 1, "maxDecoders": 99, "audio": {"master": 0.5, "stray": true}},
        "world": {"seed": 123, "sharedRoomMinTitles": 0, "decorationDensityPermille": 1, "reduceMotion": true},
        "xr": {"stray": 1, "renderScale": 99, "comfort": {"vignette": 0.5, "snapDegrees": 9999}},
        "player": {"stray": 1, "invertY": true},
        "bindings": {"no_such_action": []}})");

    const rv::app::LoadedVideoSettings video = data.video();
    CHECK(video.issues.empty());
    CHECK(video.settings.audio.master == 0.5);
    CHECK(video.settings.maxDecoders == 2);

    const rv::world::LoadedWorldConfig world = data.world();
    CHECK(world.issues.empty());
    CHECK(world.config.reduceMotion);
    // No key that shapes a plan: the seed, the density and the threshold stay the hand-written files'.
    CHECK_FALSE(world.config.seed.has_value());
    CHECK(world.config.sharedRoomMinTitles == 30);
    CHECK(world.config.decorationDensityPermille != 1);

    const rv::xr::LoadedXrSettings xr = data.xr();
    CHECK(xr.issues.empty());
    CHECK(xr.settings.comfort.vignette == 0.5f);
    CHECK(xr.settings.renderScale == 1.0);

    const rv::app::LoadedPlayerSettings player = data.player();
    CHECK(player.issues.empty());
    CHECK(player.settings.invertY);
}

TEST_CASE("app/settings: a value a loader refuses costs that value alone, the user's file stays in force",
          "[app][settings]") {
    const UserData data;
    data.writeConfig("video.json", R"({"crtEffect": "vintage"})");
    data.writeSettings(R"({"schemaVersion": 1,
        "video": {"audio": {"master": "loud", "video": 0.5}, "crtEffect": 7},
        "world": {"reduceMotion": "yes", "lightingProfile": "afternoon"},
        "xr": {"quality": "best", "posture": "seated"},
        "player": {"horizontalFovDeg": 1000, "invertY": true}})");

    const rv::app::LoadedVideoSettings video = data.video();
    CHECK(video.issues.empty());
    CHECK(video.settings.audio.master == 1.0);
    CHECK(video.settings.audio.video == 0.5);
    CHECK(video.settings.crtEffect == rv::app::CrtEffect::Vintage);
    std::vector<std::string> refused = video.refusedKeys;
    std::sort(refused.begin(), refused.end());
    CHECK(refused == std::vector<std::string>{"audio.master", "crtEffect"});

    const rv::world::LoadedWorldConfig world = data.world();
    CHECK(world.config.lightingProfile == "afternoon");
    CHECK_FALSE(world.config.reduceMotion);
    CHECK(world.refusedKeys == std::vector<std::string>{"reduceMotion"});

    const rv::xr::LoadedXrSettings xr = data.xr();
    CHECK(xr.settings.posture == rv::xr::Posture::Seated);
    CHECK(xr.refusedKeys == std::vector<std::string>{"quality"});

    const rv::app::LoadedPlayerSettings player = data.player();
    CHECK_FALSE(player.settings.horizontalFovDeg.has_value());
    CHECK(player.settings.invertY);
}

TEST_CASE("app/settings: an unreadable settings.json leaves every loader on its layers", "[app][settings]") {
    const UserData data;
    data.writeConfig("video.json", R"({"audio": {"master": 0.5}})");
    data.writeSettings("{ not json");
    CHECK(data.video().settings.audio.master == 0.5);
    CHECK(data.video().issues.empty());
    CHECK(data.player().settings == rv::app::PlayerSettings{});
    CHECK(rv::core::readTextFile(data.settings).value() == "{ not json"); // only the app backs it up, once
}

TEST_CASE("app/settings: a user's file that does not load is left out alone; settings.json still applies",
          "[app][settings]") {
    const UserData data;
    data.writeConfig("video.json", R"({"crtEffect": "none such"})");
    data.writeSettings(R"({"schemaVersion": 1, "video": {"audio": {"master": 0.25}}})");
    const rv::app::LoadedVideoSettings loaded = data.video();
    CHECK(loaded.issues.size() == 1);
    CHECK(loaded.settings.crtEffect == rv::app::CrtEffect::Light);
    CHECK(loaded.settings.audio.master == 0.25);
}

TEST_CASE("app/settings: the store writes only the keys the user changed", "[app][settings]") {
    const UserData data;
    SettingsStore store(rv::core::SettingsFile::load(data.settings), data.layers(), rows());
    CHECK_FALSE(store.dirty());

    CHECK(store.set(rowOf(store, "video.audio.master"), 0.5));
    CHECK(store.set(rowOf(store, "player.invertY"), true));
    CHECK(store.dirty());
    store.saveAsync();
    store.flush();
    CHECK_FALSE(store.dirty());
    CHECK(store.writes() == 1);
    CHECK(store.writeFailures() == 0);

    CHECK(data.readSettings() == Json{{"schemaVersion", 1},
                                      {"video", {{"audio", {{"master", 0.5}}}}},
                                      {"player", {{"invertY", true}}}});
    // Nothing is written again when nothing changed.
    store.saveAsync();
    store.flush();
    CHECK(store.writes() == 1);
}

TEST_CASE("app/settings: a value equal to the layer below is no change", "[app][settings]") {
    const UserData data;
    data.writeConfig("video.json", R"({"audio": {"master": 0.5}})");
    SettingsStore store(rv::core::SettingsFile::load(data.settings), data.layers(), rows());
    const SettingRow& master = rowOf(store, "video.audio.master");
    CHECK_FALSE(store.set(master, 0.5)); // it shows already
    CHECK(store.set(master, 0.75));
    CHECK(store.overridden(master));
    CHECK(store.set(master, 0.5)); // back to the hand-written value: its key goes
    CHECK_FALSE(store.overridden(master));
    store.saveAsync();
    store.flush();
    CHECK(data.readSettings() == Json{{"schemaVersion", 1}});
}

TEST_CASE("app/settings: a value out of a row's range is refused", "[app][settings]") {
    const UserData data;
    SettingsStore store(rv::core::SettingsFile::load(data.settings), data.layers(), rows());
    CHECK_FALSE(store.set(rowOf(store, "video.audio.master"), 2.0));
    CHECK_FALSE(store.set(rowOf(store, "video.audio.master"), "loud"));
    CHECK_FALSE(store.set(rowOf(store, "player.horizontalFovDeg"), 120));
    CHECK(store.set(rowOf(store, "player.horizontalFovDeg"), 90));
    CHECK(store.value(rowOf(store, "player.horizontalFovDeg")) == Json(90));
    // Below 70: the scene's again.
    CHECK(store.set(rowOf(store, "player.horizontalFovDeg"), nullptr));
    CHECK_FALSE(store.overridden(rowOf(store, "player.horizontalFovDeg")));
}

TEST_CASE("app/settings: Ripristina lets the layer below show through", "[app][settings]") {
    const UserData data;
    data.writeConfig("video.json", R"({"audio": {"master": 0.5}})");
    data.writeSettings(R"({"schemaVersion": 1, "video": {"audio": {"master": 0.75, "video": 0.25}}, "future": 1,
                           "player": {"invertY": true}})");
    SettingsStore store(rv::core::SettingsFile::load(data.settings), data.layers(), rows());
    const SettingRow& master = rowOf(store, "video.audio.master");
    CHECK(store.value(master) == Json(0.75));
    CHECK(store.below(master) == Json(0.5));
    CHECK(store.restore(master));
    CHECK(store.value(master) == Json(0.5));
    CHECK_FALSE(store.restore(master));
    CHECK(store.document("video")["audio"]["video"] == Json(0.25));

    CHECK(store.restoreAll() == 2);
    store.saveAsync();
    store.flush();
    // Ripristina tutto removes the rows' keys alone: what no row names stays as read.
    CHECK(data.readSettings() == Json{{"schemaVersion", 1}, {"future", 1}});
}

TEST_CASE("app/settings: document() is what the loaders would give", "[app][settings]") {
    const UserData data;
    data.writeConfig("player.json", R"({"mouseSensitivity": 2})");
    SettingsStore store(rv::core::SettingsFile::load(data.settings), data.layers(), rows());
    CHECK(store.set(rowOf(store, "player.horizontalFovDeg"), 95));
    CHECK(store.set(rowOf(store, "video.audio.master"), 0.5));
    CHECK(store.set(rowOf(store, "xr.comfort.turn"), "smooth"));
    store.saveAsync();
    store.flush();

    const auto player = rv::app::parsePlayerSettings(store.document("player"), "test");
    REQUIRE(player.has_value());
    CHECK(*player == data.player().settings);
    const auto video = rv::app::parseVideoSettings(store.document("video"), "test");
    REQUIRE(video.has_value());
    CHECK(*video == data.video().settings);
    const auto xr = rv::xr::parseXrSettings(store.document("xr"), "test");
    REQUIRE(xr.has_value());
    CHECK(*xr == data.xr().settings);
}

TEST_CASE("app/settings: the user's config files keep their bytes, comments included", "[app][settings]") {
    const UserData data;
    constexpr std::string_view kVideo = R"(// my audio
{
  /* quieter at night */
  "audio": {"master": 0.5}
}
)";
    constexpr std::string_view kWorld = "// my world\n{\"seed\": 7}\n";
    constexpr std::string_view kXr = "// my headset\n{\"posture\": \"seated\"}\n";
    constexpr std::string_view kPlayer = "// me\n{\"invertY\": true}\n";
    constexpr std::string_view kBindings = "// keys\n{\"schemaVersion\": 1, \"bindings\": {}}\n";
    data.writeConfig("video.json", kVideo);
    data.writeConfig("world.json", kWorld);
    data.writeConfig("xr.json", kXr);
    data.writeConfig("player.json", kPlayer);
    data.writeConfig("bindings.json", kBindings);

    SettingsStore store(rv::core::SettingsFile::load(data.settings), data.layers(), rows());
    for (const SettingRow& row : store.rows()) {
        const Json value = store.value(row);
        const auto next = row.kind == rv::app::SettingKind::Toggle  ? Json(!value.get<bool>())
                          : row.kind == rv::app::SettingKind::Slider ? Json(row.max)
                                                                     : row.choices.back();
        static_cast<void>(store.set(row, next));
    }
    store.saveAsync();
    store.flush();
    CHECK(store.writeFailures() == 0);
    CHECK(std::filesystem::exists(data.settings));
    static_cast<void>(store.restoreAll());
    store.saveAsync();
    store.flush();

    CHECK(rv::core::readTextFile(data.config / "video.json").value() == kVideo);
    CHECK(rv::core::readTextFile(data.config / "world.json").value() == kWorld);
    CHECK(rv::core::readTextFile(data.config / "xr.json").value() == kXr);
    CHECK(rv::core::readTextFile(data.config / "player.json").value() == kPlayer);
    CHECK(rv::core::readTextFile(data.config / "bindings.json").value() == kBindings);
    // Nothing else appeared there.
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(data.config)) {
        static_cast<void>(entry);
        ++files;
    }
    CHECK(files == 5);
}

TEST_CASE("app/settings: a key shadowing a hand-written value is named", "[app][settings]") {
    const UserData data;
    data.writeConfig("video.json", R"({"audio": {"master": 0.5, "video": 0.25}})");
    data.writeSettings(R"({"schemaVersion": 1, "video": {"audio": {"master": 0.75, "video": 0.25}},
                           "player": {"invertY": true}})");
    const auto shadowed = rv::app::shadowedSettings(rv::core::SettingsFile::load(data.settings), data.config);
    CHECK(shadowed == std::vector<std::string>{"video.audio.master"});
}

TEST_CASE("app/settings: the bindings' section goes over the user's file, action by action", "[app][settings]") {
    const UserData data;
    rv::input::ActionMap map = rv::input::ActionMap::defaults();
    const auto interact = map.toJson()["bindings"]["interact"];
    data.writeSettings(R"({"schemaVersion": 1, "bindings": {
        "jump_nowhere": [],
        "ui_info": [{"key": "i"}],
        "ui_media": "not a list"}})");
    CHECK(rv::app::applySettingsBindings(map, rv::core::SettingsFile::load(data.settings)) == 1);
    const Json after = map.toJson()["bindings"];
    CHECK(after["ui_info"][0]["key"] == Json("i"));
    CHECK(after["interact"] == interact);
    CHECK(after["ui_media"] == rv::input::ActionMap::defaults().toJson()["bindings"]["ui_media"]);
}

TEST_CASE("app/setting_rows: every row names a key its loader takes, and none shapes a plan", "[app][settings]") {
    const auto contains = [](std::span<const std::string_view> keys, std::string_view key) {
        return std::find(keys.begin(), keys.end(), key) != keys.end();
    };
    std::vector<std::string> seen;
    for (const SettingRow& row : rows()) {
        INFO(row.id());
        if (row.section == "video") {
            CHECK(contains(rv::app::videoSettingKeys(), row.key));
        } else if (row.section == "world") {
            CHECK(contains(rv::world::worldSettingKeys(), row.key));
        } else if (row.section == "xr") {
            CHECK(contains(rv::xr::xrSettingKeys(), row.key));
        } else {
            CHECK(row.section == "player");
            CHECK(contains(rv::app::playerSettingKeys(), row.key));
        }
        CHECK(std::find(seen.begin(), seen.end(), row.id()) == seen.end());
        seen.push_back(row.id());
    }
    // ADR-0018 §2: only how the world is shown.
    CHECK(std::vector<std::string_view>(rv::world::worldSettingKeys().begin(), rv::world::worldSettingKeys().end()) ==
          std::vector<std::string_view>{"reduceMotion", "lightingProfile"});
}

TEST_CASE("app/setting_rows: the shipped values are among each row's", "[app][settings]") {
    const UserData data;
    SettingsStore store(rv::core::SettingsFile::load(data.settings), data.layers(), rows());
    for (const SettingRow& row : store.rows()) {
        INFO(row.id());
        CHECK(row.takes(store.below(row)));
    }
}

TEST_CASE("app/setting_rows: what a headset decides is hidden in one; the VR rows wait for a session", "[app][settings]") {
    std::vector<std::string> hidden;
    for (const SettingRow& row : rows()) {
        if (row.hiddenInVr) {
            hidden.push_back(row.id());
        }
        CHECK((row.group == rv::app::SettingGroup::Vr) == (row.apply == rv::app::SettingApply::NextSession));
    }
    CHECK(hidden == std::vector<std::string>{"player.displayMode", "player.horizontalFovDeg"});
}

TEST_CASE("app/player_settings: the shipped file holds every key at its default, a bad document is named",
          "[app][settings]") {
    auto shipped = rv::core::readJsonFile(assetRoot() / "config" / "player.json");
    REQUIRE(shipped.has_value());
    auto parsed = rv::app::parsePlayerSettings(*shipped, "player.json");
    REQUIRE(parsed.has_value());
    CHECK(*parsed == rv::app::PlayerSettings{});

    Json unknown = *shipped;
    unknown["stray"] = 1;
    CHECK(rv::app::parsePlayerSettings(unknown, "x").error().code == rv::ErrorCode::ParseError);
    Json version = *shipped;
    version["schemaVersion"] = 2;
    CHECK(rv::app::parsePlayerSettings(version, "x").error().code == rv::ErrorCode::Unsupported);
    Json range = *shipped;
    range["textScale"] = 9;
    CHECK(rv::app::parsePlayerSettings(range, "x").error().code == rv::ErrorCode::ValidationFailed);
    Json mode = *shipped;
    mode["displayMode"] = "exclusive"; // not offered: ADR-0018 §3
    CHECK(rv::app::parsePlayerSettings(mode, "x").error().code == rv::ErrorCode::ValidationFailed);
}
