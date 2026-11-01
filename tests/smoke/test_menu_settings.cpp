// The menu and the settings screen (I7 step 1, ADR-0018 §1, §2, §3, §12, §15), driven by the pad's actions alone through
// input scripts on the snes-room fixture imported into the case's data directory, read from the run report's "menu" and
// "settings" sections: Start and Back open the menu, Back walks out, Esci ends the run after its one confirmation; a
// volume and reduceMotion apply at once and survive a restart; the FOV changes the view; a lighting change commits the
// floor without moving anything; a broken settings.json is left out, kept and replaced; the user's config files keep
// their bytes; the panel in a headset, its FOV and window rows hidden, its ray clicking; no text cut at 125%.
// Every helper's frames are the room's: the menu's actions come after the lockstep run's room stands.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/world/world_store.hpp>

#include "image_checks.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

/// The frame the first press comes on: the room stands by then (a lockstep run waits for it), the frame time past.
constexpr int kFirstPress = 10;
/// Frames between two presses.
constexpr int kGap = 3;

constexpr glm::ivec2 kShotSize{1920, 1080};

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

/// The snes-room fixture recorded as the library of <case>/data and imported with retroverse-cli.
struct ImportedRoom {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit ImportedRoom(std::string_view name)
        : fixture(rv::test::Fixture::materialize("esde/snes-room").value()),
          dir(freshCaseDir(std::string("menu_settings/") + std::string(name))), data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
    }

    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }

    void writeSettings(std::string_view text) const {
        REQUIRE(rv::core::writeTextFileAtomic(data / "settings.json", std::string(text)).has_value());
    }

    [[nodiscard]] Json readSettings() const {
        auto text = rv::core::readTextFile(data / "settings.json");
        REQUIRE(text.has_value());
        return Json::parse(*text);
    }
};

/// A script of pad actions, one press every kGap frames from kFirstPress: each a name of input::Action.
struct Script {
    int frame = kFirstPress;
    Json steps = Json::array();

    Script& press(std::string_view action, int times = 1) {
        for (int index = 0; index < times; ++index) {
            steps.push_back(Json{{"fromFrame", frame}, {"toFrame", frame}, {"actions", {{std::string(action), 1}}}});
            frame += kGap;
        }
        return *this;
    }
    Script& wait(int frames) {
        frame += frames;
        return *this;
    }
    /// From the world into the settings screen, on its first group.
    Script& openSettings() { return press("menu").press("ui_down").press("ui_activate"); }

    void write(const fs::path& file) const {
        REQUIRE(rv::core::writeTextFileAtomic(file, steps.dump(2) + "\n").has_value());
    }
};

std::vector<std::string> roomArguments(int frames, const fs::path& shot, const fs::path& script) {
    return {"--start-room",   "room:snes", "--exit-after-frames", "--frames",       std::to_string(frames),
            "--fixed-dt",     "0.016667",  "--screenshot",        arg(shot),        "--input-script",
            arg(script)};
}

const rv::app::SettingValueRecord& rowOf(const rv::app::RunReport& report, std::string_view key) {
    REQUIRE(report.settings);
    const auto found = std::find_if(report.settings->rows.begin(), report.settings->rows.end(),
                                    [&](const auto& row) { return row.key == key; });
    REQUIRE(found != report.settings->rows.end());
    return *found;
}

std::vector<std::string> menuEvents(const rv::app::RunReport& report) {
    REQUIRE(report.menu);
    std::vector<std::string> events;
    for (const rv::app::MenuEventRecord& event : report.menu->events) {
        events.push_back(event.detail.empty() ? event.event : event.event + ":" + event.detail);
    }
    return events;
}

} // namespace

