// Launches from the quick catalog of retroverse.exe (I2 step A1): the mock catalog of tests/smoke/catalogs, games
// launched through their emulator profiles into tests/fake_emulator's rv_fake_emulator with the automation guard on
// (RV_LAUNCH_ALLOWLIST lists the fake alone), driven by input scripts (UI actions: open the catalog, activate). The run
// reports tell what happened: the launch's state and outcome, the frames (none rendered while the game ran: the host
// waits), the catalog and the walker after the return (the application still answers its input).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/text.hpp>
#include <rv/screenshot_diff/screenshot_diff.hpp>

#include "launch_support.hpp"
#include "smoke_support.hpp"

using namespace rv::smoke;
using namespace std::chrono_literals;

namespace {

/// Where the walker starts (feet, yaw, pitch: the walk scripts' entrance of the test room); move_forward then walks
/// towards -Z.
constexpr std::string_view kSpawn = "0,0,3.2,0,-6";
constexpr float kSpawnZ = 3.2f;
/// The launch scripts' frames: the run ends on the 50th.
constexpr std::string_view kFrames = "50";

/// The visible case runs only when its ctest registration asks for it (RV_SMOKE_DESKTOP=1 in its environment).
bool desktopRunAllowed() {
    char value[8] = {};
    std::size_t length = 0;
    return getenv_s(&length, value, sizeof(value), "RV_SMOKE_DESKTOP") == 0 && std::string_view(value) == "1";
}

/// The arguments of a launch run: the catalog, the script, 50 frames at a fixed step from the spawn point.
std::vector<std::string> launchArguments(const std::filesystem::path& catalog, std::string_view script) {
    return {"--mock-catalog", arg(catalog),     "--input-script", arg(inputScript(script)), "--exit-after-frames",
            "--frames",       std::string(kFrames), "--fixed-dt", "0.016667",             "--spawn",
            std::string(kSpawn)};
}

/// Runs the first game of a catalog of `games` with `script`, in the case directory `name`, with `extra` arguments.
AppRun runLaunch(std::string_view name, const std::vector<std::string_view>& games,
                 std::string_view script = "quick_catalog_launch.json", const std::vector<std::string>& extra = {}) {
    ensureLaunchAllowList();
    const std::filesystem::path dir = freshCaseDir(std::string("launch/") + std::string(name));
    const std::filesystem::path catalog = writeFakeCatalog(dir, games);
    std::vector<std::string> arguments = launchArguments(catalog, script);
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    AppRun run = runApp(retroverseExe(), dir, name, std::move(arguments));
    skipWithoutOpenGl(run);
    return run;
}

/// What every launch run must show: a clean run of all its frames, one launch of `game`, no frame rendered while the
/// emulator had the screen (`whileRunning` with a choice to go back to the game: the frame of its notice), the catalog
/// opened once and closed again, the walker moved after the return (the input reached the world again).
const rv::app::LaunchRecord& requireLaunch(const AppRun& run, std::string_view game, std::uint64_t whileRunning = 0) {
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    REQUIRE(report.catalog.has_value());
    CHECK(report.catalog->source == "mock");
    CHECK(report.catalog->opened == 1);
    CHECK_FALSE(report.catalog->open);
    CHECK(report.catalog->selectedId == game);
    REQUIRE(report.launches.size() == 1);
    const rv::app::LaunchRecord& launch = report.launches.front();
    CHECK(launch.gameId == game);
    CHECK(launch.requestFrame == 8);
    CHECK(launch.framesWhileRunning == whileRunning);
    REQUIRE(report.finalCamera.has_value());
    CHECK(report.finalCamera->feet.z < kSpawnZ - 0.5f);
    return launch;
}

} // namespace

