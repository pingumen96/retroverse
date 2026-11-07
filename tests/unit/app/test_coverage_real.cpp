// The coverage and library screens on the real library (I7 step 8, ADR-0018 §14, §16). Hidden ([.real-library]): ctest
// never runs it; scripts/real-library.ps1 does (Release by default), with ES-DE closed, read-only snapshots of the library
// before and after (0 differences: nothing here writes there), and the folders in the RV_REAL_* variables. A user data
// directory under the build tree: retroverse-cli library create and import, as the user makes them; then the app's
// catalog answers the coverage screen (SqliteCatalogQuery::mediaCoverage, the catalog's rows) and the library screen,
// each timed (shown within 1 s of opening), and the figures are compared with retroverse-cli media --coverage's, which
// reads the library and its media folders anew: every system's share of every kind, for the games and the scraped games,
// within 0.1 point. Nothing launches; the quick-hash pass is not run (it reads every ROM: the user's own trial).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/app/app_catalog.hpp>
#include <rv/app/library_data.hpp>
#include <rv/catalog/coverage.hpp>
#include <rv/catalog/media.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>
#include <rv/core/utf8.hpp>
#include <rv/ui/quick_catalog_model.hpp>

#include "support/fixture.hpp"

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using rv::catalog::CoveragePopulation;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

std::optional<fs::path> pathVariable(const wchar_t* name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size <= 1) {
        return std::nullopt;
    }
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(name, value.data(), size));
    return fs::path(value);
}

class LoggingReset {
public:
    LoggingReset() = default;
    ~LoggingReset() {
        rv::core::LogConfig config;
        config.console = false;
        rv::core::initLogging(config);
        rv::core::shutdownLogging();
    }
    LoggingReset(const LoggingReset&) = delete;
    LoggingReset& operator=(const LoggingReset&) = delete;
};

/// What ProductUi does each frame, until nothing is pending.
void settle(rv::app::AppCatalog& catalog, rv::ui::QuickCatalogModel& model) {
    const auto deadline = std::chrono::steady_clock::now() + 10min;
    while (true) {
        const rv::app::CatalogChanges changes = catalog.poll();
        if (changes.opened || changes.refreshed || changes.otherImport) {
            model.refresh();
        } else if (changes.rowsChanged) {
            model.reread();
        }
        model.poll();
        if (!catalog.busy() && catalog.roomCatalogState() != rv::app::CatalogState::Opening) {
            return;
        }
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}

/// A share of `counts` (a population's figures in the CLI's JSON), over its members whose media are known.
std::optional<double> cliShare(const nlohmann::json& counts, std::string_view kind) {
    const double known = counts.at("entries").get<double>() - counts.at("unknown").get<double>();
    if (known <= 0.0) {
        return std::nullopt;
    }
    return counts.at("withKind").at(std::string(kind)).get<double>() / known;
}

} // namespace

