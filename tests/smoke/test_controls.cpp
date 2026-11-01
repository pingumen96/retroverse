// Controls (I7 step 3, ADR-0018 §7, §12, §15), on the snes-room fixture imported into the case's data directory, read from
// the run report's "controls" section: the hints name the active map on the keyboard and on the pad; a rebound activate
// works through the bindings (the script's "raw" goes through them, unlike its "actions"), by a press a capture read; a
// clash swaps; the very press a capture took presses nothing; a hand-written file cannot lock anybody out;
// --safe-bindings; "Ripristina predefiniti"; "Controller scollegato"; the hints whole at 125% at 1280 x 720. No real input:
// everything comes from input scripts.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr int kFirstPress = 10;
constexpr int kGap = 3;
/// In Controlli, past the three rows of the look and the world's bindings.
constexpr int kRowUiActivate = 14;
constexpr std::string_view kSwappedActivate = R"({"schemaVersion": 1, "bindings": {
  "ui_activate": [{"key": "insert"}, {"padButton": "south"}]}})";

fs::path cliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

struct Room {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit Room(std::string_view name)
        : fixture(rv::test::Fixture::materialize("esde/snes-room").value()),
          dir(freshCaseDir(std::string("controls/") + std::string(name))), data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
    }

    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(cliExe(), dir, std::string("cli_") + std::string(name), arguments);
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

/// A script: a press every kGap frames from kFirstPress, of an action, or of a key or a pad's button on the raw input.
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
    Script& key(std::string_view name, int held = 1) {
        steps.push_back(Json{{"fromFrame", frame},
                             {"toFrame", frame + held - 1},
                             {"raw", {{"keys", Json::array({std::string(name)})}}}});
        frame += held - 1 + kGap;
        return *this;
    }
    Script& pad(std::string_view name) {
        steps.push_back(Json{{"fromFrame", frame},
                             {"toFrame", frame},
                             {"raw", {{"padButtons", Json::array({std::string(name)})}}}});
        frame += kGap;
        return *this;
    }
    Script& wait(int frames) {
        frame += frames;
        return *this;
    }
    Script& device(std::string_view name) {
        steps.push_back(Json{{"fromFrame", frame}, {"toFrame", frame}, {"device", std::string(name)}});
        return *this;
    }
    /// A pad plugged in from the first frame to `last`.
    Script& padPlugged(int last) {
        steps.push_back(Json{{"fromFrame", 0}, {"toFrame", last}, {"raw", {{"padConnected", true}}}});
        return *this;
    }
    /// From the world to UiActivate's row of Controlli.
    Script& toActivateRow() {
        press("menu").press("ui_down").press("ui_activate");
        return press("ui_filter_next", 3).press("ui_down", kRowUiActivate);
    }

    void write(const fs::path& file) const {
        REQUIRE(rv::core::writeTextFileAtomic(file, steps.dump(2) + "\n").has_value());
    }
};

AppRun runScript(const Room& room, std::string_view name, const Script& script,
                 const std::vector<std::string>& more = {}) {
    const fs::path file = room.dir / (std::string(name) + ".json");
    script.write(file);
    std::vector<std::string> arguments = {
        "--start-room", "room:snes", "--exit-after-frames",  "--frames",      std::to_string(script.frame + 6),
        "--fixed-dt",   "0.016667",  "--screenshot",         arg(room.dir / (std::string(name) + ".png")),
        "--input-script", arg(file)};
    arguments.insert(arguments.end(), more.begin(), more.end());
    return runApp(retroverseExe(), room.dir, name, arguments);
}

std::string hintOf(const rv::app::RunReport& report, std::string_view actions) {
    REQUIRE(report.controls);
    const auto found = std::find_if(report.controls->hints.begin(), report.controls->hints.end(),
                                    [&](const rv::app::HintRecord& hint) { return hint.actions == actions; });
    return found != report.controls->hints.end() ? found->text : std::string();
}

bool bound(const rv::app::RunReport& report, std::string_view line) {
    REQUIRE(report.controls);
    const std::vector<std::string>& lines = report.controls->bindings;
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

} // namespace