TEST_CASE("launch: a game launched from the quick catalog returns, and no frame runs while it plays",
          "[gpu][launch]") {
    const AppRun run = runLaunch("returns", {fake_games::kReturns});
    const rv::app::LaunchRecord& launch = requireLaunch(run, fake_games::kReturns);
    CHECK(launch.profile == "fake-return");
    CHECK(launch.state == "Returned");
    CHECK(launch.outcome == "returned");
    CHECK(launch.exitCode == 0u);
    CHECK(launch.strategy == "DirectProcess");
    CHECK_FALSE(launch.quickExit);
    CHECK_FALSE(launch.processesRemained);
    CHECK(launch.panels.empty());
    // The frame that asked was presented, then the host waited: the game started and ended between frames 8 and 9.
    CHECK(launch.startFrame == 9u);
    CHECK(launch.endFrame == 9u);
    CHECK(launch.framesWithPanel == 0);
    // It ran its 2.5 s, which the frame budget did not pay for (the run still rendered its 50 frames), and the host
    // woke to follow it.
    REQUIRE(launch.runMs.has_value());
    CHECK(*launch.runMs >= 2400.0);
    CHECK(launch.idleWakes >= 1);
    CHECK(run.report->framesRendered == 50);

    // The fake received the ROM as one argument, in the ROM's folder (the profile's working directory).
    const std::filesystem::path dir = run.reportFile.parent_path();
    const auto printed = rv::core::readJsonFile(dir / "roms" / "fake-return-args.json");
    REQUIRE(printed.has_value());
    const std::vector<std::string> payload = printed->at("payload").get<std::vector<std::string>>();
    REQUIRE(payload.size() == 2);
    CHECK(rv::core::pathFromUtf8(payload[0]) == (dir / "roms" / "lanterna.gb").lexically_normal());
    CHECK(payload[1] == "gb");
}

TEST_CASE("launch: a pad lost during a game says so on the return (FR-029)", "[gpu][launch][controls]") {
    // No frame runs while the fake plays: the script's pad is there up to the launch's frame, gone at the first after it.
    const AppRun run = runLaunch("pad_lost", {fake_games::kReturns}, "quick_catalog_launch_pad_lost.json");
    const rv::app::LaunchRecord& launch = requireLaunch(run, fake_games::kReturns);
    CHECK(launch.state == "Returned");
    REQUIRE(run.report->controls);
    CHECK(run.report->controls->padsLost == 1);
    CHECK(run.report->controls->padsBack == 0);
    CHECK(run.report->controls->noticeShown);
    CHECK(run.report->controls->noticeFrames > 0);
}

TEST_CASE("launch: failures leave the application interactive with their outcome", "[gpu][launch]") {
    struct Case {
        std::string_view name;
        std::string_view game;
        std::string_view state;
        std::string_view outcome;
        std::string_view reason;
        /// The detail, or a part of it when not `exact` (the guard's refusal names a path of this machine).
        std::string_view detail;
        bool exact = true;
    };
    const Case cases[] = {
        {"quick_exit", fake_games::kQuickExit, "Returned", "quick-exit", "", "0"},
        {"error_exit", fake_games::kErrorExit, "FailedToStart", "failed-to-start", "early-exit", "3"},
        {"crash", fake_games::kCrashes, "Crashed", "crashed", "", "0xC0000005 STATUS_ACCESS_VIOLATION"},
        {"not_listed", fake_games::kNotListed, "FailedToStart", "failed-to-start", "guard-refused",
         "RV_LAUNCH_ALLOWLIST", false},
        {"emulator_missing", fake_games::kMissingEmulator, "Blocked", "emulator-missing", "", "Emulatore assente"},
        {"rom_missing", fake_games::kRomMissing, "Blocked", "rom-missing", "", "assente.gb"},
        {"not_configured", fake_games::kUnsetVariable, "Blocked", "not-configured", "template", "{var:core}"},
        {"no_launch", fake_games::kNoLaunch, "Blocked", "not-configured", "no-launch", ""},
        // A wrapper exits at once while the emulator it started, its child, runs on: told, never ended.
        {"processes_remained", fake_games::kWrapper, "Returned", "processes-remained", "", "0"},
    };
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            const AppRun run = runLaunch(c.name, {c.game});
            const rv::app::LaunchRecord& launch = requireLaunch(run, c.game);
            CHECK(launch.state == c.state);
            CHECK(launch.outcome == c.outcome);
            CHECK(launch.reason == c.reason);
            INFO("detail: " << launch.detail);
            if (c.exact) {
                CHECK(launch.detail == c.detail);
            } else {
                CHECK(launch.detail.find(c.detail) != std::string::npos);
            }
            if (c.outcome == "processes-remained") {
                CHECK(launch.processesRemained);
            }
            // The outcome's message showed until Back dismissed it; the next Back left the catalog.
            CHECK(launch.panels == std::vector<std::string>{"outcome"});
            if (c.state != "Blocked") {
                CHECK(launch.startFrame == 9u);
                CHECK(launch.endFrame == 9u);
            } else {
                CHECK_FALSE(launch.startFrame.has_value()); // nothing started
            }
        }
    }
}

