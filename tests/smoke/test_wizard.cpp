// The first-run wizard (I7 step 2, ADR-0018 §4, §12, §15), driven by the pad's actions alone through input scripts on the
// esde/building fixture, read from the run report's "wizard" section: the seven pages to the lobby, the world started in
// place on the catalog the wizard made, the report's counts equal to retroverse-cli's dry run, nothing written under the
// fixture's ES-DE, ROM or media folders; the folder browser; a resumed wizard; --wizard without --esde-home exits 2; a
// frame-limited run without library.json keeps today's test room; no text cut at 125% at 1280 x 720; the ray clicks.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/sha256.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>

#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

constexpr int kFirstPress = 5;
constexpr int kGap = 3;

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

/// A script of pad actions, one press every kGap frames: a lockstep run waits for what the wizard's pages wait for, so
/// that each press finds its page ready.
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
    /// Avanti, `pages` times, from the first page.
    Script& forward(int pages) { return press("ui_activate", pages); }

    void write(const fs::path& file) const {
        REQUIRE(rv::core::writeTextFileAtomic(file, steps.dump(2) + "\n").has_value());
    }
};

std::vector<std::string> wizardArguments(const rv::test::Fixture& fixture, int frames, const fs::path& shot,
                                         const fs::path& script) {
    return {"--wizard",     "--esde-home",  arg(fixture.root()),    "--esde-install",
            arg(fixture.path("Programs/ES-DE")), "--exit-after-frames",  "--frames",
            std::to_string(frames), "--fixed-dt", "0.016667", "--screenshot",
            arg(shot),      "--input-script", arg(script)};
}

/// A name and a digest per file under `root`.
std::map<std::string, std::string> treeOf(const fs::path& root) {
    std::map<std::string, std::string> files;
    for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            const auto bytes = rv::core::readBinaryFile(entry.path());
            REQUIRE(bytes.has_value());
            files[rv::core::pathToUtf8(fs::relative(entry.path(), root))] =
                rv::core::sha256Hex(std::span<const std::byte>(*bytes));
        }
    }
    return files;
}

Json dryRun(const rv::test::Fixture& fixture, const fs::path& dir) {
    const fs::path data = dir / "cli-data";
    const std::vector<std::string> create = {"--data-dir", arg(data), "library", "create", "--home", arg(fixture.root()),
                                             "--install", arg(fixture.path("Programs/ES-DE"))};
    const AppRun created = runCommand(retroverseCliExe(), dir, "cli_library", create);
    INFO(created.describe());
    REQUIRE(created.exitCode == 0);
    const fs::path out = dir / "dry-run.json";
    const AppRun read = runCommand(retroverseCliExe(), dir, "cli_dry_run",
                                   {"--data-dir", arg(data), "import", "--dry-run", "--json", "--output", arg(out)});
    INFO(read.describe());
    REQUIRE((read.exitCode == 0 || read.exitCode == 1));
    return Json::parse(rv::core::readTextFile(out).value());
}

} // namespace

