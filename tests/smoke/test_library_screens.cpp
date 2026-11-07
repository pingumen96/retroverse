// The media coverage and library screens (I7 step 8, ADR-0018 §14, §15), driven by the pad's actions alone through input
// scripts on the building fixture, read from the run report and the catalog database: the coverage per system and both
// populations, a system's details; a review row resolved to one of its candidates; the removals the guard holds shown;
// the quick-hash pass stopped and resumed (a lockstep run hashes one file per frame); nothing cut at 1280 x 720 and at
// text scale 125%. Hidden and frame-limited; the data under the build tree; nothing launches.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/app/run_report.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>

#include "smoke_support.hpp"
#include "support/fixture.hpp"

using namespace rv::smoke;
namespace fs = std::filesystem;
using Json = nlohmann::json;

namespace {

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
    Script& wait(int frames) {
        frame += frames;
        return *this;
    }
    /// From the world into the coverage screen (the menu's fifth button), its figures answered.
    Script& openCoverage() { return press("menu").press("ui_down", 4).press("ui_activate").wait(4); }
    /// Into the library screen (the sixth), on its section `tab` (0 Origini ... 4 Impronte).
    Script& openLibrary(int tab = 0) {
        press("menu").press("ui_down", 5).press("ui_activate").wait(4);
        return press("ui_filter_next", tab);
    }

    void write(const fs::path& file) const {
        REQUIRE(rv::core::writeTextFileAtomic(file, steps.dump(2) + "\n").has_value());
    }
};

/// The building fixture imported into <case>/data.
struct Building {
    rv::test::Fixture fixture;
    fs::path dir;
    fs::path data;

    explicit Building(std::string_view name)
        : fixture(rv::test::Fixture::materialize("esde/building").value()),
          dir(freshCaseDir(std::string("library_screens/") + std::string(name))), data(dir / "data") {
        cli("library",
            {"library", "create", "--home", arg(fixture.root()), "--install", arg(fixture.path("Programs/ES-DE"))});
        cli("import", {"import"});
    }

    /// retroverse-cli; `problems`: exit code 1 (problems found, the guard's issue) is as good as 0.
    void cli(std::string_view name, std::vector<std::string> arguments, bool problems = false) const {
        arguments.insert(arguments.begin(), {"--data-dir", arg(data)});
        const AppRun run = runCommand(retroverseCliExe(), dir, std::string("cli_") + std::string(name), arguments);
        INFO(run.describe());
        REQUIRE((run.exitCode == 0 || (problems && run.exitCode == 1)));
    }