TEST_CASE("menu: Start and Back open it, Back walks out to the world, the world goes on", "[gpu][menu_settings]") {
    const ImportedRoom room("back");
    Script script;
    script.press("menu").wait(6).press("ui_back");         // Start opens it, B closes it
    script.press("back").wait(6);                          // B in free roam opens it: it no longer quits
    script.press("ui_down").press("ui_activate").wait(6);  // Impostazioni
    script.press("ui_back").wait(6).press("ui_back");      // to the menu, to the world
    script.press("move_forward");
    const fs::path file = room.dir / "script.json";
    script.write(file);
    const int frames = script.frame + 20;
    const AppRun run = runApp(retroverseExe(), room.dir, "run", roomArguments(frames, room.dir / "end.png", file));
    skipWithoutOpenGl(run);
    requireCleanRun(run); // every frame rendered: Back never ended the run
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    REQUIRE(report.menu);
    CHECK(report.menu->opened == 2);
    CHECK(report.menu->settingsOpened == 1);
    CHECK_FALSE(report.menu->open);
    CHECK_FALSE(report.menu->quit);
    CHECK(menuEvents(report) == std::vector<std::string>{"open:start", "closed", "open:back", "settings",
                                                         "settings-closed", "closed"});
    CHECK(report.menu->textCut == 0);
    CHECK(report.menu->settingsTextCut == 0);
    // Nothing was changed: nothing was written.
    CHECK_FALSE(fs::exists(room.data / "settings.json"));
    REQUIRE(report.settings);
    CHECK(report.settings->writes == 0);
}

TEST_CASE("menu: Esci asks once, then ends the run", "[gpu][menu_settings]") {
    const ImportedRoom room("quit");
    Script script;
    script.press("menu").press("ui_down", 2);
    script.press("ui_activate"); // Esci: the confirmation, Annulla focused
    script.press("ui_activate"); // Annulla: the menu again
    script.press("ui_activate"); // Esci again
    script.press("ui_down").press("ui_activate"); // Esci, confirmed
    const fs::path file = room.dir / "script.json";
    script.write(file);
    // No --screenshot: the run ends before its last frame.
    const AppRun run = runApp(retroverseExe(), room.dir, "run",
                              {"--start-room", "room:snes", "--exit-after-frames", "--frames", "400", "--fixed-dt",
                               "0.016667", "--input-script", arg(file)});
    skipWithoutOpenGl(run);
    INFO(run.describe());
    REQUIRE(run.exitCode == 0);
    REQUIRE(run.report);
    CHECK(run.report->framesRendered < 400);
    REQUIRE(run.report->menu);
    CHECK(run.report->menu->quit);
    CHECK(menuEvents(*run.report) == std::vector<std::string>{"open:start", "quit"});
}

TEST_CASE("menu: nothing opens it in Focus; Back leaves Focus first", "[gpu][menu_settings]") {
    // Start in free roam only: a press while the quick catalog shows opens nothing either.
    const ImportedRoom room("over_catalog");
    Script script;
    script.press("quick_catalog").wait(6).press("menu").wait(6).press("ui_back");
    const fs::path file = room.dir / "script.json";
    script.write(file);
    const AppRun run = runApp(retroverseExe(), room.dir, "run",
                              roomArguments(script.frame + 10, room.dir / "end.png", file));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->menu);
    CHECK(run.report->menu->opened == 0);
    REQUIRE(run.report->catalog);
    CHECK_FALSE(run.report->catalog->open);
}

