// The ES-DE catalog in retroverse.exe (I2 step 13): the standard fixture (tests/fixtures/esde/standard) recorded as the
// library of the case's data directory and imported with retroverse-cli, an emulators.json there whose profile runs the
// fake emulator of this build (tests/fake_emulator), assigned to gb with retroverse-cli assign, and psx assigned the
// fixture's "Fake Emulator (Inject)" command (Solo Disc's %INJECT% file makes that game require launcher
// configuration). Input scripts open the quick catalog, search, filter, select and launch with UI actions and typed text;
// the run reports and the catalog database tell what happened: the launch returned with its play session recorded, and
// the selection, the search, the filter and the walker's camera were the same before and after it; a blocked game shows
// its readiness and does not launch; the card of a blocked game with a description shows its text whole (no line cut by
// the layout, the run report's catalog.cardSqueezed); a data directory without library.json shows how to import. Every
// run has --fixed-dt, so its frames wait for the catalog worker (lockstep) and the frames of the scripts are
// deterministic. The automation guard lists the fake alone (RV_LAUNCH_ALLOWLIST): the fixture's own copies of it are
// never started.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/catalog/ids.hpp>
#include <rv/catalog/play_session.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/session_store.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

#include "launch_support.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;

namespace {

/// Where the walker starts (feet, yaw, pitch: the walk scripts' entrance of the test room).
constexpr std::string_view kSpawn = "0,0,3.2,0,-6";

std::filesystem::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

/// The standard fixture imported into <caseDir>/data with retroverse-cli: library.json, the catalog database, an
/// emulators.json whose "fake-return" profile runs the build's fake for 2.5 s (gb), gb assigned that profile and psx
/// the fixture's "Fake Emulator (Inject)" command. Keeps the materialized fixture for the test's duration.
struct ImportedCatalog {
    rv::test::Fixture fixture;
    std::filesystem::path dir;
    std::filesystem::path data;

