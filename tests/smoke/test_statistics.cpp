// The statistics screen and Informazioni's rows (I7 step 6, ADR-0018 §9, §11, §12, §15), driven by the pad's actions alone
// through input scripts on the snes-room fixture, read from the run report, the catalog database and retroverse-cli: another
// version launched from Informazioni, its session under its own GameId, the screen counting it and saying *stimate*; a
// title's emulator stored by its launch test; the status stepped; a catalog with nothing played, no catalog at all; no text
// cut at 125% at 1280 x 720; the ray in a headset. The guard lists the build's fake alone (ctest ENVIRONMENT).

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
#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/catalog/game_status.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/core/time.hpp>
#include <rv/core/utf8.hpp>

#include "launch_support.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

/// When the room's plan is there (the versions come from it).
constexpr int kRoomReady = 150;
constexpr int kGap = 3;

fs::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

struct Script {
    int frame = 10;
    Json steps = Json::array();

    Script& press(std::string_view action, int times = 1) {
        for (int index = 0; index < times; ++index) {
            steps.push_back(Json{{"fromFrame", frame}, {"toFrame", frame}, {"actions", {{std::string(action), 1}}}});
            frame += kGap;
        }
        return *this;
    }
    Script& type(std::string_view text) {
        steps.push_back(Json{{"fromFrame", frame}, {"toFrame", frame}, {"text", std::string(text)}});
        frame += kGap;
        return *this;
    }
    Script& wait(int frames) {
        frame += frames;
        return *this;
    }
    /// From the world into the statistics screen.
    Script& openStatistics() { return press("menu").press("ui_down", 3).press("ui_activate").wait(4); }
    /// In the room: Informazioni of the first Cobalt Compass from the quick catalog, its rows answered.
    Script& openInfo() {
        frame = std::max(frame, kRoomReady);
        press("quick_catalog").press("ui_up", 2).type("cobalt").press("ui_activate").press("ui_info").wait(6);
        return *this;
    }
    /// On Informazioni: down past the text to the row `index` (0: Stato, 1: Emulatore, 2: the first version).
    Script& toRow(int index) { return press("ui_end").press("ui_down", index + 1); }

    void write(const fs::path& file) const {
        REQUIRE(rv::core::writeTextFileAtomic(file, steps.dump(2) + "\n").has_value());
    }
};