TEST_CASE("settings: a volume and reduceMotion change at once and survive a restart; config keeps its bytes",
          "[gpu][menu_settings]") {
    const ImportedRoom room("restart");
    constexpr std::string_view kVideo = "// mine\n{\n  /* hand-written */\n  \"audio\": {\"video\": 0.5}\n}\n";
    constexpr std::string_view kWorld = "// mine too\n{\"sharedRoomMinTitles\": 30}\n";
    fs::create_directories(room.data / "config");
    REQUIRE(rv::core::writeTextFileAtomic(room.data / "config" / "video.json", std::string(kVideo)).has_value());
    REQUIRE(rv::core::writeTextFileAtomic(room.data / "config" / "world.json", std::string(kWorld)).has_value());

    Script script;
    script.openSettings();
    script.press("ui_filter_next").press("ui_left", 2); // Audio: the general volume down two steps
    script.press("ui_filter_next").press("ui_down").press("ui_activate"); // Mondo: reduceMotion on
    const fs::path file = room.dir / "script.json";
    script.write(file);
    // The run ends with the screen still open: what it says was in force before anything closed it.
    const AppRun run = runApp(retroverseExe(), room.dir, "change",
                              roomArguments(script.frame + 4, room.dir / "change.png", file));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->settings);
    const rv::app::SettingsRecord& settings = *run.report->settings;
    CHECK(settings.masterGain == 0.9 * 0.5);
    CHECK(settings.reduceMotion);
    CHECK(settings.changes.size() == 3);
    CHECK(settings.writeFailures == 0);
    CHECK(run.report->menu->settingsOpen);
    CHECK(run.report->menu->settingsGroup == "world");
    CHECK(run.report->menu->settingsTextCut == 0);

    // Only what changed; the hand-written files as they were.
    const Json written = room.readSettings();
    CHECK(written == Json{{"schemaVersion", 1},
                          {"video", {{"audio", {{"master", 0.9}}}}},
                          {"world", {{"reduceMotion", true}}}});
    CHECK(rv::core::readTextFile(room.data / "config" / "video.json").value() == kVideo);
    CHECK(rv::core::readTextFile(room.data / "config" / "world.json").value() == kWorld);

    // A restart, no script: in force from its first frame.
    const AppRun again = runApp(retroverseExe(), room.dir, "again",
                                {"--start-room", "room:snes", "--exit-after-frames", "--frames", "5", "--fixed-dt",
                                 "0.016667", "--screenshot", arg(room.dir / "again.png")});
    requireCleanRun(again);
    REQUIRE(again.report->settings);
    CHECK(again.report->settings->masterGain == 0.9 * 0.5);
    CHECK(again.report->settings->reduceMotion);
    CHECK(rowOf(*again.report, "video.audio.master").overridden);
    CHECK(rowOf(*again.report, "video.audio.video").value == "0.5");
    CHECK_FALSE(rowOf(*again.report, "video.audio.video").overridden);
    CHECK(again.report->settings->writes == 0);
}

TEST_CASE("settings: F8's mute stays through another row, and only the mute row sets it", "[gpu][menu_settings]") {
    const ImportedRoom room("mute");
    Script script;
    script.press("toggle_mute").wait(4); // F8: this run alone
    script.openSettings().press("ui_filter_next").press("ui_left").press("ui_down").press("ui_down");
    const fs::path file = room.dir / "script.json";
    script.write(file);
    const AppRun run = runApp(retroverseExe(), room.dir, "run",
                              roomArguments(script.frame + 4, room.dir / "end.png", file));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->audio);
    CHECK(run.report->audio->muteToggles == 1);
    CHECK(run.report->audio->muted); // the volume's change left it
    CHECK(run.report->settings->masterGain == 0.95);
    // The mute row itself was not touched, so nothing of F8 reached the file.
    CHECK_FALSE(rowOf(*run.report, "video.audio.muted").overridden);
}

TEST_CASE("settings: only the display mode's row reaches the window", "[gpu][menu_settings]") {
    const ImportedRoom room("display_mode");
    Script script;
    script.openSettings();
    script.press("ui_down", 2).press("ui_right", 2); // Campo visivo: not the window's
    script.press("ui_filter_next", 3).press("ui_right"); // Controlli: the mouse's sensitivity: nor this
    const fs::path file = room.dir / "script.json";
    script.write(file);
    const AppRun other = runApp(retroverseExe(), room.dir, "other",
                                roomArguments(script.frame + 4, room.dir / "other.png", file));
    skipWithoutOpenGl(other);
    requireCleanRun(other);
    INFO(other.describe());
    CHECK(other.report->settings->changes.size() == 3);
    CHECK(other.report->settings->displayModeApplied == 0);

    Script own;
    own.openSettings().press("ui_right"); // Visualizzazione: schermo intero; a hidden window stays as it is
    const fs::path ownFile = room.dir / "own.json";
    own.write(ownFile);
    const AppRun run = runApp(retroverseExe(), room.dir, "own",
                              roomArguments(own.frame + 4, room.dir / "own.png", ownFile));
    requireCleanRun(run);
    INFO(run.describe());
    CHECK(run.report->settings->displayModeApplied == 1);
    CHECK(run.report->settings->displayMode == "borderless");
    CHECK_FALSE(run.report->settings->windowFullscreen);
}