    explicit ImportedCatalog(std::string_view name)
        : fixture(rv::test::Fixture::materialize("esde/standard").value()),
          dir(freshCaseDir(std::string("catalog/") + std::string(name))), data(dir / "data") {
        cli("library", {"library", "create", "--home", arg(fixture.root()), "--install",
                        arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
        const nlohmann::json profile{
            {"id", "fake-return"},
            {"executable", arg(fakeEmulatorExe())},
            {"arguments", {"--sleep-ms", "2500", "--exit-timeout-ms", "30000", "--", "{rom}"}},
            {"windowTimeoutSeconds", 0},
            {"hideConsoleWindow", true},
            {"systems", {{"gb", nlohmann::json::object()}}}};
        const nlohmann::json document{{"schemaVersion", 1}, {"profiles", nlohmann::json::array({profile})}};
        REQUIRE(rv::core::writeTextFileAtomic(data / "config" / "emulators.json", document.dump(2) + "\n").has_value());
        cli("assign_gb", {"assign", "gb", "--profile", "fake-return"});
        cli("assign_psx", {"assign", "psx", "--command", "Fake Emulator (Inject)"});
    }

    /// retroverse-cli --data-dir <data> `arguments`, which must exit 0.
    void cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE(run.exitCode == 0);
    }

    [[nodiscard]] std::filesystem::path databaseFile() const { return data / "database" / "retroverse.sqlite"; }

    /// The GameId of the identity at `relativePath` of `system`.
    [[nodiscard]] rv::catalog::GameId gameAt(std::string_view system, std::string_view relativePath) const {
        auto reader = openReader();
        auto statement =
            reader.prepare("SELECT id FROM rv_game_identities WHERE system = ?1 AND relative_path = ?2").value();
        statement.bindText(1, system).bindText(2, relativePath);
        REQUIRE(statement.step().value());
        return rv::catalog::GameId::fromValue(static_cast<std::uint64_t>(statement.columnInt(0))).value();
    }

    /// The play sessions of `game`, newest first.
    [[nodiscard]] std::vector<rv::catalog_db::ListedSession> sessionsOf(rv::catalog::GameId game) const {
        auto reader = openReader();
        auto store = rv::catalog_db::SessionStore::open(reader);
        REQUIRE(store.has_value());
        rv::catalog_db::SessionFilter filter;
        filter.game = game;
        auto sessions = (*store)->sessions(filter);
        REQUIRE(sessions.has_value());
        return std::move(*sessions);
    }

private:
    [[nodiscard]] rv::catalog_db::Connection openReader() const {
        rv::catalog_db::DatabaseOptions options;
        options.create = false;
        options.switchToWal = false;
        auto database = rv::catalog_db::Database::open(databaseFile(), options);
        REQUIRE(database.has_value());
        auto reader = database->openReader();
        REQUIRE(reader.has_value());
        return std::move(*reader);
    }
};

/// The arguments of a catalog run: the script, `frames` frames at a fixed step (lockstep with the catalog worker) from
/// the spawn point.
std::vector<std::string> catalogArguments(std::string_view script, std::string_view frames) {
    return {"--input-script", arg(inputScript(script)), "--exit-after-frames", "--frames", std::string(frames),
            "--fixed-dt", "0.016667", "--spawn", std::string(kSpawn)};
}

} // namespace

TEST_CASE("catalog: a game found by search and filter launches from the ES-DE catalog and returns where it was",
          "[gpu][catalog]") {
    ensureLaunchAllowList();
    const ImportedCatalog catalog("search_launch");
    const rv::catalog::GameId tennis = catalog.gameAt("gb", "Tiny Tennis (Europe).gb");
    AppRun run = runApp(retroverseExe(), catalog.dir, "search_launch", catalogArguments("catalog_search_launch.json", "40"));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;

    REQUIRE(report.catalog.has_value());
    const rv::app::CatalogRecord& record = *report.catalog;
    CHECK(record.source == "esde");
    CHECK(record.notice.empty());
    CHECK(record.library == "esde");
    CHECK(record.systems == 3);  // gb, snes, psx
    CHECK(record.games == 15);   // the fixture's games (its missing file is not listed)
    CHECK(record.search == "nintendo");
    CHECK(record.filter == "gb");
    // The gb games matching "nintendo", the list the screen shows and its count alike.
    CHECK(record.listed == 8);
    CHECK(record.shownCount == std::optional<std::size_t>(8));
    CHECK(record.selectedId == tennis.toString());
    // The filter's count was logged with the filter's own list, which the catalog's worker brought a frame later,
    // never with the search's list shown when the filter was chosen (13 games: gb's and snes's).
    {
        const auto log = rv::core::readTextFile(run.outputFile);
        REQUIRE(log.has_value());
        CHECK(log->find("Quick catalog: filter 'gb', 8 game(s)") != std::string::npos);
        CHECK(log->find("Quick catalog: filter 'gb', 13 game(s)") == std::string::npos);
    }
    REQUIRE(record.openedMs.has_value());
    REQUIRE(record.firstListMs.has_value());
    REQUIRE(record.firstPageMs.has_value());
    REQUIRE(record.pages.has_value());
    CHECK(record.pages->count >= 3);
    CHECK(record.cardSqueezed.empty()); // the card shows its text whole

    REQUIRE(report.launches.size() == 1);
    const rv::app::LaunchRecord& launch = report.launches.front();
    CHECK(launch.gameId == tennis.toString());
    CHECK(launch.profile == "fake-return");
    CHECK(launch.state == "Returned");
    CHECK(launch.outcome == "returned");
    CHECK(launch.exitCode == 0u);
    CHECK(launch.framesWhileRunning == 0);
    REQUIRE(launch.runMs.has_value());
    CHECK(*launch.runMs >= 2400.0);
    // The decision came from the worker: Avvia on frame 23, the request (and its "Gioco in corso") on frame 24.
    CHECK(launch.requestFrame == 24);
    CHECK(launch.startFrame == 25u);
    CHECK(launch.endFrame == 25u);
    CHECK(report.framesRendered == 40);

    // What the catalog and the walker showed before the launch, and after the return: the same.
    REQUIRE(launch.before.has_value());
    REQUIRE(launch.after.has_value());
    CHECK(launch.before->selectedId == tennis.toString());
    CHECK(launch.before->search == "nintendo");
    CHECK(launch.before->filter == "gb");
    CHECK(launch.before->readiness == "ready");
    REQUIRE(launch.before->camera.has_value());
    CHECK(*launch.after == *launch.before);

    // The play session, recorded on the worker: returned, with its launch verification.
    REQUIRE(launch.sessionId.has_value());
    CHECK(launch.sessionError.empty());
    CHECK(launch.verified);
    const auto sessions = catalog.sessionsOf(tennis);
    REQUIRE(sessions.size() == 1);
    const rv::catalog::PlaySession& session = sessions.front().session;
    CHECK(session.id == *launch.sessionId);
    CHECK(session.outcome == rv::catalog::PlaySessionOutcome::Returned);
    CHECK(session.choice.kind == rv::launcher::LaunchChoiceKind::Profile);
    CHECK(session.choice.value == "fake-return");
    CHECK(session.choiceSource == rv::catalog::ChoiceSource::RetroverseSystem);
    CHECK(session.exitCode == 0u);
    CHECK(session.ended.has_value());
    REQUIRE(session.runTime.has_value());
    CHECK(session.runTime->count() >= 2400);
    CHECK(session.owner.processId != 0);
    CHECK(rv::catalog::returnedNormally(session));
}

TEST_CASE("catalog: a game that is not ready shows its readiness, and Avvia shows why instead of launching",
          "[gpu][catalog]") {
    ensureLaunchAllowList();
    const ImportedCatalog catalog("blocked");
    const rv::catalog::GameId solo = catalog.gameAt("psx", "Solo Disc (USA).chd");
    const std::filesystem::path screenshot = catalog.dir / "blocked.png";
    std::vector<std::string> arguments = catalogArguments("catalog_blocked_launch.json", "30");
    arguments.insert(arguments.end(), {"--screenshot", arg(screenshot)});
    AppRun run = runApp(retroverseExe(), catalog.dir, "blocked", std::move(arguments));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;
    requireScreenshot(run, screenshot, {1920, 1080});

    REQUIRE(report.launches.size() == 1);
    const rv::app::LaunchRecord& launch = report.launches.front();
    CHECK(launch.gameId == solo.toString());
    CHECK(launch.state == "Blocked");
    CHECK(launch.outcome == "not-ready");
    CHECK(launch.reason == "requires-configuration");
    CHECK(launch.detail.find("%INJECT%") != std::string::npos);
    CHECK(launch.processId == 0);
    CHECK_FALSE(launch.startFrame.has_value());
    CHECK(launch.panels == std::vector<std::string>{"outcome"});
    REQUIRE(report.catalog.has_value());
    CHECK(report.catalog->cardSqueezed.empty()); // the readiness and the blocker's sentence cut nothing of the card
    // Its card said so before Avvia: the game's own readiness, resolved on the worker (its system's command is ready
    // for Twin Tracks, which has no %INJECT% file).
    REQUIRE(launch.before.has_value());
    CHECK(launch.before->selectedId == solo.toString());
    CHECK(launch.before->search == "solo");
    CHECK(launch.before->readiness == "requires-configuration");
    CHECK(launch.before->blocker == "inject-file");
    REQUIRE(launch.after.has_value());
    CHECK(*launch.after == *launch.before);

    REQUIRE(launch.sessionId.has_value());
    const auto sessions = catalog.sessionsOf(solo);
    REQUIRE(sessions.size() == 1);
    const rv::catalog::PlaySession& session = sessions.front().session;
    CHECK(session.outcome == rv::catalog::PlaySessionOutcome::Blocked);
    CHECK(session.readiness == rv::catalog::LaunchReadiness::RequiresConfiguration);
    CHECK(session.choice.value == "Fake Emulator (Inject)");
    CHECK(session.message.find("inject-file") != std::string::npos);
    CHECK(session.processId == 0);
}

TEST_CASE("catalog: the card of a blocked game with a description shows its text whole, also with Informazioni",
          "[gpu][catalog]") {
    const ImportedCatalog catalog("card");
    // ES-DE's configuration gone since the import: every game requires configuration (library-unreadable), with the
    // longest of the blocker sentences; Tiny Tennis has a two-line description.
    std::filesystem::remove(catalog.fixture.path("Programs/ES-DE/resources/systems/windows/es_systems.xml"));
    const rv::catalog::GameId tennis = catalog.gameAt("gb", "Tiny Tennis (Europe).gb");
    for (const auto& [name, frames] : {std::pair<std::string, std::string>{"card", "22"}, {"card_info", "26"}}) {
        INFO(name);
        const std::filesystem::path screenshot = catalog.dir / (name + ".png");
        std::vector<std::string> arguments = catalogArguments("catalog_card_info.json", frames);
        arguments.insert(arguments.end(), {"--screenshot", arg(screenshot)});
        AppRun run = runApp(retroverseExe(), catalog.dir, name, std::move(arguments));
        skipWithoutOpenGl(run);
        requireCleanRun(run);
        INFO(run.describe());
        requireScreenshot(run, screenshot, {1920, 1080});
        REQUIRE(run.report->catalog.has_value());
        const rv::app::CatalogRecord& record = *run.report->catalog;
        CHECK(record.selectedId == tennis.toString());
        CHECK(record.cardSqueezed.empty());
        CHECK(run.report->launches.empty());
    }
}

TEST_CASE("catalog: without library.json the quick catalog says how to import", "[gpu][catalog]") {
    const std::filesystem::path dir = freshCaseDir("catalog/no_library");
    const std::filesystem::path screenshot = dir / "no_library.png";
    std::vector<std::string> arguments = catalogArguments("catalog_open.json", "8");
    arguments.insert(arguments.end(), {"--screenshot", arg(screenshot)});
    AppRun run = runApp(retroverseExe(), dir, "no_library", std::move(arguments));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    requireScreenshot(run, screenshot, {1920, 1080});
    REQUIRE(run.report->catalog.has_value());
    const rv::app::CatalogRecord& record = *run.report->catalog;
    CHECK(record.source == "esde");
    CHECK(record.notice == "no-library");
    CHECK(record.open);
    CHECK(record.games == 0);
    CHECK(record.library.empty());
    CHECK_FALSE(record.openedMs.has_value());
    CHECK_FALSE(record.firstPageMs.has_value());
    // Nothing was written in the data directory but the log and the report: no database, no configuration.
    CHECK_FALSE(std::filesystem::exists(dir / "data" / "database"));
    CHECK_FALSE(std::filesystem::exists(dir / "data" / "config" / "library.json"));
}