    /// The ROM files of `system`, by name.
    [[nodiscard]] std::vector<fs::path> roms(std::string_view system, std::string_view extension) const {
        std::vector<fs::path> files;
        for (const fs::directory_entry& entry : fs::directory_iterator(fixture.path("roms/" + std::string(system)))) {
            if (entry.is_regular_file() && entry.path().extension() == extension) {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    /// One gb cartridge moved into two folders at once, its size, time and name kept (a review row), and five of
    /// pokemini's eight gone (the guard holds them); imported again as retroverse-cli import makes it.
    void moveAndLose() const {
        const std::vector<fs::path> gb = roms("gb", ".gb");
        REQUIRE_FALSE(gb.empty());
        const fs::path moved = gb.front();
        for (const char* folder : {"A", "B"}) {
            const fs::path to = moved.parent_path() / folder / moved.filename();
            fs::create_directories(to.parent_path());
            fs::copy_file(moved, to);
            fs::last_write_time(to, fs::last_write_time(moved));
        }
        fs::remove(moved);
        const std::vector<fs::path> pokemini = roms("pokemini", ".min");
        REQUIRE(pokemini.size() >= 5);
        for (std::size_t index = 0; index < 5; ++index) {
            fs::remove(pokemini[index]);
        }
        cli("import_again", {"import"}, true);
    }

    AppRun run(std::string_view name, const Script& script, std::vector<std::string> extra = {}) const {
        const fs::path file = dir / (std::string(name) + ".script.json");
        script.write(file);
        std::vector<std::string> arguments{"--scene", "m0_testroom", "--exit-after-frames", "--frames",
                                           std::to_string(script.frame + 4), "--fixed-dt", "0.016667", "--input-script",
                                           arg(file)};
        arguments.insert(arguments.end(), extra.begin(), extra.end());
        return runApp(retroverseExe(), dir, name, std::move(arguments));
    }

    [[nodiscard]] std::int64_t count(std::string_view sql) const {
        rv::catalog_db::DatabaseOptions options;
        options.create = false;
        options.switchToWal = false;
        auto database = rv::catalog_db::Database::open(data / "database" / "retroverse.sqlite", options);
        REQUIRE(database.has_value());
        auto statement = database->writer().prepare(sql);
        REQUIRE(statement.has_value());
        REQUIRE(statement->step().value());
        return statement->columnInt(0);
    }
};

std::vector<std::string> eventsNamed(const rv::app::LibraryRecord& library, std::string_view name) {
    std::vector<std::string> details;
    for (const rv::app::LibraryEventRecord& event : library.events) {
        if (event.event == name) {
            details.push_back(event.detail);
        }
    }
    return details;
}

} // namespace

TEST_CASE("coverage: the screen by the pad's actions: the systems, a system's details, both populations",
          "[gpu][library_screens]") {
    const Building building("coverage");
    Script script;
    script.openCoverage().press("ui_down").press("ui_activate").wait(2).press("ui_down", 2);
    script.press("ui_filter_next").wait(2).press("ui_back").press("ui_back").press("ui_back");
    const AppRun run = building.run("run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->coverage);
    const rv::app::CoverageRecord& coverage = *run.report->coverage;
    CHECK(coverage.opened == 1);
    CHECK_FALSE(coverage.open);
    CHECK(coverage.state == "listed");
    CHECK(coverage.games == static_cast<std::uint64_t>(building.count("SELECT count(*) FROM idx_games WHERE state = 'game'")));
    CHECK(coverage.scraped ==
          static_cast<std::uint64_t>(
              building.count("SELECT count(*) FROM idx_games WHERE state = 'game' AND gamelist_path <> ''")));
    CHECK(coverage.systems.size() >= 7);
    // Worst first: amiga has no media at all.
    REQUIRE_FALSE(coverage.systems.empty());
    CHECK(coverage.systems.front().starts_with("amiga="));
    REQUIRE(coverage.views.size() == 3);
    CHECK(coverage.views[0] == "systems");
    CHECK(coverage.views[1].starts_with("system:"));
    CHECK(coverage.views[2] == "systems");
    CHECK(coverage.tabs == std::vector<std::string>{"games", "scraped"});
    CHECK(coverage.textCut == 0);
    CHECK(coverage.readMs < 1000.0);
    REQUIRE(run.report->menu);
    CHECK_FALSE(run.report->menu->open);
}

TEST_CASE("library: a review row resolved to one of its candidates by the pad's actions; the removals held",
          "[gpu][library_screens]") {
    const Building building("review");
    building.moveAndLose();
    REQUIRE(building.count("SELECT count(*) FROM rv_identity_reviews WHERE resolved_at IS NULL") == 1);
    const std::int64_t pokeminiBefore = building.count("SELECT count(*) FROM idx_games WHERE system = 'pokemini'");
    Script script;
    // Rimozioni: shown, asked, cancelled; then Da verificare: the row, its first candidate, Abbina.
    script.openLibrary(2).press("ui_activate").press("ui_activate").wait(2);
    script.press("ui_filter_next").press("ui_activate").wait(2).press("ui_activate").press("ui_down").press("ui_activate");
    script.wait(8).press("ui_back").press("ui_back");
    const AppRun run = building.run("run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->library);
    const rv::app::LibraryRecord& library = *run.report->library;
    CHECK(library.opened == 1);
    CHECK_FALSE(library.open);
    CHECK(library.held == std::vector<std::string>{"pokemini"});
    CHECK(eventsNamed(library, "confirm-removals").size() == 1);
    CHECK(eventsNamed(library, "apply-removals").empty()); // cancelled: nothing applied
    CHECK(eventsNamed(library, "confirm-pair").size() == 1);
    const std::vector<std::string> resolved = eventsNamed(library, "resolve");
    REQUIRE(resolved.size() == 1);
    CHECK(resolved.front().find("separate") == std::string::npos);
    CHECK(eventsNamed(library, "resolved") == std::vector<std::string>{""});
    CHECK(library.reviews.empty()); // asked again after it
    CHECK(library.textCut == 0);
    CHECK(library.readMs < 1000.0);
    // The vanished cartridge's identity follows its file into A; the row is resolved, paired.
    CHECK(building.count("SELECT count(*) FROM rv_identity_reviews WHERE resolution = 'paired'") == 1);
    CHECK(building.count("SELECT count(*) FROM rv_game_identities WHERE system = 'gb' AND relative_path LIKE 'A/%'") ==
          1);
    CHECK(building.count("SELECT count(*) FROM rv_identity_aliases") == 1);
    // Nothing of pokemini was removed.
    CHECK(building.count("SELECT count(*) FROM idx_games WHERE system = 'pokemini'") == pokeminiBefore);
}

TEST_CASE("library: Calcola impronte stopped and resumed by the pad's actions", "[gpu][library_screens]") {
    const Building building("hash");
    const std::int64_t files =
        building.count("SELECT count(*) FROM rv_game_identities WHERE present = 1 AND is_directory = 0");
    REQUIRE(files > 40);
    Script script;
    script.openLibrary(4).press("ui_activate").wait(6).press("ui_activate").wait(6);
    script.press("ui_activate").wait(static_cast<int>(files) + 20).press("ui_back").press("ui_back");
    const AppRun run = building.run("run", script);
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->library);
    const rv::app::LibraryRecord& library = *run.report->library;
    CHECK(eventsNamed(library, "hash-start").size() == 2);
    CHECK(eventsNamed(library, "hash-cancel").size() == 1);
    REQUIRE(library.passes.size() == 2);
    CHECK(library.passes[0].outcome == "cancelled");
    CHECK(library.passes[0].cancelled);
    CHECK(library.passes[0].hashed > 0);
    CHECK(library.passes[0].remaining > 0);
    CHECK(library.passes[1].outcome == "done");
    CHECK(library.passes[1].remaining == 0);
    CHECK(library.passes[0].hashed + library.passes[1].hashed == static_cast<std::uint64_t>(files));
    CHECK(library.files == static_cast<std::uint64_t>(files));
    CHECK(library.hashed == static_cast<std::uint64_t>(files));
    CHECK(building.count("SELECT count(*) FROM rv_game_identities WHERE present = 1 AND is_directory = 0 AND "
                         "quick_hash IS NOT NULL") == files);
    CHECK(library.textCut == 0);
}

TEST_CASE("library and coverage: at 1280 x 720 and text scale 125% nothing is cut", "[gpu][library_screens]") {
    const Building building("scale125");
    building.moveAndLose();
    REQUIRE(rv::core::writeTextFileAtomic(building.data / "settings.json",
                                          R"({"schemaVersion": 1, "player": {"textScale": 1.25}})")
                .has_value());
    Script script;
    script.openLibrary(0);
    for (int tab = 1; tab < 5; ++tab) {
        script.press("ui_filter_next").wait(2);
    }
    script.press("ui_filter_previous").press("ui_activate").wait(2).press("ui_back", 3);
    script.openCoverage().press("ui_activate").wait(2);
    const AppRun run = building.run("run", script, {"--width", "1280", "--height", "720"});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    INFO(run.describe());
    REQUIRE(run.report->library);
    CHECK(run.report->library->textCut == 0);
    REQUIRE(run.report->coverage);
    CHECK(run.report->coverage->textCut == 0);
    REQUIRE(run.report->menu);
    CHECK(run.report->menu->textCut == 0);
}

// By hand, for a look: rv_smoke_tests "[library_shots]" with RV_SHOTS_DIR naming a folder writes the coverage list and a
// system's details, each section of the library, a review and the removals' confirmation, at 1920 x 1080 and 1280 x 720,
// hidden and frame-limited, the pad's actions alone. Not a check: visual verification.
TEST_CASE("library and coverage: the pictures", "[.shots][.library_shots]") {
    const char* folder = std::getenv("RV_SHOTS_DIR");
    if (folder == nullptr) {
        SKIP("RV_SHOTS_DIR names no folder");
    }
    const fs::path out = rv::core::pathFromUtf8(folder);
    fs::create_directories(out);
    const Building building("shots");
    building.moveAndLose();
    for (const auto& [prefix, width, height] :
         {std::tuple<std::string, std::string, std::string>{"", "1920", "1080"}, {"720_", "1280", "720"}}) {
        const auto shoot = [&](std::string_view name, const Script& script) {
            const std::string run = fmt::format("{}{}", prefix, name);
            const AppRun result = building.run(
                run, script, {"--width", width, "--height", height, "--screenshot", arg(out / (run + ".png"))});
            requireCleanRun(result);
            INFO(result.describe());
            if (result.report->library) {
                CHECK(result.report->library->textCut == 0);
            }
            if (result.report->coverage) {
                CHECK(result.report->coverage->textCut == 0);
            }
        };
        shoot("menu", Script{}.press("menu").press("ui_down", 5).wait(4));
        shoot("coverage", Script{}.openCoverage());
        shoot("coverage_system", Script{}.openCoverage().press("ui_activate").wait(4));
        shoot("coverage_scraped", Script{}.openCoverage().press("ui_filter_next").wait(4));
        shoot("library_sources", Script{}.openLibrary(0).wait(4));
        shoot("library_issues", Script{}.openLibrary(1).wait(4));
        shoot("library_removals", Script{}.openLibrary(2).wait(4));
        shoot("library_removals_confirm", Script{}.openLibrary(2).press("ui_activate").wait(4));
        shoot("library_reviews", Script{}.openLibrary(3).wait(4));
        shoot("library_review", Script{}.openLibrary(3).press("ui_activate").wait(4));
        shoot("library_review_confirm", Script{}.openLibrary(3).press("ui_activate").press("ui_activate").wait(4));
        shoot("library_hashes", Script{}.openLibrary(4).wait(4));
        shoot("library_hashing", Script{}.openLibrary(4).press("ui_activate").wait(8));
    }
}