/// The snes-room fixture imported into <case>/data, two profiles running the build's fake, snes on the first.
struct Room {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit Room(std::string_view name)
        : fixture(rv::test::Fixture::materialize("esde/snes-room").value()),
          dir(freshCaseDir(std::string("statistics/") + std::string(name))), data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
        const auto profile = [](std::string_view id) {
            return Json{{"id", id},
                        {"executable", arg(fakeEmulatorExe())},
                        {"arguments", {"--sleep-ms", "2600", "--exit-timeout-ms", "30000", "--", "{rom}"}},
                        {"windowTimeoutSeconds", 0},
                        {"hideConsoleWindow", true},
                        {"systems", {{"snes", Json::object()}}}};
        };
        const Json document{{"schemaVersion", 1},
                            {"profiles", Json::array({profile("fake-return"), profile("fake-title")})}};
        REQUIRE(rv::core::writeTextFileAtomic(data / "config" / "emulators.json", document.dump(2) + "\n").has_value());
        cli("assign", {"assign", "snes", "--profile", "fake-return"});
    }

    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }

    [[nodiscard]] Json stats() const {
        const fs::path out = dir / "stats.json";
        cli("stats", {"stats", "--json", "--output", arg(out)});
        return Json::parse(rv::core::readTextFile(out).value());
    }

    [[nodiscard]] rv::catalog_db::Database open() const {
        rv::catalog_db::DatabaseOptions options;
        options.create = false;
        options.switchToWal = false;
        auto database = rv::catalog_db::Database::open(data / "database" / "retroverse.sqlite", options);
        REQUIRE(database.has_value());
        return std::move(*database);
    }

    [[nodiscard]] std::vector<rv::catalog_db::ListedSession> sessions() const {
        auto database = open();
        auto reader = database.openReader();
        REQUIRE(reader.has_value());
        auto store = rv::catalog_db::SessionStore::open(*reader);
        REQUIRE(store.has_value());
        return (*store)->sessions(rv::catalog_db::SessionFilter{}).value();
    }

    AppRun inRoom(std::string_view name, const Script& script, std::vector<std::string> extra = {}) const {
        const fs::path file = dir / (std::string(name) + ".script.json");
        script.write(file);
        std::vector<std::string> arguments{"--start-room", "room:snes", "--exit-after-frames", "--frames",
                                           std::to_string(script.frame + 4), "--fixed-dt", "0.016667", "--input-script",
                                           arg(file)};
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        return runApp(retroverseExe(), dir, name, std::move(arguments));
    }

    AppRun inScene(std::string_view name, const Script& script, std::vector<std::string> extra = {}) const {
        const fs::path file = dir / (std::string(name) + ".script.json");
        script.write(file);
        std::vector<std::string> arguments{"--scene", "m0_testroom", "--exit-after-frames", "--frames",
                                           std::to_string(script.frame + 4), "--fixed-dt", "0.016667", "--input-script",
                                           arg(file)};
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        return runApp(retroverseExe(), dir, name, std::move(arguments));
    }

    /// Sessions of every game: returned, different lengths, one crash, written as the app would.
    void seed() const {
        auto database = open();
        auto ids = database.writer().prepare("SELECT id FROM idx_games WHERE state = 'game' ORDER BY id");
        REQUIRE(ids.has_value());
        std::vector<std::uint64_t> games;
        while (ids->step().value()) {
            games.push_back(static_cast<std::uint64_t>(ids->columnInt(0)));
        }
        REQUIRE_FALSE(games.empty());
        auto store = rv::catalog_db::SessionStore::open(database.writer());
        REQUIRE(store.has_value());
        const auto start = rv::core::parseUtcIso8601("2026-10-17T12:00:00.000Z").value();
        int minutes = 40;
        for (const std::uint64_t game : games) {
            rv::catalog::PlaySession session;
            session.game = rv::catalog::GameId::fromValue(game).value();
            session.choice = rv::launcher::LaunchChoice{rv::launcher::LaunchChoiceKind::Profile, "fake-return"};
            session.choiceSource = rv::catalog::ChoiceSource::RetroverseSystem;
            session.started = start + std::chrono::hours(minutes);
            session.ended = session.started + std::chrono::minutes(minutes);
            session.outcome = rv::catalog::PlaySessionOutcome::Returned;
            session.owner = rv::catalog::SessionOwner{4242, 1};
            REQUIRE((*store)->insertSession(session).has_value());
            minutes += 25;
        }
        rv::catalog::PlaySession crashed;
        crashed.game = rv::catalog::GameId::fromValue(games.front()).value();
        crashed.choice = rv::launcher::LaunchChoice{rv::launcher::LaunchChoiceKind::Profile, "fake-return"};
        crashed.choiceSource = rv::catalog::ChoiceSource::RetroverseSystem;
        crashed.started = start;
        crashed.ended = start + std::chrono::minutes(3);
        crashed.outcome = rv::catalog::PlaySessionOutcome::Crashed;
        crashed.exitCode = 3221225477u;
        crashed.exitKind = rv::launcher::ExitKind::Crash;
        crashed.owner = rv::catalog::SessionOwner{4242, 1};
        REQUIRE((*store)->insertSession(crashed).has_value());
    }
};

std::vector<std::string> eventsNamed(const rv::app::GameScreensRecord& screens, std::string_view name) {
    std::vector<std::string> details;
    for (const rv::app::GameScreenEventRecord& event : screens.events) {
        if (event.event == name) {
            details.push_back(event.detail);
        }
    }
    return details;
}

} // namespace

