// The catalog's refresh in retroverse.exe (I3 step 13r): the standard fixture (tests/fixtures/esde/standard) recorded
// as the library of the case's data directory and imported with retroverse-cli, then ROMs added to its ROM folder
// before the run. --refresh-library refreshes the catalog once it opened: the import on the catalog worker, in slices,
// the quick catalog usable meanwhile. The run report (catalog.refreshes, the "refresh" phase) and the catalog database
// tell what happened: the new game appears, the selection is the same game after the list ran again, the frame times
// of the refresh phase (informative: the budget is measured on a quiet machine); "Aggiorna libreria" pressed with the
// pad's actions refreshes again. A run that ends during a refresh (a library of 10,000 more files) stops its worker
// within the bound, the import rolled back or committed whole, and the database passes retroverse-cli db check.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <rv/catalog_db/database.hpp>
#include <rv/core/file_io.hpp>

#include "launch_support.hpp"
#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;

namespace {

constexpr std::string_view kSpawn = "0,0,3.2,0,-6";

std::filesystem::path retroverseCliExe() {
    return retroverseExe().parent_path() / "retroverse-cli.exe";
}

/// The standard fixture imported into <caseDir>/data with retroverse-cli (library.json, the catalog database).
struct ImportedLibrary {
    rv::test::Fixture fixture;
    std::filesystem::path dir;
    std::filesystem::path data;

    explicit ImportedLibrary(std::string_view name)
        : fixture(rv::test::Fixture::materialize("esde/standard").value()),
          dir(freshCaseDir(std::string("refresh/") + std::string(name))), data(dir / "data") {
        REQUIRE(cli("library", {"library", "create", "--home", arg(fixture.root()), "--install",
                                arg(fixture.path("Programs/ES-DE"))}) == 0);
        REQUIRE(cli("import", {"import"}) == 0);
    }

    /// retroverse-cli --data-dir <data> `arguments`: its exit code.
    int cli(std::string_view name, std::vector<std::string> arguments) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        return run.exitCode;
    }

    /// A ROM of `size` bytes at `relativePath` under the fixture (its ROM folder).
    void addRom(std::string_view relativePath, std::size_t size = 65536) const {
        REQUIRE(rv::core::writeTextFileAtomic(fixture.path(std::string(relativePath)), std::string(size, 'R'))
                    .has_value());
    }

    /// Available games of the library in the catalog database now.
    [[nodiscard]] std::size_t storedGames() const {
        rv::catalog_db::DatabaseOptions options;
        options.create = false;
        options.switchToWal = false;
        auto database = rv::catalog_db::Database::open(data / "database" / "retroverse.sqlite", options);
        REQUIRE(database.has_value());
        auto reader = database->openReader();
        REQUIRE(reader.has_value());
        auto count =
            reader->queryInt("SELECT COUNT(*) FROM idx_games WHERE state = 'game' AND availability = 'available'");
        REQUIRE(count.has_value());
        return static_cast<std::size_t>(count->value_or(0));
    }
};

std::vector<std::string> refreshArguments(std::string_view frames) {
    return {"--refresh-library", "--exit-after-frames", "--frames", std::string(frames), "--fixed-dt", "0.016667",
            "--spawn", std::string(kSpawn)};
}