TEST_CASE("wizard: the seven pages to the lobby by the pad alone, its counts the dry run's", "[gpu][wizard]") {
    const fs::path dir = freshCaseDir("wizard/seven_pages");
    const rv::test::Fixture fixture = rv::test::Fixture::materialize("esde/building").value();
    const std::map<std::string, std::string> before = treeOf(fixture.root());
    const Json cli = dryRun(fixture, dir);
    REQUIRE(treeOf(fixture.root()) == before);

    Script script;
    script.forward(6);         // Benvenuto, Rilevamento, Anteprima, Importazione, Emulatori, Aspetto
    script.press("ui_activate"); // Entra, the world ready
    script.wait(30);
    const fs::path file = dir / "script.json";
    script.write(file);
    const fs::path shot = dir / "lobby.png";
    const AppRun run = runApp(retroverseExe(), dir, "run", wizardArguments(fixture, script.frame + 2, shot, file));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    requireScreenshot(run, shot, {1920, 1080});

    REQUIRE(run.report->wizard);
    const rv::app::WizardRecord& wizard = *run.report->wizard;
    CHECK(wizard.entered);
    CHECK_FALSE(wizard.open);
    CHECK_FALSE(wizard.resumed);
    CHECK(wizard.pages == std::vector<std::string>{"welcome", "detection", "preview", "import", "emulators", "appearance",
                                                   "world"});
    CHECK(wizard.previewWork == "done");
    CHECK(wizard.importWork == "done");
    CHECK(wizard.worldWork == "done");
    CHECK(wizard.textCut == 0);

    // The report's counts are the CLI's dry run.
    const Json& totals = cli.at("totals");
    CHECK(wizard.systems == cli.at("systems").size());
    CHECK(wizard.entries == totals.at("entries").get<std::uint64_t>());
    CHECK(wizard.games == totals.at("games").get<std::uint64_t>());
    CHECK(wizard.esdeGames == totals.at("esdeGames").get<std::uint64_t>());
    CHECK(wizard.missingFiles == totals.at("missingFiles").get<std::uint64_t>());
    CHECK(wizard.notAGame == totals.at("notAGame").get<std::uint64_t>());
    CHECK(wizard.discMembers == totals.at("discMembers").get<std::uint64_t>());
    CHECK(wizard.issues == cli.at("issues").size());
    CHECK(wizard.games > 0);
    CHECK(wizard.media > 0);

    // The database made, the first import through the refresh, the world in place on that catalog.
    CHECK(fs::exists(dir / "data" / "database" / "retroverse.sqlite"));
    CHECK(fs::exists(dir / "data" / "config" / "library.json"));
    REQUIRE(run.report->catalog);
    const rv::app::CatalogRecord& catalog = *run.report->catalog;
    CHECK(catalog.source == "esde");
    CHECK(catalog.notice.empty());
    CHECK(catalog.games > 0);
    REQUIRE(catalog.refreshes.size() == 1);
    CHECK(catalog.refreshes.front().trigger == "wizard");
    CHECK(catalog.refreshes.front().outcome == "done");
    CHECK(catalog.refreshes.front().added == wizard.added);
    CHECK(catalog.refreshes.front().added > 0);
    CHECK(run.report->world.state == "room");
    CHECK(run.report->world.notice.empty());
    REQUIRE(run.report->menu);
    CHECK(run.report->menu->opened == 0);

    // Nothing under the fixture's ES-DE, ROM or media folders changed.
    CHECK(treeOf(fixture.root()) == before);

    // The next start finds it all ready: no wizard, the building at once.
    const fs::path next = dir / "next.png";
    const AppRun again = runApp(retroverseExe(), dir, "again",
                                {"--exit-after-frames", "--frames", "20", "--fixed-dt", "0.016667", "--screenshot",
                                 arg(next)});
    requireCleanRun(again);
    INFO(again.describe());
    CHECK_FALSE(again.report->wizard.has_value());
    CHECK(again.report->world.state == "room");
}

TEST_CASE("wizard: --wizard without --esde-home exits 2, frame-limited or not", "[gpu][wizard]") {
    const fs::path dir = freshCaseDir("wizard/no_home");
    const AppRun limited = runCommand(retroverseExe(), dir, "limited",
                                      {"--wizard", "--smoke", "--hidden", "--data-dir", arg(dir / "data")});
    INFO(limited.describe());
    CHECK(limited.exitCode == 2);
    const AppRun plain = runCommand(retroverseExe(), dir, "plain", {"--wizard", "--data-dir", arg(dir / "data")});
    INFO(plain.describe());
    CHECK(plain.exitCode == 2);
    CHECK_FALSE(fs::exists(dir / "data" / "config"));
}

TEST_CASE("wizard: a frame-limited run without library.json keeps today's test room and report", "[gpu][wizard]") {
    const fs::path dir = freshCaseDir("wizard/keeps_test_room");
    const AppRun run = runApp(retroverseExe(), dir, "run", {"--smoke", "--frames", "12", "--fixed-dt", "0.016667"});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    CHECK_FALSE(run.report->wizard.has_value());
    CHECK(run.report->world.state == "test-room");
    CHECK(run.report->world.notice == "no-catalog");
    CHECK(run.report->world.catalogState == "no-library");
    REQUIRE(run.report->catalog);
    CHECK(run.report->catalog->notice == "no-library");
    CHECK_FALSE(fs::exists(dir / "data" / "database"));
    CHECK_FALSE(fs::exists(dir / "data" / "config" / "library.json"));
}