TEST_CASE("statistics: another version launched from Informazioni; the screen counts it and says stimate",
          "[gpu][statistics]") {
    ensureLaunchAllowList();
    const Room room("version");
    Script script;
    script.openInfo().toRow(2).press("ui_activate").wait(8).press("ui_back");
    const AppRun launched = room.inRoom("launched", script);
    skipWithoutOpenGl(launched);
    requireCleanRun(launched);
    INFO(launched.describe());

    const std::vector<std::string> versions = eventsNamed(launched.report->screens, "version-launch");
    REQUIRE(versions.size() == 1);
    REQUIRE(launched.report->launches.size() == 1);
    CHECK(launched.report->launches.front().state == "Returned");
    const auto sessions = room.sessions();
    REQUIRE(sessions.size() == 1);
    CHECK(sessions.front().session.game.toString() == versions.front()); // under its own GameId
    CHECK_FALSE(sessions.front().session.test);
    CHECK(sessions.front().session.outcome == rv::catalog::PlaySessionOutcome::Returned);
    // Not the game Informazioni was about.
    CHECK(launched.report->screens.events.front().game != versions.front());

    Script read;
    read.openStatistics().press("ui_filter_next", 4).press("ui_down").press("ui_back").press("ui_back");
    const AppRun shown = room.inScene("shown", read);
    requireCleanRun(shown);
    INFO(shown.describe());
    REQUIRE(shown.report->statistics);
    const rv::app::StatisticsRecord& statistics = *shown.report->statistics;
    CHECK(statistics.opened == 1);
    CHECK_FALSE(statistics.open);
    CHECK(statistics.state == "listed");
    CHECK(statistics.sessions == 1);
    CHECK(statistics.gamesPlayed == 1);
    CHECK(statistics.playedMs >= 2000);
    CHECK(statistics.hoursLabel.find("stimate") != std::string::npos);
    CHECK(statistics.tabs == std::vector<std::string>{"summary", "most-played", "recent", "systems", "statuses"});
    REQUIRE(statistics.mostPlayed.size() == 1);
    CHECK(statistics.mostPlayed.front().find("Cobalt") != std::string::npos);
    CHECK(statistics.systems == std::vector<std::string>{"snes=1"});
    CHECK(statistics.textCut == 0);
    CHECK(statistics.readMs < 1000.0);
    CHECK(shown.report->launches.empty());

    // retroverse-cli says the same.
    const Json stats = room.stats();
    CHECK(stats.at("sessions") == 1);
    CHECK(stats.at("playedMs") == statistics.playedMs);
    CHECK(stats.at("mostPlayed").at(0).at("id") == versions.front());
    CHECK(stats.at("mostPlayed").at(0).at("status") == "in-progress");
}

TEST_CASE("statistics: a title's emulator stored by its launch test, which counts for nothing", "[gpu][statistics]") {
    ensureLaunchAllowList();
    const Room room("title");
    Script open;
    open.openInfo().toRow(1).press("ui_activate").wait(8);
    const AppRun probe = room.inRoom("probe", open);
    skipWithoutOpenGl(probe);
    requireCleanRun(probe);
    INFO(probe.describe());
    REQUIRE(probe.report->emulators);
    CHECK(probe.report->emulators->view == "system");
    const auto system = std::find_if(probe.report->emulators->systems.begin(), probe.report->emulators->systems.end(),
                                     [](const auto& row) { return row.name == "snes"; });
    REQUIRE(system != probe.report->emulators->systems.end());
    const auto option = std::find(system->options.begin(), system->options.end(), "fake-title=ready");
    REQUIRE(option != system->options.end());

    Script script = open;
    script.press("ui_down", static_cast<int>(option - system->options.begin())).press("ui_activate").wait(3);
    script.press("ui_activate").wait(8); // Prova: the fake runs, returns
    script.press("ui_back").wait(6);     // Back: Informazioni again, its row answered
    const AppRun run = room.inRoom("run", script);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->emulators);
    CHECK(run.report->emulators->testsStored == 1);
    CHECK(run.report->emulators->verdict == "verified");
    CHECK_FALSE(run.report->emulators->open);

    const auto sessions = room.sessions();
    REQUIRE(sessions.size() == 1);
    CHECK(sessions.front().session.test);
    CHECK(sessions.front().session.choice.value == "fake-title");
    const rv::catalog::GameId game = sessions.front().session.game;
    // Informazioni came back on the same game, naming the title's choice.
    const std::vector<std::string> controls = eventsNamed(run.report->screens, "controls");
    REQUIRE_FALSE(controls.empty());
    CHECK(controls.back().ends_with(" own"));
    CHECK(run.report->screens.events.back().game == game.toString());
    {
        auto database = room.open();
        auto reader = database.openReader();
        REQUIRE(reader.has_value());
        auto store = rv::catalog_db::SessionStore::open(*reader);
        REQUIRE(store.has_value());
        CHECK((*store)->gameChoice(game).value()->choice.value == "fake-title");
        // The system's stays: the choice is the title's alone.
        CHECK((*store)->systemChoices(rv::catalog::LibraryId{}).value().front().stored.choice.value == "fake-return");
    }
    // A launch test is no session of the statistics.
    CHECK(room.stats().at("sessions") == 0);
}