TEST_CASE("launch: the game-session panel asks the user, whose answer ends the session", "[gpu][launch]") {
    SECTION("no window for the window timeout: the user returns, the game left running") {
        const AppRun run = runLaunch("still_running", {fake_games::kStillRunning}, "quick_catalog_launch_confirm.json");
        const rv::app::LaunchRecord& launch = requireLaunch(run, fake_games::kStillRunning);
        CHECK(launch.state == "Returned");
        CHECK(launch.outcome == "left-running");
        CHECK(launch.reason == "confirmed");
        CHECK(launch.wasStillRunning);
        CHECK(launch.processesRemained);
        CHECK(launch.confirmedByUser);
        CHECK(launch.panels == std::vector<std::string>{"still-running"});
        // The panel rendered frames 9 to 13; the choice on frame 14 ended the session.
        CHECK(launch.startFrame == 9u);
        CHECK(launch.endFrame == 14u);
        CHECK(launch.framesWithPanel == 5);
        // Left running, never ended: the fake ends by itself (its 6 s), not by Retroverse. On a loaded machine the run
        // can outlast those 6 s, so the process may be gone before it is looked for: its log, written only at a normal
        // end, then tells how it ended.
        const std::optional<std::uint32_t> exitCode = waitForExit(launch.processId, 40s);
        if (exitCode.has_value()) {
            CHECK(*exitCode == 0u);
        } else {
            const std::filesystem::path logFile = run.reportFile.parent_path() / "roms" / "fake-still-running-log.json";
            const auto log = rv::core::readJsonFile(logFile);
            REQUIRE(log.has_value());
            CHECK((*log)["exit"] == "code");
            CHECK((*log)["exitCode"] == 0);
        }
    }
    SECTION("UserConfirmReturn: the started program ended, the user confirms the return") {
        const AppRun run = runLaunch("confirm", {fake_games::kConfirm}, "quick_catalog_launch_confirm.json");
        const rv::app::LaunchRecord& launch = requireLaunch(run, fake_games::kConfirm);
        CHECK(launch.state == "Returned");
        CHECK(launch.outcome == "returned");
        CHECK(launch.strategy == "UserConfirmReturn");
        CHECK(launch.confirmedByUser);
        CHECK(launch.exitCode == 0u);
        CHECK(launch.panels == std::vector<std::string>{"confirm"});
        CHECK(launch.endFrame == 14u);
        CHECK(launch.framesWithPanel == 5);
    }
    SECTION("UserConfirmReturn after its launcher's quick exit: the confirmed return is a plain one") {
        const AppRun run =
            runLaunch("confirm_quick", {fake_games::kConfirmQuick}, "quick_catalog_launch_confirm.json");
        const rv::app::LaunchRecord& launch = requireLaunch(run, fake_games::kConfirmQuick);
        CHECK(launch.state == "Returned");
        CHECK(launch.outcome == "returned"); // no quick-exit hint after the user said the game was over
        CHECK(launch.strategy == "UserConfirmReturn");
        CHECK(launch.quickExit);
        CHECK(launch.confirmedByUser);
        CHECK(launch.panels == std::vector<std::string>{"confirm"});
        CHECK(launch.endFrame == 14u);
    }
}

TEST_CASE("launch: Back on the game-session panel goes back to the game, and the question comes back",
          "[gpu][launch]") {
    // The hidden window counts as focused and nothing takes its focus: the question must come back by itself (a window
    // that keeps the focus is never left waiting without a way out).
    const AppRun run = runLaunch("back_to_game", {fake_games::kConfirm}, "quick_catalog_launch_back.json");
    const rv::app::LaunchRecord& launch = requireLaunch(run, fake_games::kConfirm, 1);
    CHECK(launch.state == "Returned");
    CHECK(launch.outcome == "returned");
    CHECK(launch.confirmedByUser);
    CHECK(launch.panels == std::vector<std::string>{"confirm", "confirm"});
    // Frames 9 to 13 asked, frame 14 showed the notice (Back), then the host waited until the question came back:
    // frames 15 to 19 asked again, the choice on frame 20 ended the session.
    CHECK(launch.startFrame == 9u);
    CHECK(launch.endFrame == 20u);
    CHECK(launch.framesWithPanel == 10);
}