TEST_CASE("settings: Ripristina gives the layer below back", "[gpu][menu_settings]") {
    const ImportedRoom room("restore");
    room.writeSettings(R"({"schemaVersion": 1, "future": {"kept": true}, "video": {"audio": {"master": 0.5}}})");
    Script script;
    script.openSettings();
    script.press("ui_filter_next").press("ui_info").press("ui_back"); // Audio: X on the general volume; B closes
    const fs::path file = room.dir / "script.json";
    script.write(file);
    const AppRun run = runApp(retroverseExe(), room.dir, "run",
                              roomArguments(script.frame + 10, room.dir / "end.png", file));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    CHECK(run.report->settings->masterGain == 1.0);
    // The key is gone, the section with it; what this build does not know stays as read.
    CHECK(room.readSettings() == Json{{"schemaVersion", 1}, {"future", {{"kept", true}}}});
}

TEST_CASE("settings: the FOV changes the view; the scene's lens stays until it is set", "[gpu][menu_settings]") {
    const ImportedRoom room("fov");
    Script closed;
    closed.openSettings().press("ui_back").press("ui_back");
    const fs::path untouched = room.dir / "untouched.json";
    closed.write(untouched);
    const fs::path before = room.dir / "before.png";
    const AppRun plain = runApp(retroverseExe(), room.dir, "before", roomArguments(closed.frame + 10, before, untouched));
    skipWithoutOpenGl(plain);
    requireCleanRun(plain);
    INFO(plain.describe());
    const float sceneFov = static_cast<float>(plain.report->settings->lensFovDeg);

    Script script;
    script.openSettings();
    script.press("ui_down", 2).press("ui_right", 5); // Campo visivo: from the scene's to 90 degrees
    script.press("ui_back").press("ui_back");
    const fs::path file = room.dir / "script.json";
    script.write(file);
    const fs::path after = room.dir / "after.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "after", roomArguments(script.frame + 10, after, file));
    requireCleanRun(run);
    INFO(run.describe());
    CHECK(run.report->settings->lensFovDeg == 90.0);
    CHECK(sceneFov != 90.0f);
    const rv::assets::ImageData first = requireScreenshot(plain, before, kShotSize);
    const rv::assets::ImageData second = requireScreenshot(run, after, kShotSize);
    REQUIRE(first.pixels.size() == second.pixels.size());
    CHECK(first.pixels != second.pixels);
}

TEST_CASE("settings: a lighting change commits the floor again, nothing moved", "[gpu][menu_settings]") {
    const ImportedRoom room("lighting");
    Script first;
    first.press("menu").press("ui_back").wait(150);
    const fs::path plainScript = room.dir / "plain.json";
    first.write(plainScript);
    const fs::path before = room.dir / "before.png";
    const AppRun plain = runApp(retroverseExe(), room.dir, "before", roomArguments(first.frame + 40, before, plainScript));
    skipWithoutOpenGl(plain);
    requireCleanRun(plain);
    INFO(plain.describe());
    const fs::path planFile =
        rv::world::worldFiles(room.data, rv::catalog::LibraryId::parse(plain.report->world.library).value()).plan;
    const std::string plan = rv::core::readTextFile(planFile, 1u << 30).value();

    Script script;
    script.openSettings();
    script.press("ui_filter_next", 2).press("ui_left"); // Mondo: from evening to afternoon
    script.press("ui_back").press("ui_back");
    script.frame = first.frame; // the same last view
    const fs::path file = room.dir / "script.json";
    script.write(file);
    const fs::path after = room.dir / "after.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "after", roomArguments(first.frame + 40, after, file));
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->world.liveUpdates.size() == 1);
    const rv::app::WorldLiveUpdateRecord& update = run.report->world.liveUpdates.front();
    CHECK(update.trigger == "look");
    CHECK(update.kind == "rebuild");
    CHECK(update.moved == 0);
    CHECK(update.added == 0);
    CHECK(update.removed == 0);
#if defined(NDEBUG)
    // The save row of the acceptable limits.
    REQUIRE(update.requestToApplyMs);
    CHECK(*update.requestToApplyMs <= 3000.0);
#endif
    CHECK(rv::core::readTextFile(planFile, 1u << 30).value() == plan);
    CHECK(room.readSettings()["world"]["lightingProfile"] == Json("afternoon"));
    const rv::assets::ImageData first_ = requireScreenshot(plain, before, kShotSize);
    const rv::assets::ImageData second = requireScreenshot(run, after, kShotSize);
    REQUIRE(first_.pixels.size() == second.pixels.size());
    CHECK(first_.pixels != second.pixels);
}