TEST_CASE("statistics: the status steps from Informazioni and stays", "[gpu][statistics]") {
    ensureLaunchAllowList();
    const Room room("status");
    Script script;
    script.openInfo().toRow(0).press("ui_activate").press("ui_right").press("ui_left").press("ui_activate").wait(6);
    script.press("ui_up").press("ui_back");
    const AppRun run = room.inRoom("run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    // From the derived Da iniziare: In corso, Completato, back to In corso, Completato.
    CHECK(eventsNamed(run.report->screens, "status") ==
          std::vector<std::string>{"in-progress", "completed", "in-progress", "completed"});
    const std::vector<std::string> rows = eventsNamed(run.report->screens, "row");
    REQUIRE(rows.size() == 2);
    CHECK(rows[0] == "status");
    CHECK(rows[1] == "text"); // Up from the first row: the text again
    CHECK(run.report->launches.empty());
    CHECK(run.report->screens.infoTextCut == 0);

    const Json stats = room.stats();
    CHECK(stats.at("statuses").at(2).at("status") == "completed");
    CHECK(stats.at("statuses").at(2).at("games") == 1);
}

TEST_CASE("statistics: nothing played, and no catalog at all", "[gpu][statistics]") {
    ensureLaunchAllowList();
    const Room room("empty");
    {
        // The fixture's <lastplayed> dates are games played in ES-DE, which the recent games show.
        auto database = room.open();
        REQUIRE(database.writer().execute("UPDATE idx_games SET last_played = NULL").has_value());
    }
    const AppRun played = room.inScene("played", Script{}.openStatistics().press("ui_filter_next", 2));
    skipWithoutOpenGl(played);
    requireCleanRun(played);
    INFO(played.describe());
    REQUIRE(played.report->statistics);
    CHECK(played.report->statistics->state == "empty");
    CHECK(played.report->statistics->sessions == 0);
    CHECK(played.report->statistics->games > 0);
    CHECK(played.report->statistics->textCut == 0);

    const fs::path dir = freshCaseDir("statistics/none");
    Script script;
    script.openStatistics().press("ui_back").wait(3);
    const fs::path file = dir / "script.json";
    script.write(file);
    const AppRun none =
        runApp(retroverseExe(), dir, "none",
               {"--scene", "m0_testroom", "--exit-after-frames", "--frames", std::to_string(script.frame + 4),
                "--fixed-dt", "0.016667", "--input-script", arg(file)});
    requireCleanRun(none);
    INFO(none.describe());
    REQUIRE(none.report->statistics);
    CHECK(none.report->statistics->state == "unavailable");
    CHECK_FALSE(none.report->statistics->open);
    CHECK(none.report->statistics->sessions == 0);
}

TEST_CASE("statistics: at text scale 125% at 1280 x 720 nothing is cut", "[gpu][statistics]") {
    ensureLaunchAllowList();
    const Room room("scale125");
    room.seed();
    REQUIRE(rv::core::writeTextFileAtomic(room.data / "settings.json",
                                          R"({"schemaVersion": 1, "player": {"textScale": 1.25}})")
                .has_value());
    Script script;
    script.openStatistics().press("ui_filter_next").wait(2).press("ui_filter_next").wait(2);
    script.press("ui_filter_next").wait(2).press("ui_filter_next").wait(2);
    const AppRun run = room.inScene("run", script, {"--width", "1280", "--height", "720"});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->statistics);
    CHECK(run.report->statistics->state == "listed");
    CHECK(run.report->statistics->crashed == 1);
    CHECK(run.report->statistics->textCut == 0);

    Script info;
    info.openInfo().toRow(2);
    const AppRun rows = room.inRoom("rows", info, {"--width", "1280", "--height", "720"});
    requireCleanRun(rows);
    INFO(rows.describe());
    CHECK(rows.report->screens.infoFrames > 0);
    CHECK(rows.report->screens.infoTextCut == 0);
}