TEST_CASE("controls: the hints name the active map, the keyboard's and the pad's", "[gpu][controls]") {
    const Room room("hints");
    Script keys;
    keys.key("tab").wait(20);
    const AppRun onKeys = runScript(room, "keys", keys);
    skipWithoutOpenGl(onKeys);
    requireCleanRun(onKeys);
    INFO(onKeys.describe());
    CHECK(onKeys.report->controls->device == "keys");
    CHECK(hintOf(*onKeys.report, "ui_activate") == "Invio");
    CHECK(hintOf(*onKeys.report, "ui_media") == "F6");
    CHECK(hintOf(*onKeys.report, "ui_info") == "F4");
    CHECK(hintOf(*onKeys.report, "ui_back") == "Esc");

    Script pad;
    pad.padPlugged(200);
    pad.pad("north").wait(20); // Y opens the catalog, and makes the pad the device used last
    const AppRun onPad = runScript(room, "pad", pad);
    requireCleanRun(onPad);
    INFO(onPad.describe());
    CHECK(onPad.report->controls->device == "pad");
    CHECK(hintOf(*onPad.report, "ui_activate") == "A");
    CHECK(hintOf(*onPad.report, "ui_media") == "Y");
    CHECK(hintOf(*onPad.report, "ui_info") == "X");
    CHECK(hintOf(*onPad.report, "ui_back") == "B");
}

