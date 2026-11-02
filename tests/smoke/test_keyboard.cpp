// The on-screen keyboard (I7 step 5, ADR-0018 §8, §12, §15), on the snes-room fixture imported into the case's data
// directory, read from the run report's "keyboard" and "catalog" sections: a search typed with the pad's actions alone
// narrows the list; the accented letters, delete, close; a physical keyboard types as before; the catalog closing under
// it; its hints from the active map and a remapped pad; nothing cut at 1280 x 720, 1920 x 1080 and text scale 125%; the same
// search by a scripted ray with --xr-runtime fake. No real input: everything comes from input scripts.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/ui/keyboard_model.hpp>

#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr int kFirstPress = 10;
constexpr int kGap = 3;

fs::path cliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

struct Room {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit Room(std::string_view name)
        : fixture(rv::test::Fixture::materialize("esde/snes-room").value()),
          dir(freshCaseDir(std::string("keyboard/") + std::string(name))), data(dir / "data") {
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
};

/// A script of the pad's actions: a press every kGap frames from kFirstPress. It follows the keyboard's selected key on a
/// model of its own, so that a test names the text it types.
struct Script {
    int frame = kFirstPress;
    Json steps = Json::array();
    rv::ui::KeyboardModel keyboard;

    Script& press(std::string_view action, int times = 1) {
        for (int index = 0; index < times; ++index) {
            steps.push_back(Json{{"fromFrame", frame}, {"toFrame", frame}, {"actions", {{std::string(action), 1}}}});
            frame += kGap;
        }
        return *this;
    }
    Script& raw(std::string_view field, std::string_view name) {
        steps.push_back(
            Json{{"fromFrame", frame}, {"toFrame", frame}, {"raw", {{std::string(field), Json::array({name})}}}});
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
    Script& padPlugged(int last) {
        steps.push_back(Json{{"fromFrame", 0}, {"toFrame", last}, {"raw", {{"padConnected", true}}}});
        return *this;
    }
    /// The quick catalog, then Up to its search field (over the filters' row).
    Script& toField() { return press("quick_catalog").wait(20).press("ui_up", 2); }
    /// Activate on the field, the pad used last.
    Script& openKeyboard() {
        keyboard.open();
        return press("ui_activate").wait(2);
    }
    /// Walks to the key that types `text`, each UTF-8 character one key, and presses it.
    Script& type(std::string_view text) {
        for (std::size_t at = 0; at < text.size();) {
            const std::size_t length = (static_cast<unsigned char>(text[at]) & 0x80) != 0 ? 2 : 1;
            const std::string_view wanted = text.substr(at, length);
            at += length;
            const auto keys = rv::ui::keyboardKeys();
            const auto key = std::find_if(keys.begin(), keys.end(),
                                          [&](const rv::ui::KeyboardKey& one) { return one.text == wanted; });
            REQUIRE(key != keys.end());
            while (keyboard.selectedKey().row != key->row) {
                static_cast<void>(keyboard.apply(rv::ui::UiCommand::Down));
                press("ui_down");
            }
            while (&keyboard.selectedKey() != &*key) {
                static_cast<void>(keyboard.apply(rv::ui::UiCommand::Right));
                press("ui_right");
            }
            press("ui_activate");
        }
        return *this;
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
        "--start-room", "room:snes", "--exit-after-frames", "--frames",  std::to_string(script.frame + 8),
        "--fixed-dt",   "0.016667",  "--screenshot",        arg(room.dir / (std::string(name) + ".png")),
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

} // namespace

TEST_CASE("keyboard: a search typed with the pad's actions alone narrows the list", "[gpu][keyboard]") {
    const Room room("search");
    Script script;
    script.device("pad").toField().openKeyboard().type("opal").wait(6);
    const AppRun typing = runScript(room, "typing", script);
    skipWithoutOpenGl(typing);
    requireCleanRun(typing);
    INFO(typing.describe());
    REQUIRE(typing.report->keyboard);
    CHECK(typing.report->keyboard->opened == 1);
    CHECK(typing.report->keyboard->via == "pad");
    CHECK(typing.report->keyboard->open);
    CHECK(typing.report->keyboard->typed == 4);
    CHECK(typing.report->keyboard->selected == "l");
    CHECK(typing.report->catalog->search == "opal");
    CHECK(typing.report->catalog->listed == 1);
    CHECK(typing.report->catalog->shownCount == 1);

    // Start confirms: it closes, the search stays, and the next A starts the game found: the focus went on to the list.
    script.press("confirm").wait(4).press("ui_activate").wait(4);
    const AppRun confirmed = runScript(room, "confirmed", script);
    requireCleanRun(confirmed);
    INFO(confirmed.describe());
    REQUIRE(confirmed.report->keyboard);
    CHECK(confirmed.report->keyboard->confirmed == 1);
    CHECK_FALSE(confirmed.report->keyboard->open);
    CHECK(confirmed.report->keyboard->opened == 1);
    CHECK(confirmed.report->catalog->search == "opal");
    CHECK(confirmed.report->catalog->open);
    CHECK(confirmed.report->launches.size() == 1);
}

TEST_CASE("keyboard: accents and the space reach the field in UTF-8, X deletes one letter, B closes and keeps it",
          "[gpu][keyboard]") {
    const Room room("accents");
    Script script;
    script.device("pad").toField().openKeyboard().type("caffè àù").press("ui_info").wait(6);
    const AppRun run = runScript(room, "run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->keyboard);
    CHECK(run.report->keyboard->typed == 8);
    CHECK(run.report->keyboard->deleted == 1);
    CHECK(run.report->catalog->search == "caffè à");
    CHECK(run.report->catalog->listed == 0);

    script.press("ui_back").wait(4);
    const AppRun closed = runScript(room, "closed", script);
    requireCleanRun(closed);
    INFO(closed.describe());
    CHECK(closed.report->keyboard->closed == 1);
    CHECK_FALSE(closed.report->keyboard->open);
    CHECK(closed.report->catalog->open); // B closed the keyboard, not the catalog
    CHECK(closed.report->catalog->search == "caffè à");

    // Back on the field the next A opens it again, on the first letter; delete goes on where it stopped.
    script.openKeyboard().press("ui_info", 2).wait(4);
    const AppRun again = runScript(room, "again", script);
    requireCleanRun(again);
    INFO(again.describe());
    CHECK(again.report->keyboard->opened == 2);
    CHECK(again.report->keyboard->open);
    CHECK(again.report->keyboard->selected == "q");
    CHECK(again.report->catalog->search == "caffè");
}

TEST_CASE("keyboard: a physical keyboard types as before: Enter on the field opens nothing", "[gpu][keyboard]") {
    const Room room("physical");
    Script script;
    script.device("keys").toField();
    script.steps.push_back(Json{{"fromFrame", script.frame}, {"toFrame", script.frame}, {"text", "opal"}});
    script.wait(6).press("ui_activate").wait(6);
    const AppRun run = runScript(room, "run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    CHECK_FALSE(run.report->keyboard);
    CHECK(run.report->catalog->search == "opal");
    CHECK(run.report->catalog->listed == 1);
}

TEST_CASE("keyboard: over it Enter confirms, once, and the catalog's toggle closes both", "[gpu][keyboard]") {
    const Room room("keys_over");
    Script script;
    script.padPlugged(120).device("pad").toField().openKeyboard().type("o");
    script.raw("keys", "enter").wait(4); // Activate and Confirm in one press: the key used last says Confirm
    const AppRun enter = runScript(room, "enter", script);
    skipWithoutOpenGl(enter);
    requireCleanRun(enter);
    INFO(enter.describe());
    REQUIRE(enter.report->keyboard);
    CHECK(enter.report->keyboard->typed == 1);
    CHECK(enter.report->keyboard->confirmed == 1);
    CHECK_FALSE(enter.report->keyboard->open);

    Script toggle;
    toggle.device("pad").toField().openKeyboard();
    toggle.raw("keys", "tab").wait(6);
    const AppRun closed = runScript(room, "toggle", toggle);
    requireCleanRun(closed);
    INFO(closed.describe());
    CHECK_FALSE(closed.report->catalog->open);
    CHECK_FALSE(closed.report->keyboard->open);
}

TEST_CASE("keyboard: a direction held through Start or B moves nothing behind it", "[gpu][keyboard]") {
    const Room room("held");
    const auto holding = [](std::string_view closing) {
        Script script;
        script.device("pad").toField().openKeyboard();
        const int from = script.frame;
        script.steps.push_back(Json{{"fromFrame", from}, {"toFrame", from + 60}, {"actions", {{"ui_down", 1}}}});
        script.steps.push_back(Json{{"fromFrame", from + 20}, {"toFrame", from + 20}, {"actions", {{std::string(closing), 1}}}});
        script.frame = from + 66;
        return script;
    };
    // Start: on to the list, whose selection stays on its first game.
    const AppRun confirmed = runScript(room, "confirmed", holding("confirm"));
    skipWithoutOpenGl(confirmed);
    requireCleanRun(confirmed);
    INFO(confirmed.describe());
    CHECK(confirmed.report->keyboard->confirmed == 1);
    CHECK(confirmed.report->catalog->selectedPosition == 0);

    // B: the focus stays on the field, so the next A opens the keyboard again.
    Script closing = holding("ui_back");
    closing.openKeyboard().wait(4);
    const AppRun closed = runScript(room, "closed", closing);
    requireCleanRun(closed);
    INFO(closed.describe());
    CHECK(closed.report->keyboard->closed == 1);
    CHECK(closed.report->keyboard->opened == 2);
}

TEST_CASE("keyboard: the hints name the active map, and a remapped pad's buttons work", "[gpu][keyboard]") {
    const Room room("hints");
    Script shipped;
    shipped.device("pad").toField().openKeyboard().wait(4);
    const AppRun onPad = runScript(room, "shipped", shipped);
    skipWithoutOpenGl(onPad);
    requireCleanRun(onPad);
    INFO(onPad.describe());
    CHECK(hintOf(*onPad.report, "ui_activate") == "A");
    CHECK(hintOf(*onPad.report, "ui_info") == "X");
    CHECK(hintOf(*onPad.report, "confirm") == "Start");
    CHECK(hintOf(*onPad.report, "ui_back") == "B");
    CHECK(hintOf(*onPad.report, "ui_media").empty()); // the catalog's bar hides under it

    room.writeSettings(R"({"schemaVersion": 1, "bindings": {
      "confirm": [{"key": "enter"}, {"padButton": "right_shoulder"}],
      "ui_info": [{"key": "f4"}, {"padButton": "left_shoulder"}]}})");
    Script remapped;
    remapped.padPlugged(160).device("pad").toField().openKeyboard().type("o");
    remapped.raw("padButtons", "west").wait(2);          // X no longer deletes
    remapped.raw("padButtons", "start").wait(2);         // nor does Start confirm
    const AppRun kept = runScript(room, "kept", remapped);
    requireCleanRun(kept);
    INFO(kept.describe());
    CHECK(hintOf(*kept.report, "ui_info") == "LB");
    CHECK(hintOf(*kept.report, "confirm") == "RB");
    CHECK(kept.report->keyboard->open);
    CHECK(kept.report->catalog->search == "o");

    remapped.raw("padButtons", "left_shoulder").wait(2); // LB deletes
    remapped.raw("padButtons", "right_shoulder").wait(4); // RB confirms
    const AppRun used = runScript(room, "used", remapped);
    requireCleanRun(used);
    INFO(used.describe());
    CHECK(used.report->keyboard->deleted == 1);
    CHECK(used.report->keyboard->confirmed == 1);
    CHECK(used.report->catalog->search.empty());
}

TEST_CASE("keyboard: nothing cut at 1280 x 720 and 1920 x 1080, at text scale 100% and 125%, on either device",
          "[gpu][keyboard]") {
    const Room room("sizes");
    struct Size {
        int width;
        int height;
    };
    for (const Size size : {Size{1280, 720}, Size{1920, 1080}}) {
        for (const double scale : {1.0, 1.25}) {
            for (const std::string_view device : {"pad", "keys"}) {
                room.writeSettings(fmt::format(R"({{"schemaVersion": 1, "player": {{"textScale": {}}}}})", scale));
                Script script;
                // The pad opens it; the keyboard's device then names the hints, the longest of them.
                script.device("pad").toField().openKeyboard().device(device).wait(6);
                const std::string name = fmt::format("{}x{}_{}_{}", size.width, size.height, scale * 100, device);
                const AppRun run = runScript(room, name, script,
                                             {"--width", std::to_string(size.width), "--height",
                                              std::to_string(size.height)});
                skipWithoutOpenGl(run);
                requireCleanRun(run);
                INFO(name << ": " << run.describe());
                REQUIRE(run.report->keyboard);
                CHECK(run.report->keyboard->open);
                CHECK(run.report->keyboard->textCut == 0);
                CHECK(run.report->controls->hintsOutside == 0);
                CHECK(hintOf(*run.report, "confirm") == (device == "pad" ? "Start" : "Invio"));
                REQUIRE(run.report->keyboard->gridBox);
                const glm::vec4 grid = *run.report->keyboard->gridBox;
                CHECK(grid.x + grid.z <= static_cast<float>(size.width));
                CHECK(grid.y + grid.w <= static_cast<float>(size.height));
                // The field stays in view beside the panel: what is typed shows.
                REQUIRE(run.report->keyboard->fieldBox);
                const glm::vec4 field = *run.report->keyboard->fieldBox;
                CHECK(field.x + field.z < grid.x);
            }
        }
    }
}

namespace {

/// The right controller's pose aimed at `pixel` of a `size` view on VR's panel, which stands 1.3 m in front of the eyes,
/// 0.12 m below them, 1.8 m wide at the window's 16:9.
std::string aimAt(glm::vec2 pixel, glm::vec2 size) {
    const glm::vec3 hand(0.2f, 1.25f, -0.3f);
    const glm::vec3 target((pixel.x / size.x - 0.5f) * 1.8f,
                           1.65f - 0.12f + (0.5f - pixel.y / size.y) * 1.8f * size.y / size.x, -1.3f);
    const glm::vec3 toTarget = glm::normalize(target - hand);
    const float yawDeg = rv::core::radiansToDegrees(std::atan2(toTarget.x, -toTarget.z));
    const float pitchDeg = rv::core::radiansToDegrees(std::asin(toTarget.y));
    return fmt::format(R"({{"yaw": {}, "pitch": {}, "position": [{}, {}, {}]}})", yawDeg, pitchDeg, hand.x, hand.y,
                       hand.z);
}

/// The centre of `key` in a `grid` box.
glm::vec2 keyCentre(const rv::ui::KeyboardKey& key, const glm::vec4& grid) {
    const float column = (static_cast<float>(key.column) + static_cast<float>(key.span) / 2.0f) /
                         static_cast<float>(rv::ui::keyboard::kColumns);
    const float row = (static_cast<float>(key.row) + 0.5f) / static_cast<float>(rv::ui::keyboard::kRows);
    return {grid.x + column * grid.z, grid.y + row * grid.w};
}

} // namespace

TEST_CASE("keyboard: in a headset the ray opens it on the field and types with clicks", "[gpu][keyboard][xr]") {
    const Room room("vr");
    const std::vector<std::string> vr = {"--vr", "--xr-runtime", "fake", "--width", "1280", "--height", "720"};
    constexpr glm::vec2 kView(1280.0f, 720.0f);
    const auto head = [](int last) {
        return Json{{"fromFrame", 0}, {"toFrame", last}, {"headPose", {{"position", {0, 1.65, 0}}}}};
    };

    // Where the field and the grid stand on this view, from the pad.
    Script measure;
    measure.steps.push_back(head(120));
    measure.device("pad").toField().openKeyboard().wait(6);
    const AppRun measured = runScript(room, "measure", measure, vr);
    skipWithoutOpenGl(measured);
    requireCleanRun(measured);
    INFO(measured.describe());
    REQUIRE(measured.report->keyboard);
    REQUIRE(measured.report->keyboard->gridBox);
    REQUIRE(measured.report->keyboard->fieldBox);
    const glm::vec4 grid = *measured.report->keyboard->gridBox;
    const glm::vec4 field = *measured.report->keyboard->fieldBox;

    // Each click: the ray on its target for some frames, the trigger down and up in the middle of them.
    Script script;
    script.steps.push_back(head(400));
    script.press("quick_catalog").wait(20);
    const auto click = [&](glm::vec2 pixel) {
        const int from = script.frame;
        script.steps.push_back(Json{{"fromFrame", from},
                                    {"toFrame", from + 9},
                                    {"rightHand", Json::parse(aimAt(pixel, kView))}});
        script.steps.push_back(Json{{"fromFrame", from + 3}, {"toFrame", from + 5}, {"rightSelect", 1}});
        script.frame = from + 12;
    };
    click(glm::vec2(field.x + field.z / 2.0f, field.y + field.w / 2.0f));
    const auto keys = rv::ui::keyboardKeys();
    for (const std::string_view letter : {"o", "p", "a", "l"}) {
        const auto key = std::find_if(keys.begin(), keys.end(),
                                      [&](const rv::ui::KeyboardKey& one) { return one.text == letter; });
        REQUIRE(key != keys.end());
        click(keyCentre(*key, grid));
    }
    const AppRun typed = runScript(room, "typed", script, vr);
    requireCleanRun(typed);
    INFO(typed.describe());
    REQUIRE(typed.report->xr);
    CHECK(typed.report->xr->uiClicks == 5);
    REQUIRE(typed.report->keyboard);
    CHECK(typed.report->keyboard->via == "ray");
    CHECK(typed.report->keyboard->open);
    CHECK(typed.report->keyboard->clicks == 4);
    CHECK(typed.report->keyboard->typed == 4);
    CHECK(typed.report->catalog->search == "opal");
    CHECK(typed.report->catalog->listed == 1);

    // Conferma's key, then the keyboard opened again and a click beside its panel.
    const auto confirm = std::find_if(keys.begin(), keys.end(), [](const rv::ui::KeyboardKey& one) {
        return one.kind == rv::ui::KeyboardKeyKind::Confirm;
    });
    click(keyCentre(*confirm, grid));
    click(glm::vec2(field.x + field.z / 2.0f, field.y + field.w / 2.0f));
    click(glm::vec2(grid.x + grid.z / 2.0f, kView.y - 12.0f));
    const AppRun ended = runScript(room, "ended", script, vr);
    requireCleanRun(ended);
    INFO(ended.describe());
    CHECK(ended.report->keyboard->confirmed == 1);
    CHECK(ended.report->keyboard->opened == 2);
    CHECK(ended.report->keyboard->closed == 1);
    CHECK_FALSE(ended.report->keyboard->open);
    CHECK(ended.report->catalog->open);
    CHECK(ended.report->catalog->search == "opal");
}

// By hand, for a look: rv_smoke_tests "[keyboard_shots]" with RV_SHOTS_DIR naming a folder writes the pictures, hidden and
// frame-limited, at 1920 x 1080 and 1280 x 720, at text scale 100% and 125%. Not a check: visual verification
// (docs/reference/visual-verification.md).
TEST_CASE("keyboard: the pictures", "[.keyboard_shots]") {
    const char* folder = std::getenv("RV_SHOTS_DIR");
    if (folder == nullptr) {
        SKIP("RV_SHOTS_DIR names no folder");
    }
    const fs::path out = rv::core::pathFromUtf8(folder);
    fs::create_directories(out);
    const Room room("shots");
    struct Shot {
        std::string_view name;
        std::string_view width;
        std::string_view height;
        double scale;
    };
    for (const Shot shot : {Shot{"1920x1080_100", "1920", "1080", 1.0}, Shot{"1920x1080_125", "1920", "1080", 1.25},
                            Shot{"1280x720_100", "1280", "720", 1.0}, Shot{"1280x720_125", "1280", "720", 1.25}}) {
        room.writeSettings(fmt::format(R"({{"schemaVersion": 1, "player": {{"textScale": {}}}}})", shot.scale));
        Script script;
        script.device("pad").toField().openKeyboard().type("opà").wait(6);
        const std::string name = std::string("keyboard_") + std::string(shot.name);
        const AppRun run = runScript(room, name, script,
                                     {"--width", std::string(shot.width), "--height", std::string(shot.height)});
        requireCleanRun(run);
        INFO(run.describe());
        CHECK(run.report->keyboard->textCut == 0);
        fs::copy_file(room.dir / (name + ".png"), out / (name + ".png"), fs::copy_options::overwrite_existing);
    }
}