TEST_CASE("statistics: in a headset the panel shows and the ray changes the section", "[gpu][statistics][xr]") {
    const Room room("vr");
    room.seed();
    // The right controller at the hip points at "Recenti", the third of five sections in the panel's upper part (the canvas
    // stands 1.3 m in front of the eyes, 0.12 m below them, 1.8 m wide at the window's 16:9).
    const glm::vec3 hand(0.2f, 1.25f, -0.3f);
    const glm::vec3 target(0.0f, 1.65f - 0.12f + 0.33f, -1.3f);
    const glm::vec3 toTarget = glm::normalize(target - hand);
    const float yawDeg = rv::core::radiansToDegrees(std::atan2(toTarget.x, -toTarget.z));
    const float pitchDeg = rv::core::radiansToDegrees(std::asin(toTarget.y));
    const std::string pose = fmt::format(R"("rightHand": {{"yaw": {}, "pitch": {}, "position": [{}, {}, {}]}})", yawDeg,
                                         pitchDeg, hand.x, hand.y, hand.z);
    const fs::path script = room.dir / "vr.json";
    REQUIRE(rv::core::writeTextFileAtomic(script, fmt::format(R"([
  {{"fromFrame": 0, "toFrame": 79, "headPose": {{"position": [0, 1.65, 0]}}}},
  {{"fromFrame": 0, "toFrame": 79, {0}}},
  {{"fromFrame": 10, "toFrame": 10, "actions": {{"menu": 1}}}},
  {{"fromFrame": 13, "toFrame": 13, "actions": {{"ui_down": 1}}}},
  {{"fromFrame": 16, "toFrame": 16, "actions": {{"ui_down": 1}}}},
  {{"fromFrame": 19, "toFrame": 19, "actions": {{"ui_down": 1}}}},
  {{"fromFrame": 22, "toFrame": 22, "actions": {{"ui_activate": 1}}}},
  {{"fromFrame": 50, "toFrame": 53, "rightSelect": 1}}
])",
                                                              pose))
                .has_value());
    const fs::path eyes = room.dir / "eyes.png";
    const AppRun run = runApp(retroverseExe(), room.dir, "run",
                              {"--scene", "m0_testroom", "--vr", "--xr-runtime", "fake", "--exit-after-frames",
                               "--frames", "80", "--fixed-dt", "0.016667", "--screenshot", arg(eyes), "--input-script",
                               arg(script)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->xr);
    CHECK(run.report->xr->uiClicks == 1);
    REQUIRE(run.report->statistics);
    CHECK(run.report->statistics->clicks == 1);
    CHECK(run.report->statistics->tab == "recent");
}