TEST_CASE("settings: a broken settings.json is left out, kept aside and replaced at the next write",
          "[gpu][menu_settings]") {
    const ImportedRoom room("broken");
    constexpr std::string_view kBroken = "{\"schemaVersion\": 1, \"video\": ";
    room.writeSettings(kBroken);
    Script script;
    script.openSettings();
    script.press("ui_filter_next").press("ui_left").press("ui_back").press("ui_back");
    const fs::path file = room.dir / "script.json";
    script.write(file);
    const AppRun run = runApp(retroverseExe(), room.dir, "run",
                              roomArguments(script.frame + 10, room.dir / "end.png", file));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->settings);
    CHECK(run.report->settings->unreadable);
    CHECK(run.report->settings->backedUp);
    CHECK(room.readSettings()["video"]["audio"]["master"] == Json(0.95));
    bool kept = false;
    for (const auto& entry : fs::directory_iterator(room.data / "backups")) {
        kept = kept || rv::core::readTextFile(entry.path()).value() == kBroken;
    }
    CHECK(kept);
}

TEST_CASE("settings: a stray key and a hand-written key shadowed are reported, the run unharmed", "[gpu][menu_settings]") {
    const ImportedRoom room("stray");
    fs::create_directories(room.data / "config");
    REQUIRE(rv::core::writeTextFileAtomic(room.data / "config" / "video.json", R"({"audio": {"master": 0.25}})")
                .has_value());
    room.writeSettings(R"({"schemaVersion": 1, "video": {"maxDecoders": 77, "audio": {"master": 0.5, "video": "loud"}}})");
    const AppRun run = runApp(retroverseExe(), room.dir, "run",
                              {"--start-room", "room:snes", "--exit-after-frames", "--frames", "5", "--fixed-dt",
                               "0.016667", "--screenshot", arg(room.dir / "end.png")});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->settings);
    CHECK(run.report->settings->masterGain == 0.5);
    CHECK(run.report->settings->refused == std::vector<std::string>{"video.audio.video"});
    CHECK(run.report->settings->shadowed == std::vector<std::string>{"video.audio.master"});
    CHECK_FALSE(run.report->settings->unreadable);
}

TEST_CASE("settings: at text scale 125% at 1280 x 720 no row, button or hint is cut", "[gpu][menu_settings]") {
    const ImportedRoom room("text_scale");
    room.writeSettings(R"({"schemaVersion": 1, "player": {"textScale": 1.25}})");
    Script script;
    script.press("menu").wait(6);
    script.press("ui_down").press("ui_activate").wait(4); // the settings screen, then every group
    script.press("ui_filter_next", 4).wait(4).press("ui_down", 7).wait(4);
    const fs::path file = room.dir / "script.json";
    script.write(file);
    const AppRun run = runApp(retroverseExe(), room.dir, "run",
                              roomArguments(script.frame + 4, room.dir / "end.png", file));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    CHECK(run.report->settings->textScale == 1.25);
    REQUIRE(run.report->menu);
    CHECK(run.report->menu->textCut == 0);
    CHECK(run.report->menu->settingsTextCut == 0);
}