TEST_CASE("wizard: the folder browser changes a root; a folder that vanished leaves its buttons", "[gpu][wizard]") {
    const fs::path dir = freshCaseDir("wizard/browser");
    const rv::test::Fixture fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    // ES-DE's ROM folder moved away: page 2 stops, and the browser opens over what vanished.
    fs::rename(fixture.path("roms"), fixture.path("moved"));
    Script script;
    script.press("ui_activate").wait(3); // to Rilevamento: Avanti not offered
    script.press("ui_down", 2);          // the ROM row
    script.press("ui_activate").wait(3); // the browser: the folder is gone
    script.press("ui_activate").wait(3); // the row up, the fixture's root
    const fs::path file = dir / "script.json";
    script.write(file);
    const fs::path shot = dir / "browser.png";
    const AppRun run = runApp(retroverseExe(), dir, "run", wizardArguments(fixture, script.frame + 3, shot, file));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->wizard);
    const rv::app::WizardRecord& wizard = *run.report->wizard;
    CHECK(wizard.page == "detection");
    CHECK(wizard.problem == "rom-root-unavailable");
    CHECK(wizard.browserState == "listed");
    CHECK(wizard.browserTextCut == 0);
    CHECK_FALSE(fs::exists(dir / "data" / "config" / "library.json"));
}

TEST_CASE("wizard: a run with library.json and no catalog resumes at the preview", "[gpu][wizard]") {
    const fs::path dir = freshCaseDir("wizard/resume");
    const rv::test::Fixture fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    const AppRun created = runCommand(retroverseCliExe(), dir, "cli_library",
                                      {"--data-dir", arg(dir / "data"), "library", "create", "--home", arg(fixture.root()),
                                       "--install", arg(fixture.path("Programs/ES-DE"))});
    INFO(created.describe());
    REQUIRE(created.exitCode == 0);
    const std::string before = rv::core::readTextFile(dir / "data" / "config" / "library.json").value();

    Script script;
    script.wait(3);
    script.forward(5).press("ui_activate").wait(20); // Anteprima to Aspetto, Entra
    const fs::path file = dir / "script.json";
    script.write(file);
    const AppRun run = runApp(retroverseExe(), dir, "run",
                              wizardArguments(fixture, script.frame + 2, dir / "end.png", file));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->wizard);
    CHECK(run.report->wizard->resumed);
    CHECK(run.report->wizard->entered);
    CHECK(run.report->wizard->pages.front() == "preview");
    CHECK(run.report->world.state == "room");
    CHECK(rv::core::readTextFile(dir / "data" / "config" / "library.json").value() == before);
}

TEST_CASE("wizard: at text scale 125% at 1280 x 720 no page and no browser is cut", "[gpu][wizard]") {
    const fs::path dir = freshCaseDir("wizard/text_scale");
    const rv::test::Fixture fixture = rv::test::Fixture::materialize("esde/building").value();
    REQUIRE(rv::core::writeTextFileAtomic(dir / "data" / "settings.json",
                                          R"({"schemaVersion": 1, "player": {"textScale": 1.25}})")
                .has_value());
    Script script;
    script.press("ui_activate").wait(3);
    script.press("ui_activate").wait(3); // Avanti is focused: on to the preview, and back
    script.press("ui_back").wait(3);
    script.press("ui_up", 4).press("ui_activate").wait(3); // the first folder's browser
    script.press("ui_back").wait(3);
    script.press("ui_down", 4); // Avanti again
    script.forward(5).wait(3);  // on through the pages
    const fs::path file = dir / "script.json";
    script.write(file);
    std::vector<std::string> arguments = wizardArguments(fixture, script.frame + 3, dir / "end.png", file);
    arguments.insert(arguments.end(), {"--width", "1280", "--height", "720", "--screenshot-size", "1280x720"});
    const AppRun run = runApp(retroverseExe(), dir, "run", std::move(arguments));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->wizard);
    CHECK(run.report->wizard->pages.size() >= 7);
    CHECK(run.report->wizard->textCut == 0);
    CHECK(run.report->wizard->browserTextCut == 0);
}