TEST_CASE("statistics: in a headset the ray presses Informazioni's Stato", "[gpu][statistics][xr]") {
    const Room room("vr_info");
    // The row stands at the panel's foot, in the middle of the canvas's width (as above).
    const glm::vec3 hand(0.2f, 1.25f, -0.3f);
    const glm::vec3 target(0.0f, 1.65f - 0.12f - 0.22f, -1.3f);
    const glm::vec3 toTarget = glm::normalize(target - hand);
    const float yawDeg = rv::core::radiansToDegrees(std::atan2(toTarget.x, -toTarget.z));
    const float pitchDeg = rv::core::radiansToDegrees(std::asin(toTarget.y));
    Script script;
    script.press("quick_catalog").press("ui_up", 2).type("cobalt").press("ui_activate").press("ui_info").wait(14);
    const int click = script.frame;
    script.steps.push_back(Json{{"fromFrame", click}, {"toFrame", click + 3}, {"rightSelect", 1}});
    script.wait(14);
    script.steps.push_back(Json{{"fromFrame", 0}, {"toFrame", script.frame}, {"headPose", {{"position", {0, 1.65, 0}}}}});
    script.steps.push_back(
        Json{{"fromFrame", 0},
             {"toFrame", script.frame},
             {"rightHand", {{"yaw", yawDeg}, {"pitch", pitchDeg}, {"position", {hand.x, hand.y, hand.z}}}}});
    const fs::path file = room.dir / "vr.json";
    script.write(file);
    const AppRun run = runApp(retroverseExe(), room.dir, "run",
                              {"--scene", "m0_testroom", "--vr", "--xr-runtime", "fake", "--exit-after-frames",
                               "--frames", std::to_string(script.frame + 4), "--fixed-dt", "0.016667", "--screenshot",
                               arg(room.dir / "eyes.png"), "--input-script", arg(file)});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->xr);
    CHECK(run.report->xr->uiClicks == 1);
    CHECK(eventsNamed(run.report->screens, "status") == std::vector<std::string>{"in-progress"});
}

// By hand, for a look: rv_smoke_tests "[statistics_shots]" with RV_SHOTS_DIR naming a folder writes each section and
// Informazioni, at 1920 x 1080 at 100% and 125%, hidden and frame-limited, the pad's actions alone. Not a check: visual
// verification.
TEST_CASE("statistics: the pictures", "[.shots][.statistics_shots]") {
    const char* folder = std::getenv("RV_SHOTS_DIR");
    if (folder == nullptr) {
        SKIP("RV_SHOTS_DIR names no folder");
    }
    ensureLaunchAllowList();
    const fs::path out = rv::core::pathFromUtf8(folder);
    fs::create_directories(out);
    const Room room("shots");
    room.seed();
    const auto all = [&](std::string_view prefix) {
        const std::vector<std::string> size{"--width", "1920", "--height", "1080"};
        const auto shoot = [&](std::string_view name, const Script& script, bool scene) {
            const std::string run = fmt::format("{}{}", prefix, name);
            std::vector<std::string> extra = size;
            extra.insert(extra.end(), {"--screenshot", arg(out / (run + ".png"))});
            const AppRun result = scene ? room.inScene(run, script, extra) : room.inRoom(run, script, extra);
            requireCleanRun(result);
            INFO(result.describe());
            if (result.report->statistics) {
                CHECK(result.report->statistics->textCut == 0);
            }
            CHECK(result.report->screens.infoTextCut == 0);
        };
        shoot("menu", Script{}.press("menu").wait(4), true);
        shoot("summary", Script{}.openStatistics(), true);
        shoot("most_played", Script{}.openStatistics().press("ui_filter_next"), true);
        shoot("recent", Script{}.openStatistics().press("ui_filter_next", 2), true);
        shoot("systems", Script{}.openStatistics().press("ui_filter_next", 3), true);
        shoot("statuses", Script{}.openStatistics().press("ui_filter_next", 4), true);
        shoot("info_status", Script{}.openInfo().toRow(0), false);
        shoot("info_version", Script{}.openInfo().toRow(2), false);
        shoot("info_emulators", Script{}.openInfo().toRow(1).press("ui_activate").wait(8), false);
    };
    all("");
    REQUIRE(rv::core::writeTextFileAtomic(room.data / "settings.json",
                                          R"({"schemaVersion": 1, "player": {"textScale": 1.25}})")
                .has_value());
    all("scale125_");
}