const rv::app::PhaseRecord* findPhase(const rv::app::RunReport& report, std::string_view name) {
    const auto found = std::find_if(report.phases.begin(), report.phases.end(),
                                    [&](const rv::app::PhaseRecord& phase) { return phase.name == name; });
    return found == report.phases.end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("refresh: --refresh-library imports a ROM added since; the new game appears, the selection is kept",
          "[gpu][refresh]") {
    const ImportedLibrary library("refresh_library");
    // Added after the import: its title sorts first ("!" before the fixture's ".Dot Hero"), so the game selected before
    // (the list's first) moves down one.
    library.addRom("roms/snes/!Aardvark Arcade (USA).sfc");
    // 600 frames: both refreshes end well before the last (each takes about 0.1 to 1 s in Debug on a busy machine).
    std::vector<std::string> arguments = refreshArguments("600");
    arguments.insert(arguments.begin(), {"--input-script", arg(inputScript("catalog_refresh.json"))});
    AppRun run = runApp(retroverseExe(), library.dir, "refresh_library", std::move(arguments));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    const rv::app::RunReport& report = *run.report;

    REQUIRE(report.catalog.has_value());
    const rv::app::CatalogRecord& catalog = *report.catalog;
    CHECK(catalog.notice.empty());
    CHECK(catalog.games == 16); // the fixture's 15 and the new one
    CHECK(catalog.listed == 16);
    CHECK(catalog.shownCount == std::optional<std::size_t>(16));
    REQUIRE(catalog.refreshes.size() == 2);

    // The command line's refresh: once the catalog opened, the new game imported, the list run again with the same
    // game selected (one place down: the new game sorts first).
    const rv::app::RefreshRecord& first = catalog.refreshes[0];
    INFO(first.error);
    CHECK(first.trigger == "command-line");
    CHECK(first.outcome == "done");
    CHECK(first.added == 1);
    CHECK(first.removed == 0);
    CHECK(first.entries > 15);
    REQUIRE(first.ms.has_value());
    REQUIRE(first.endFrame.has_value());
    CHECK(*first.endFrame >= first.requestFrame);
    CHECK(first.slices > 0);
    REQUIRE(first.before.has_value());
    REQUIRE(first.after.has_value());
    CHECK_FALSE(first.before->selectedId.empty());
    CHECK(first.after->selectedId == first.before->selectedId);
    CHECK(first.before->selectedPosition == std::optional<std::size_t>(0));
    CHECK(first.after->selectedPosition == std::optional<std::size_t>(1));

    // "Aggiorna libreria" pressed on frame 90: a second refresh, which finds nothing new.
    const rv::app::RefreshRecord& second = catalog.refreshes[1];
    INFO(second.error);
    CHECK(second.trigger == "user");
    CHECK(second.outcome == "done");
    CHECK(second.requestFrame == 90);
    CHECK(second.added == 0);
    CHECK(second.removed == 0);
    CHECK(second.changed == 0);
    REQUIRE(second.after.has_value());
    REQUIRE(second.before.has_value());
    CHECK(second.after->selectedId == second.before->selectedId);
    CHECK(library.storedGames() == 16);

    // The refresh phase: both openings, closed when they ended; frame times of the second (past the warm-up).
    const rv::app::PhaseRecord* phase = findPhase(report, "refresh");
    REQUIRE(phase != nullptr);
    CHECK(phase->opened == 2);
    CHECK_FALSE(phase->open);
    CHECK(phase->frames >= 2);
    if (phase->times) {
        // NFR-04 over the refresh phase: p99 at most 33.3 ms. Informative here (other work may share the machine);
        // the budget's figure is taken on a quiet machine (docs/i3/step-13r.md).
        UNSCOPED_INFO(fmt::format("refresh phase: {} frame(s), p50 {:.2f} ms, p99 {:.2f} ms, max {:.2f} ms",
                                  phase->times->count, phase->times->p50Ms, phase->times->p99Ms, phase->times->maxMs));
        if (phase->times->p99Ms > 33.3) {
            WARN("frame p99 over the refresh phase above 33.3 ms (informative while other work runs)");
        }
    }
    for (const rv::app::RefreshRecord& refresh : catalog.refreshes) {
        if (refresh.lists) {
            UNSCOPED_INFO(fmt::format("lists during a refresh: {}, max {:.1f} ms", refresh.lists->count,
                                      refresh.lists->maxMs));
        }
    }
    // The new game is in the catalog the CLI reads too.
    CHECK(library.cli("db_check", {"db", "check"}) == 0);
}

TEST_CASE("refresh: a run that ends during a refresh stops its worker within the bound, the import whole or undone",
          "[gpu][refresh]") {
    const ImportedLibrary library("quit_mid_refresh");
    // 10,000 more files in gb: an import of seconds, which a run of 40 frames ends in the middle of.
    const std::filesystem::path bulk = library.fixture.path("roms/gb/Bulk");
    std::filesystem::create_directories(bulk);
    for (int index = 0; index < 10000; ++index) {
        std::ofstream file(bulk / fmt::format("Bulk {:05}.gb", index), std::ios::binary);
        file << 'B';
        REQUIRE(file.good());
    }
    AppRun run = runApp(retroverseExe(), library.dir, "quit_mid_refresh", refreshArguments("40"));
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->catalog.has_value());
    const rv::app::CatalogRecord& catalog = *run.report->catalog;
    REQUIRE(catalog.refreshes.size() == 1);
    const rv::app::RefreshRecord& refresh = catalog.refreshes.front();
    INFO(refresh.error);
    CHECK(refresh.trigger == "command-line");
    // Quitting during the refresh: the worker stopped within the bound (2 s), the import cancelled.
    CHECK(refresh.outcome == "cancelled");
    REQUIRE(refresh.stopMs.has_value());
    CHECK(*refresh.stopMs < 2000.0);
    UNSCOPED_INFO(fmt::format("the worker stopped in {:.1f} ms", *refresh.stopMs));
    const rv::app::PhaseRecord* phase = findPhase(*run.report, "refresh");
    REQUIRE(phase != nullptr);
    CHECK(phase->open); // never closed: the run ended first
    // Rolled back whole (a cancelled import writes nothing), and the database is sound.
    CHECK(library.storedGames() == 15);
    CHECK(library.cli("db_check", {"db", "check"}) == 0);
}