TEST_CASE("controls: a rebound activate works, and the hints name it, on the keyboard", "[gpu][controls]") {
    const Room room("rebound_keys");
    Script script;
    script.toActivateRow();
    script.press("ui_activate").wait(2); // the capture
    script.key("insert", 4);             // held: the press it took presses nothing under the new map
    script.key("f9");                    // no capture runs: the key is no command at all
    script.key("enter");                 // the old key no longer activates
    script.key("f7");                    // nor is anything capturing it
    script.key("insert").wait(2);        // the new one starts a capture ...
    script.key("insert").wait(4);        // ... and the same press ends it
    const AppRun run = runScript(room, "run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    CHECK(run.report->controls->captures == std::vector<std::string>{"free", "same"});
    CHECK(bound(*run.report, "ui_activate=Ins|A"));
    CHECK(run.report->controls->device == "keys");
    CHECK(hintOf(*run.report, "ui_activate") == "Ins");
    CHECK(run.report->menu->settingsOpen); // Enter closed nothing
}

TEST_CASE("controls: on the pad a clash offers a swap; the rebound button works and the hints name it",
          "[gpu][controls]") {
    const Room room("rebound_pad");
    Script script;
    script.padPlugged(300);
    script.toActivateRow();
    script.press("ui_activate").wait(2);
    script.pad("west").wait(2); // X is Informazioni's: the swap is offered
    script.press("ui_activate").wait(2); // taken
    script.pad("west").wait(2); // X starts a capture ...
    script.pad("west").wait(4); // ... and the same press ends it
    const AppRun run = runScript(room, "run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    CHECK(run.report->controls->captures == std::vector<std::string>{"clash", "same"});
    CHECK(bound(*run.report, "ui_activate=Invio|X"));
    CHECK(bound(*run.report, "ui_info=F4|A"));
    CHECK(run.report->controls->device == "pad");
    CHECK(hintOf(*run.report, "ui_activate") == "X");
    CHECK(hintOf(*run.report, "ui_info") == "A");

    // It survives a restart, in settings.json alone: both rows, nothing else.
    Script closing;
    closing.padPlugged(300).toActivateRow();
    closing.press("ui_activate").wait(2).pad("west").wait(2).press("ui_activate").wait(2);
    closing.press("ui_back").press("ui_back").wait(10);
    requireCleanRun(runScript(room, "closing", closing));
    const Json held = room.readSettings();
    REQUIRE(held.contains("bindings"));
    CHECK(held["bindings"].size() == 2);
    CHECK(held["bindings"].contains("ui_activate"));
    CHECK(held["bindings"].contains("ui_info"));
    Script next;
    next.padPlugged(100).press("menu").wait(10);
    const AppRun again = runScript(room, "again", next);
    requireCleanRun(again);
    CHECK(bound(*again.report, "ui_activate=Invio|X"));
}

TEST_CASE("controls: a reserved press is refused and the capture goes on; time ends it", "[gpu][controls]") {
    const Room room("reserved");
    Script script;
    script.toActivateRow();
    script.press("ui_activate").wait(2);
    script.key("k").wait(2); // a text field's
    script.key("f1").wait(2); // the developer's
    script.wait(360);        // 5 s of the fixed step
    script.key("f7").wait(4); // no capture runs any more
    const AppRun run = runScript(room, "run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    CHECK(run.report->controls->captures == std::vector<std::string>{"reserved", "reserved"});
    CHECK(bound(*run.report, "ui_activate=Invio|A"));
}

TEST_CASE("controls: the capture's strip leaves the row in view, a refusal's line too", "[gpu][controls]") {
    const Room room("strip");
    const auto check = [&](std::string_view name, Script script) {
        const AppRun run = runScript(room, name, script);
        skipWithoutOpenGl(run);
        requireCleanRun(run);
        INFO(name << ": " << run.describe());
        CHECK(run.report->menu->settingsOpen);
        CHECK_FALSE(run.report->menu->settingsRowCovered);
    };
    check("waiting", Script{}.toActivateRow().press("ui_activate").wait(4));
    check("refused", Script{}.toActivateRow().press("ui_activate").wait(2).key("k").wait(4));
    check("offer", Script{}.toActivateRow().press("ui_activate").wait(2).key("escape").wait(4));
}

TEST_CASE("controls: a bad entry of settings.json costs that action alone, a live rebind included", "[gpu][controls]") {
    const Room room("bad_entry");
    room.writeSettings(R"({"schemaVersion": 1, "bindings": {"sprint": [{"key": "bogus"}], "defaults": true,
        "ui_info": [{"key": "f7"}, {"padButton": "west"}]}})");
    Script script;
    script.toActivateRow();
    script.press("ui_activate").wait(2).key("insert").wait(4);
    const AppRun run = runScript(room, "run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    CHECK(run.report->controls->captures == std::vector<std::string>{"free"});
    CHECK(bound(*run.report, "ui_activate=Ins|A"));
    CHECK(bound(*run.report, "ui_info=F7|X")); // the live map kept it
    CHECK(bound(*run.report, "sprint=Maiusc|L3"));
}

TEST_CASE("controls: a hand-written file cannot lock anybody out, nor is it touched", "[gpu][controls]") {
    const Room room("lock_out");
    constexpr std::string_view kWritten = R"(// mine
{"schemaVersion": 1, "bindings": {"ui_back": [], "menu": [{"padButton": "back"}], "ui_media": [{"key": "f7"}]}}
)";
    fs::create_directories(room.data / "config");
    REQUIRE(rv::core::writeTextFileAtomic(room.data / "config" / "bindings.json", std::string(kWritten)).has_value());
    Script script;
    script.press("menu").wait(10);
    const AppRun run = runScript(room, "run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    CHECK(bound(*run.report, "ui_back=Esc|B"));    // unbound by hand: both back
    CHECK(bound(*run.report, "menu=F10|Select"));  // its button stays the file's, its key comes back
    CHECK(bound(*run.report, "ui_media=F7|"));     // not locked: as written
    auto bytes = rv::core::readTextFile(room.data / "config" / "bindings.json");
    REQUIRE(bytes.has_value());
    CHECK(*bytes == kWritten);
}

TEST_CASE("controls: --safe-bindings leaves the section out; Ripristina predefiniti clears it", "[gpu][controls]") {
    const Room room("safe");
    room.writeSettings(kSwappedActivate);
    const std::string written = *rv::core::readTextFile(room.data / "settings.json");

    Script look;
    look.press("menu").wait(10);
    const AppRun remapped = runScript(room, "remapped", look);
    skipWithoutOpenGl(remapped);
    requireCleanRun(remapped);
    CHECK_FALSE(remapped.report->controls->safeBindings);
    CHECK(bound(*remapped.report, "ui_activate=Ins|A"));

    // The section is ignored, the file left as it is, the rows gone but for the way out.
    const AppRun safe = runScript(room, "safe", look, {"--safe-bindings"});
    requireCleanRun(safe);
    INFO(safe.describe());
    CHECK(safe.report->controls->safeBindings);
    CHECK(bound(*safe.report, "ui_activate=Invio|A"));
    CHECK(*rv::core::readTextFile(room.data / "settings.json") == written);

    Script clear;
    clear.press("menu").press("ui_down").press("ui_activate");
    clear.press("ui_filter_next", 3).press("ui_down", 3); // past the three rows of the look: no binding row here
    clear.wait(2).press("ui_activate").press("ui_activate").wait(2); // asks once
    clear.press("ui_back").press("ui_back").wait(10);
    const AppRun cleared = runScript(room, "clear", clear, {"--safe-bindings"});
    requireCleanRun(cleared);
    INFO(cleared.describe());
    const std::vector<std::string>& rows = cleared.report->menu->settingsRows;
    CHECK(std::find(rows.begin(), rows.end(), "bindings.ui_activate") == rows.end());
    CHECK(std::find(rows.begin(), rows.end(), "bindings.defaults") != rows.end());
    CHECK_FALSE(room.readSettings().contains("bindings"));

    const AppRun after = runScript(room, "after", look);
    requireCleanRun(after);
    CHECK(bound(*after.report, "ui_activate=Invio|A"));
}

TEST_CASE("controls: Controller scollegato shows over a screen, until the pad is back", "[gpu][controls]") {
    const Room room("pad_lost");
    Script script;
    script.press("menu").wait(6);
    const int plugged = script.frame;
    script.padPlugged(plugged).wait(10);
    const AppRun lost = runScript(room, "lost", script);
    skipWithoutOpenGl(lost);
    requireCleanRun(lost);
    INFO(lost.describe());
    REQUIRE(lost.report->controls);
    CHECK(lost.report->controls->padsLost == 1);
    CHECK(lost.report->controls->noticeShown);
    CHECK(lost.report->controls->noticeFrames > 0);
    CHECK(lost.report->menu->open); // the screen stays

    // Plugged in again: it goes at once.
    Script back = script;
    const int again = back.frame;
    back.steps.push_back(Json{{"fromFrame", again}, {"toFrame", again + 20}, {"raw", {{"padConnected", true}}}});
    back.wait(10);
    const AppRun returned = runScript(room, "back", back);
    requireCleanRun(returned);
    INFO(returned.describe());
    CHECK(returned.report->controls->padsLost == 1);
    CHECK(returned.report->controls->padsBack == 1);
    CHECK_FALSE(returned.report->controls->noticeShown);
}

TEST_CASE("controls: at text scale 125% at 1280 x 720 no hint runs past its box, on either device",
          "[gpu][controls]") {
    const Room room("text_scale");
    room.writeSettings(R"({"schemaVersion": 1, "player": {"textScale": 1.25}})");
    const auto check = [&](std::string_view name, Script script) {
        const AppRun run = runScript(room, name, script);
        skipWithoutOpenGl(run);
        requireCleanRun(run);
        INFO(name << ": " << run.describe());
        CHECK(run.report->controls->hintsOutside == 0);
        CHECK(run.report->menu->textCut == 0);
        CHECK(run.report->menu->settingsTextCut == 0);
        CHECK_FALSE(run.report->controls->hints.empty());
    };
    for (const std::string_view device : {"keys", "pad"}) {
        const std::string name(device);
        check(name + "_catalog", Script{}.device(device).press("quick_catalog").wait(20));
        check(name + "_directory", Script{}.device(device).press("map").wait(20));
        check(name + "_settings", Script{}.device(device).toActivateRow().wait(4));
        check(name + "_offer",
              Script{}.device(device).toActivateRow().press("ui_activate").wait(2).key("escape").wait(4));
    }
}

// By hand, for a look: rv_smoke_tests "[controls_shots]" with RV_SHOTS_DIR naming a folder writes the pictures, hidden and
// frame-limited. Not a check: visual verification (docs/reference/visual-verification.md).
TEST_CASE("controls: the pictures", "[.controls_shots]") {
    const char* folder = std::getenv("RV_SHOTS_DIR");
    if (folder == nullptr) {
        SKIP("RV_SHOTS_DIR names no folder");
    }
    const fs::path out = rv::core::pathFromUtf8(folder);
    fs::create_directories(out);
    const Room room("shots");
    const auto shoot = [&](std::string_view name, const Script& script) {
        const AppRun run = runScript(room, name, script);
        requireCleanRun(run);
        fs::copy_file(room.dir / (std::string(name) + ".png"), out / (std::string(name) + ".png"),
                      fs::copy_options::overwrite_existing);
    };
    shoot("hints_keyboard", Script{}.key("tab").wait(20));
    shoot("hints_pad", Script{}.padPlugged(200).pad("north").wait(20));
    shoot("settings_keyboard", Script{}.device("keys").toActivateRow().wait(4));
    shoot("settings_pad", Script{}.device("pad").toActivateRow().wait(4));
    shoot("remapping_list", Script{}.device("pad").press("menu").press("ui_down").press("ui_activate")
                                .press("ui_filter_next", 3).press("ui_down", 16).wait(4));
    shoot("capture", Script{}.device("pad").toActivateRow().press("ui_activate").wait(4));
    shoot("capture_refused", Script{}.device("keys").toActivateRow().press("ui_activate").wait(2).key("k").wait(4));
    shoot("swap_offer", Script{}.padPlugged(200).toActivateRow().press("ui_activate").wait(2).pad("west").wait(4));
    Script lost;
    lost.padPlugged(30).wait(40);
    shoot("controller_notice_world", lost);
    Script overMenu;
    overMenu.press("menu").wait(6);
    overMenu.padPlugged(overMenu.frame).wait(10);
    shoot("controller_notice_menu", overMenu);
}