TEST_CASE("coverage and library on the real library: the figures of media --coverage within 0.1 point, each screen "
          "within 1 s",
          "[.real-library][real-library][coverage_real]") {
    const LoggingReset loggingReset;
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    REQUIRE(home.has_value());
    REQUIRE(install.has_value());
    REQUIRE(romRoot.has_value());
    REQUIRE(mediaRoot.has_value());

    // A user data directory under the build tree, never in the library.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "coverage";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    const fs::path data = folder / "data";
    rv::core::PathEnvironment environment;
    environment.executableDir = rv::test::testProgramsDir();
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    const auto cli = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(data)});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, environment);
        return std::make_tuple(code, out.text(), err.text());
    };
    {
        const auto [code, out, err] =
            cli({"library", "create", "--home", rv::core::pathToUtf8(home->parent_path()), "--install",
                 rv::core::pathToUtf8(*install), "--rom-root", rv::core::pathToUtf8(*romRoot), "--media-root",
                 rv::core::pathToUtf8(*mediaRoot)});
        INFO(err);
        REQUIRE(code == ExitCode::Ok);
    }
    {
        const auto start = std::chrono::steady_clock::now();
        const auto [code, out, err] = cli({"import"});
        INFO(err);
        REQUIRE((code == ExitCode::Ok || code == ExitCode::ProblemsFound));
        std::cout << fmt::format("import: {:.1f} s\n",
                                 std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    }
    const fs::path cliJson = folder / "coverage.json";
    {
        const auto start = std::chrono::steady_clock::now();
        const auto [code, out, err] = cli({"media", "--coverage", "--json", "--output", rv::core::pathToUtf8(cliJson)});
        INFO(err);
        REQUIRE((code == ExitCode::Ok || code == ExitCode::ProblemsFound));
        std::cout << fmt::format("media --coverage: {:.1f} s\n",
                                 std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    }
    const nlohmann::json report = nlohmann::json::parse(rv::core::readTextFile(cliJson).value());

    rv::core::PathOptions pathOptions;
    pathOptions.userDataOverride = data;
    const rv::core::Paths paths = rv::core::Paths::resolve(pathOptions, environment).value();
    rv::app::EsdeAppCatalog::Options options;
    options.paths = paths;
    options.owner = rv::catalog::SessionOwner{4'000'000'008u, 1};
    rv::app::EsdeAppCatalog catalog(options);
    rv::ui::QuickCatalogModel model(catalog.source(), 6);
    settle(catalog, model);
    REQUIRE(catalog.roomCatalogState() == rv::app::CatalogState::Ready);

    // The coverage screen: from its request to its answer, as the screen opens.
    const auto asked = std::chrono::steady_clock::now();
    REQUIRE(catalog.requestCoverage());
    settle(catalog, model);
    const double coverageWall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - asked).count();
    std::optional<rv::app::CoverageList> list = catalog.takeCoverage();
    REQUIRE(list.has_value());
    REQUIRE_FALSE(list->error.has_value());
    std::cout << fmt::format("coverage: {:.0f} ms on the worker, {:.0f} ms from the request; {} system(s), {} clip(s) "
                             "known to fail{}\n",
                             list->milliseconds, coverageWall, list->coverage.systems.size(), list->clips.size(),
                             list->clipsError ? " (verdicts unreadable)" : "");
    CHECK(coverageWall < 1000.0);

    // Each system's share of each kind, and the totals, for both populations, against the CLI's.
    std::size_t compared = 0;
    double worst = 0.0;
    const auto compare = [&](const rv::catalog::CoverageCounts& counts, const nlohmann::json& cliCounts,
                             const std::string& where) {
        CHECK(counts.entries == cliCounts.at("entries").get<std::uint64_t>() - cliCounts.at("unknown").get<std::uint64_t>());
        for (const rv::catalog::MediaKind kind : rv::catalog::kMediaKinds) {
            const std::optional<double> ours = counts.share(kind);
            const std::optional<double> theirs = cliShare(cliCounts, rv::catalog::toString(kind));
            INFO(fmt::format("{} {}", where, rv::catalog::toString(kind)));
            REQUIRE(ours.has_value() == theirs.has_value());
            if (ours) {
                const double difference = std::abs(*ours - *theirs) * 100.0;
                worst = std::max(worst, difference);
                CHECK(difference <= 0.1);
                ++compared;
            }
        }
    };
    const std::vector<std::pair<CoveragePopulation, std::string>> populations{
        {CoveragePopulation::Games, "games"}, {CoveragePopulation::ScrapedGames, "scraped-games"}};
    for (const auto& [population, name] : populations) {
        compare(list->coverage.total[population], report.at("total").at(name), "total " + name);
        for (const nlohmann::json& system : report.at("systems")) {
            const nlohmann::json& counts = system.at("populations").at(name);
            if (counts.at("entries").get<std::uint64_t>() == counts.at("unknown").get<std::uint64_t>()) {
                continue;
            }
            const auto systemName = rv::catalog::SystemName::parse(system.at("system").get<std::string>());
            REQUIRE(systemName.has_value());
            const rv::catalog::SystemCoverage* ours = list->coverage.find(*systemName);
            INFO(systemName->str());
            REQUIRE(ours != nullptr);
            compare((*ours)[population], counts, systemName->str() + " " + name);
        }
    }
    const rv::catalog::CoverageCounts& games = list->coverage.total[CoveragePopulation::Games];
    const rv::catalog::CoverageCounts& scraped = list->coverage.total[CoveragePopulation::ScrapedGames];
    std::cout << fmt::format("{} share(s) compared, the largest difference {:.4f} point(s)\n", compared, worst);
    std::cout << fmt::format("games {}: covers {}%, 3D boxes {}%, screenshots {}%, marquees {}%, videos {}%\n",
                             games.entries, rv::ui::shareText(games.share(rv::catalog::MediaKind::Cover)),
                             rv::ui::shareText(games.share(rv::catalog::MediaKind::Box3d)),
                             rv::ui::shareText(games.share(rv::catalog::MediaKind::Screenshot)),
                             rv::ui::shareText(games.share(rv::catalog::MediaKind::Marquee)),
                             rv::ui::shareText(games.share(rv::catalog::MediaKind::Video)));
    std::cout << fmt::format("scraped games {}: covers {}%, screenshots {}%, videos {}%\n", scraped.entries,
                             rv::ui::shareText(scraped.share(rv::catalog::MediaKind::Cover)),
                             rv::ui::shareText(scraped.share(rv::catalog::MediaKind::Screenshot)),
                             rv::ui::shareText(scraped.share(rv::catalog::MediaKind::Video)));
    const rv::ui::CoverageContent content = rv::app::coverageContentOf(*list);
    if (!content.games.systems.empty()) {
        std::cout << fmt::format("worst first: {} ({}), then {}\n", content.games.systems.front().name,
                                 content.games.systems.front().covers,
                                 content.games.systems.size() > 1 ? content.games.systems[1].name : std::string());
    }

    // The library screen.
    const auto libraryAsked = std::chrono::steady_clock::now();
    REQUIRE(catalog.requestLibraryStatus());
    settle(catalog, model);
    const double libraryWall =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - libraryAsked).count();
    std::optional<rv::app::LibraryStatus> status = catalog.takeLibraryStatus();
    REQUIRE(status.has_value());
    REQUIRE_FALSE(status->error.has_value());
    std::cout << fmt::format("library: {:.0f} ms on the worker, {:.0f} ms from the request; {} folder(s), {} issue(s), "
                             "{} system(s) held, {} review(s), {} of {} file(s) with a quick hash, {} system(s) "
                             "unreachable\n",
                             status->milliseconds, libraryWall, status->folders.size(), status->issues.size(),
                             status->held.size(), status->reviews.size(), status->hashes.current,
                             status->hashes.files, status->unavailableSystems);
    for (const rv::app::LibraryFolder& root : status->folders) {
        std::cout << fmt::format("  folder {}{}: {}\n", root.kind, root.system.empty() ? "" : " " + root.system,
                                 !root.reachable ? "unknown" : *root.reachable ? "reachable" : "unreachable");
        CHECK(root.reachable == std::optional<bool>(true));
    }
    const rv::ui::LibraryContent shown = rv::app::libraryContentOf(*status, {});
    for (const rv::ui::LibraryIssueGroup& group : shown.issues) {
        std::cout << fmt::format("  issues {}: {} ({})\n", group.code, group.count, group.severity);
    }
    CHECK(libraryWall < 1000.0);
    catalog.shutdown();
}