TEST_CASE("settings: in a headset the FOV and window rows are hidden and the ray clicks", "[gpu][menu_settings][xr]") {
    const fs::path dir = freshCaseDir("menu_settings/vr");
    // The right controller at the hip points at Impostazioni, the middle of the panel's width, above its middle (the
    // panel stands 1.3 m in front of the eyes, 0.12 m below them, 1.8 m wide at the window's 16:9).
    const glm::vec3 hand(0.2f, 1.25f, -0.3f);
    const glm::vec3 target(0.0f, 1.65f - 0.12f + 0.10f, -1.3f);
    const glm::vec3 toTarget = glm::normalize(target - hand);
    const float yawDeg = rv::core::radiansToDegrees(std::atan2(toTarget.x, -toTarget.z));
    const float pitchDeg = rv::core::radiansToDegrees(std::asin(toTarget.y));
    const std::string pose = fmt::format(R"("rightHand": {{"yaw": {}, "pitch": {}, "position": [{}, {}, {}]}})", yawDeg,
                                         pitchDeg, hand.x, hand.y, hand.z);
    const fs::path script = dir / "vr.json";
    REQUIRE(rv::core::writeTextFileAtomic(script, fmt::format(R"([
  {{"fromFrame": 0, "toFrame": 59, "headPose": {{"position": [0, 1.65, 0]}}}},
  {{"fromFrame": 5, "toFrame": 5, "actions": {{"menu": 1}}}},
  {{"fromFrame": 10, "toFrame": 59, {0}}},
  {{"fromFrame": 30, "toFrame": 33, "rightSelect": 1}}
])",
                                                              pose))
                .has_value());
    const fs::path shot = dir / "eyes.png";
    const AppRun run = runApp(retroverseExe(), dir, "run",
                              {"--scene", "m0_testroom", "--vr", "--xr-runtime", "fake", "--exit-after-frames",
                               "--frames", "60", "--fixed-dt", "0.016667", "--screenshot", arg(shot),
                               "--input-script", arg(script)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->xr);
    CHECK(run.report->xr->uiClicks == 1);
    REQUIRE(run.report->menu);
    CHECK(run.report->menu->settingsOpened == 1);
    CHECK(run.report->menu->settingsOpen);
    const std::vector<std::string>& rows = run.report->menu->settingsRows;
    CHECK(std::find(rows.begin(), rows.end(), "player.horizontalFovDeg") == rows.end());
    CHECK(std::find(rows.begin(), rows.end(), "player.displayMode") == rows.end());
    CHECK(std::find(rows.begin(), rows.end(), "player.textScale") != rows.end());
    CHECK(std::find(rows.begin(), rows.end(), "xr.comfort.turn") != rows.end());
}

// By hand, for a look: rv_smoke_tests "[shots]" with RV_SHOTS_DIR naming a folder writes the screens, hidden and
// frame-limited, the pad's actions alone. Not a check: visual verification (docs/reference/visual-verification.md).
TEST_CASE("menu: the screens' pictures", "[.shots]") {
    const char* folder = std::getenv("RV_SHOTS_DIR");
    if (folder == nullptr) {
        SKIP("RV_SHOTS_DIR names no folder");
    }
    const fs::path out = rv::core::pathFromUtf8(folder);
    fs::create_directories(out);
    const ImportedRoom room("shots");
    const auto shoot = [&](std::string_view name, const Script& script) {
        const fs::path file = room.dir / (std::string(name) + ".json");
        script.write(file);
        const AppRun run = runApp(retroverseExe(), room.dir, name,
                                  roomArguments(script.frame + 6, out / (std::string(name) + ".png"), file));
        requireCleanRun(run);
        INFO(run.describe());
        REQUIRE(run.report->menu);
        CHECK(run.report->menu->textCut == 0);
        CHECK(run.report->menu->settingsTextCut == 0);
    };
    const auto all = [&](std::string_view prefix) {
        const std::string name(prefix);
        shoot(name + "menu", Script{}.press("menu").wait(6));
        shoot(name + "quit_confirmation", Script{}.press("menu").press("ui_down", 2).press("ui_activate").wait(4));
        const std::vector<std::string_view> groups = {"display", "audio", "world", "controls", "vr"};
        for (std::size_t group = 0; group < groups.size(); ++group) {
            Script script;
            script.openSettings().press("ui_filter_next", static_cast<int>(group)).wait(4);
            shoot(name + "settings_" + std::string(groups[group]), script);
        }
    };
    all("");
    shoot("fov_before", Script{}.press("menu").press("ui_back").wait(10));
    shoot("fov_row", Script{}.openSettings().press("ui_down", 2).press("ui_right", 7).wait(4));
    shoot("fov_after", Script{}.openSettings().press("ui_down", 2).press("ui_back").press("ui_back").wait(10));
    REQUIRE(fs::remove(room.data / "settings.json"));
    room.writeSettings(R"({"schemaVersion": 1, "player": {"textScale": 1.25}})");
    all("scale125_");
    shoot("scale125_settings_restore_all", Script{}.openSettings().press("ui_filter_next", 2).press("ui_down", 6).wait(4));
    shoot("scale125_catalog", Script{}.press("quick_catalog").wait(20));
    shoot("scale125_directory", Script{}.press("map").wait(20));
}