TEST_CASE("launch: a frame-limited run stops waiting at its wait limit and leaves the game running",
          "[gpu][launch]") {
    const AppRun run =
        runLaunch("wait_limit", {fake_games::kHangs}, "quick_catalog_launch.json", {"--launch-wait-limit", "2"});
    const rv::app::LaunchRecord& launch = requireLaunch(run, fake_games::kHangs);
    CHECK(launch.state == "Returned");
    CHECK(launch.outcome == "left-running");
    CHECK(launch.reason == "wait-limit");
    CHECK(launch.waitLimitReached);
    CHECK(launch.processesRemained);
    CHECK_FALSE(launch.confirmedByUser);
    CHECK(launch.panels == std::vector<std::string>{"outcome"});
    CHECK(launch.startFrame == 9u);
    CHECK(launch.endFrame == 9u);
    REQUIRE(launch.runMs.has_value());
    CHECK(*launch.runMs >= 1900.0);
    // The run reached its frames and wrote its report; the game was left running and ends by its own exit timeout.
    CHECK(run.report->framesRendered == 50);
    REQUIRE(launch.processId != 0);
    const std::optional<std::uint32_t> exitCode = waitForExit(launch.processId, 30s);
    CHECK(exitCode == 124u);
}

TEST_CASE("launch: closing Retroverse while the game runs leaves the game running", "[gpu][launch]") {
    ensureLaunchAllowList();
    const std::filesystem::path dir = freshCaseDir("launch/quit_during_game");
    const std::filesystem::path catalog = writeFakeCatalog(dir, {fake_games::kHangs});
    StartedApp app(dir, "quit_during_game", launchArguments(catalog, "quick_catalog_launch.json"));
    // The fake writes its arguments as it starts: the game runs, Retroverse waits.
    const bool started = waitForFile(dir / "roms" / "fake-hang-args.json", 120s);
    if (!started) {
        const AppRun early = app.finish();
        skipWithoutOpenGl(early);
        FAIL("the fake emulator never started: " << early.describe());
    }
    std::this_thread::sleep_for(300ms);
    // Closing the window: what the user does with Alt+F4 or the taskbar.
    CHECK(app.closeWindow());
    const AppRun run = app.finish();
    INFO(run.describe());
    REQUIRE(run.report.has_value());
    // An --exit-after-frames run that the user ends early ends cleanly.
    CHECK(run.exitCode == 0);
    CHECK(run.report->framesRendered == 9); // frames 0 to 8; none after the launch
    REQUIRE(run.report->launches.size() == 1);
    const rv::app::LaunchRecord& launch = run.report->launches.front();
    CHECK(launch.gameId == fake_games::kHangs);
    CHECK(launch.state == "Returned");
    CHECK(launch.outcome == "left-running");
    CHECK(launch.reason == "quit");
    CHECK(launch.quitDuringGame);
    CHECK(launch.processesRemained);
    CHECK_FALSE(launch.confirmedByUser);
    CHECK(launch.startFrame == 9u);
    CHECK(launch.endFrame == 9u);
    CHECK(launch.framesWhileRunning == 0);
    REQUIRE(launch.processId != 0);
    // Retroverse is gone, the game still runs...
    CHECK(processRunning(launch.processId, "rv_fake_emulator.exe"));
    // ...until its own exit timeout ends it (12 s after it started; exit code 124).
    const std::optional<std::uint32_t> exitCode = waitForExit(launch.processId, 30s);
    CHECK(exitCode == 124u);
}