TEST_CASE("wizard: in a headset the ray presses what the pad would", "[gpu][wizard][xr]") {
    const fs::path dir = freshCaseDir("wizard/vr");
    const rv::test::Fixture fixture = rv::test::Fixture::materialize("esde/snes-room").value();
    // The right controller at the hip points at Avanti, at the panel's bottom left (the canvas stands 1.3 m in front of
    // the eyes, 0.12 m below them, 1.8 m wide at the window's 16:9).
    const glm::vec3 hand(0.2f, 1.25f, -0.3f);
    const glm::vec3 target(-0.68f, 1.65f - 0.12f - 0.37f, -1.3f);
    const glm::vec3 toTarget = glm::normalize(target - hand);
    const float yawDeg = rv::core::radiansToDegrees(std::atan2(toTarget.x, -toTarget.z));
    const float pitchDeg = rv::core::radiansToDegrees(std::asin(toTarget.y));
    const std::string pose = fmt::format(R"("rightHand": {{"yaw": {}, "pitch": {}, "position": [{}, {}, {}]}})", yawDeg,
                                         pitchDeg, hand.x, hand.y, hand.z);
    const fs::path script = dir / "vr.json";
    REQUIRE(rv::core::writeTextFileAtomic(script, fmt::format(R"([
  {{"fromFrame": 0, "toFrame": 59, "headPose": {{"position": [0, 1.65, 0]}}}},
  {{"fromFrame": 10, "toFrame": 59, {0}}},
  {{"fromFrame": 30, "toFrame": 33, "rightSelect": 1}}
])",
                                                              pose))
                .has_value());
    const AppRun run = runApp(retroverseExe(), dir, "run",
                              {"--wizard", "--esde-home", arg(fixture.root()), "--esde-install",
                               arg(fixture.path("Programs/ES-DE")), "--vr", "--xr-runtime", "fake",
                               "--exit-after-frames", "--frames", "60", "--fixed-dt", "0.016667", "--screenshot",
                               arg(dir / "eyes.png"), "--input-script", arg(script)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->xr);
    CHECK(run.report->xr->uiClicks == 1);
    REQUIRE(run.report->wizard);
    CHECK(run.report->wizard->page == "detection");
}

// By hand, for a look: rv_smoke_tests "[wizard_shots]" with RV_SHOTS_DIR naming a folder writes each page, the folder
// browser and an error, hidden and frame-limited, the pad's actions alone. Not a check: visual verification.
TEST_CASE("wizard: the pages' pictures", "[.shots][.wizard_shots]") {
    const char* folder = std::getenv("RV_SHOTS_DIR");
    if (folder == nullptr) {
        SKIP("RV_SHOTS_DIR names no folder");
    }
    const fs::path out = rv::core::pathFromUtf8(folder);
    fs::create_directories(out);
    const fs::path base = freshCaseDir("wizard/shots");
    const rv::test::Fixture fixture = rv::test::Fixture::materialize("esde/building").value();
    int number = 0;
    const auto shoot = [&](std::string_view name, const Script& script, std::string_view settings = {},
                           const std::vector<std::string>& extra = {}) {
        const fs::path dir = base / fmt::format("{:02}_{}", ++number, name);
        fs::create_directories(dir);
        if (!settings.empty()) {
            REQUIRE(rv::core::writeTextFileAtomic(dir / "data" / "settings.json", std::string(settings)).has_value());
        }
        const fs::path file = dir / "script.json";
        script.write(file);
        std::vector<std::string> arguments =
            wizardArguments(fixture, script.frame + 4, out / (std::string(name) + ".png"), file);
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        const AppRun run = runApp(retroverseExe(), dir, "run", std::move(arguments));
        requireCleanRun(run);
        INFO(run.describe());
        REQUIRE(run.report->wizard);
        CHECK(run.report->wizard->textCut == 0);
        CHECK(run.report->wizard->browserTextCut == 0);
    };
    const auto all = [&](std::string_view prefix, std::string_view settings, const std::vector<std::string>& extra) {
        const std::string name(prefix);
        shoot(name + "1_welcome", Script{}.wait(3), settings, extra);
        shoot(name + "2_detection", Script{}.forward(1).wait(3), settings, extra);
        shoot(name + "3_preview", Script{}.forward(2).wait(3), settings, extra);
        shoot(name + "4_import", Script{}.forward(3).wait(3), settings, extra);
        shoot(name + "5_emulators", Script{}.forward(4).wait(3), settings, extra);
        shoot(name + "6_appearance", Script{}.forward(5).wait(3), settings, extra);
        shoot(name + "7_world", Script{}.forward(6).wait(3), settings, extra);
        shoot(name + "browser", Script{}.forward(1).wait(3).press("ui_up", 4).press("ui_activate").wait(3), settings, extra);
    };
    all("", {}, {});
    all("scale125_", R"({"schemaVersion": 1, "player": {"textScale": 1.25}})",
        {"--width", "1280", "--height", "720", "--screenshot-size", "1280x720"});
    // The errors: the ROM folder moved away, page 2's problem and the browser over what vanished.
    fs::rename(fixture.path("roms"), fixture.path("moved"));
    shoot("error_rom_root", Script{}.forward(1).wait(3));
    shoot("error_browser", Script{}.forward(1).wait(3).press("ui_down", 2).press("ui_activate").wait(3));
    fs::rename(fixture.path("moved"), fixture.path("roms"));
}