TEST_CASE("launch: a closed catalog draws nothing over the scene", "[gpu][launch]") {
    // The same frames with the mock catalog and with the ES-DE catalog of an empty data directory, neither opened: the
    // product UI adds nothing to them (golden G01's runs have the latter). The test room by --scene: without it, a run
    // of the ES-DE catalog asks for the generated room and shows why it shows the test room instead (I3 step 10b).
    ensureLaunchAllowList();
    const std::filesystem::path dir = freshCaseDir("launch/closed_catalog");
    const std::filesystem::path catalog = writeFakeCatalog(dir, {fake_games::kReturns});
    // Since I5 step 1 the CRT would show the catalog game's title card (a room without a clip shows its first game's
    // picture): the hero screen's autoplay off, so that only the product UI could tell the runs apart.
    std::filesystem::create_directories(dir / "data" / "config");
    REQUIRE(rv::core::writeTextFileAtomic(dir / "data" / "config" / "video.json", R"({"hero": {"enabled": false}})"
                                                                                  "\n")
                .has_value());
    const std::filesystem::path without = dir / "without_catalog.png";
    const std::filesystem::path with = dir / "with_catalog.png";
    const AppRun plain = runApp(retroverseExe(), dir, "without_catalog",
                                {"--scene", "m0_testroom", "--smoke", "--frames", "5", "--fixed-dt", "0.016667",
                                 "--screenshot", arg(without)});
    skipWithoutOpenGl(plain);
    requireCleanRun(plain);
    const AppRun withCatalog =
        runApp(retroverseExe(), dir, "with_catalog",
               {"--mock-catalog", arg(catalog), "--smoke", "--frames", "5", "--fixed-dt", "0.016667", "--screenshot",
                arg(with)});
    requireCleanRun(withCatalog);
    // Without --mock-catalog the run's catalog is the ES-DE catalog of its data directory, which has none.
    REQUIRE(plain.report->catalog.has_value());
    CHECK(plain.report->catalog->source == "esde");
    CHECK(plain.report->catalog->notice == "no-library");
    CHECK(plain.report->catalog->opened == 0);
    REQUIRE(withCatalog.report->catalog.has_value());
    CHECK(withCatalog.report->catalog->opened == 0);
    CHECK(withCatalog.report->launches.empty());
    const rv::screenshot_diff::Tolerance tolerance;
    const auto comparison = rv::screenshot_diff::compareFiles(without, with, tolerance, dir / "heatmap.png");
    REQUIRE(comparison.has_value());
    INFO(comparison->describe(tolerance));
    CHECK(comparison->differingPixels == 0);
}

// A visible run (label "desktop", on demand only: it shows fullscreen windows and moves the foreground): Retroverse in
// borderless fullscreen launches the fake with a fullscreen window of its own, keeps its window behind it without
// rendering, and takes the foreground back when the game ends (ADR-0006, the keep policy). Hidden, outside the launch
// cases' names and tags, and skipped unless the ctest registration desktop.smoke.launch_visible sets RV_SMOKE_DESKTOP=1:
// no filter typed by hand ("launch*", "[launch]") can take over the screen.
TEST_CASE("desktop: a visible launch hands the window over and takes it back", "[.desktop]") {
    if (!desktopRunAllowed()) {
        SKIP("takes over the screen: run it through ctest (desktop.smoke.launch_visible, label desktop)");
    }
    ensureLaunchAllowList();
    const std::filesystem::path dir = freshCaseDir("launch/visible");
    const std::filesystem::path catalog = writeFakeCatalog(dir, {fake_games::kWindow});
    std::vector<std::string> arguments = launchArguments(catalog, "quick_catalog_launch.json");
    arguments.emplace_back("--fullscreen");
    StartedApp app(dir, "visible", std::move(arguments), /*visible*/ true);
    const AppRun run = app.finish();
    skipWithoutOpenGl(run);
    const rv::app::LaunchRecord& launch = requireLaunch(run, fake_games::kWindow);
    CHECK(launch.state == "Returned");
    CHECK(launch.outcome == "returned");
    CHECK(launch.startFrame == launch.endFrame);
    REQUIRE(launch.foregroundMs.has_value());
    INFO("back in the foreground " << *launch.foregroundMs << " ms after the exit");
    CHECK(*launch.foregroundMs < 2000.0);
    // The fake had the foreground while it ran.
    const auto log = rv::core::readJsonFile(dir / "roms" / "fake-window-log.json");
    REQUIRE(log.has_value());
    CHECK(log->at("frames").get<int>() == 500);
}
