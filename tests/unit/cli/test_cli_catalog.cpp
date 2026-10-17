// I2 step 9: retroverse-cli systems, games, search and show on the catalog database of --data-dir, as text and as
// JSON, to stdout or --output: the lists in their total order, the filters and pages, the search (words in any order,
// prefixes, diacritics, FTS5 syntax inert), a game's details, and the exit codes (no database, an unknown system or
// game, usage errors that touch no database).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>

#include <rv/catalog/catalog_store.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog_db/catalog_store.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/catalog_db/migrator.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>

#include "../catalog_db/catalog_db_test_support.hpp"
#include "support/temp_dir.hpp"

using namespace rv::catalog;
using rv::cli::ExitCode;
using rv::cli::Output;

namespace {

rv::core::PathEnvironment testEnvironment(const std::filesystem::path& executableDir) {
    rv::core::PathEnvironment environment;
    environment.executableDir = executableDir;
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return environment;
}

/// A run of the dispatcher logs to stderr; this puts back the plain console logger afterwards.
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

struct RunResult {
    ExitCode code = ExitCode::Ok;
    std::string out;
    std::string err;
};

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

/// The lines of `text`, without their line feeds.
std::vector<std::string> linesOf(std::string_view text) {
    std::vector<std::string> lines;
    while (!text.empty()) {
        const std::size_t end = std::min(text.find('\n'), text.size());
        lines.emplace_back(text.substr(0, end));
        text.remove_prefix(std::min(end + 1, text.size()));
    }
    return lines;
}

/// A game of `system` titled by its gamelist entry.
IndexedEntry game(std::string_view system, std::string_view relativePath, std::string_view title) {
    CatalogEntry entry = rv::test::bareEntry(system, relativePath);
    entry.gamelistPath = "./" + std::string(relativePath);
    entry.title = std::string(title);
    entry.metadata.name = std::string(title);
    const GameId id = rv::test::gameIdOf(LibraryId(), entry);
    return rv::test::indexedEntry(id, std::move(entry), false);
}

/// retroverse-cli --data-dir <dir>/data <arguments...>, over a small catalog written into that data directory.
class CatalogCli {
public:
    explicit CatalogCli(std::string_view prefix, bool withDatabase = true) : m_dir(prefix) {
        quest = game("gb", "Pixel Quest (World).gb", "Pixel Quest");
        quest.entry.metadata.developer = "Mirage Soft";
        quest.entry.metadata.publisher = "\xC3\x89" "ditions Pixel";
        quest.entry.metadata.genre = "Role Playing Game";
        quest.entry.metadata.releaseDate = ReleaseDate{1997, 7, 2};
        quest.entry.metadata.players = Players{1, 4};
        quest.entry.metadata.rating = Rating{7};
        quest.entry.metadata.description = "A line.\nAnother line.";
        quest.entry.flags.favorite = true;
        quest.entry.raw.unknownElements = {{"region", "eu"}};
        quest.media[MediaKind::Cover] = MediaPresence{true, ".png"};
        eclair = game("gb", "Eclair Nocturne.gb", "\xC3\x89" "clair Nocturne"); // Éclair: sorts after the ASCII titles
        eclair.entry.metadata.releaseDate = ReleaseDate{1992, 1, 1};
        eclair.sortKey = sortKeyOf(eclair.entry).value();
        racerUsa = game("gb", "Harbor Racer (USA).gb", "Harbor Racer");
        racerEurope = game("gb", "Harbor Racer (Europe).gb", "Harbor Racer");
        gone = game("gb", "Gone Pixel.gb", "Gone Pixel");
        gone.entry.state = EntryState::MissingFile;
        gone.entry.availability = Availability::Missing;
        gone.entry.file.reset();
        snesRacer = game("snes", "Pixel Racer.sfc", "Pixel Racer");
        snesRacer.entry.metadata.releaseDate = ReleaseDate{1993, 5, 1};
        readme = game("snes", "Readme.txt", "Readme");
        readme.entry.state = EntryState::NotAGame;
        readme.entry.notAGameReason = NotAGameReason::ExtensionNotConfigured;
        if (!withDatabase) {
            return;
        }
        auto database = rv::catalog_db::openCatalogDatabase(databaseFile());
        REQUIRE(database.has_value());
        auto store = rv::catalog_db::SqliteCatalogStore::open(*database);
        REQUIRE(store.has_value());
        auto transaction = (*store)->beginImport(LibraryId());
        REQUIRE(transaction.has_value());
        const LibraryProfile library = rv::test::testLibrary();
        REQUIRE((*transaction)->putLibrary(library).has_value());
        SystemInfo gb = rv::test::testSystem("gb", std::filesystem::path(L"E:\\roms\\gb"));
        gb.fullName = "Portable Console";
        SystemInfo snes = rv::test::testSystem("snes", std::filesystem::path(L"E:\\roms\\snes"));
        snes.fullName = "Home Console";
        REQUIRE((*transaction)->putSystem(library.id, gb).has_value());
        REQUIRE((*transaction)->putSystem(library.id, snes).has_value());
        for (const IndexedEntry* entry : all()) {
            REQUIRE((*transaction)->putIdentity(rv::test::identityOf(entry->id, library.id, entry->entry)).has_value());
            REQUIRE((*transaction)->putEntry(*entry).has_value());
        }
        REQUIRE((*transaction)->commit().has_value());
    }

    RunResult run(const std::vector<std::string>& arguments) const {
        std::vector<std::string> all{"--data-dir", rv::core::pathToUtf8(dataDir())};
        all.insert(all.end(), arguments.begin(), arguments.end());
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(all, out, err, testEnvironment(m_dir.path()));
        return RunResult{code, out.text(), err.text()};
    }

    std::vector<const IndexedEntry*> all() const {
        return {&quest, &eclair, &racerUsa, &racerEurope, &gone, &snesRacer, &readme};
    }

    [[nodiscard]] std::filesystem::path dataDir() const { return m_dir.file("data"); }
    [[nodiscard]] std::filesystem::path databaseFile() const { return dataDir() / "database" / "retroverse.sqlite"; }
    [[nodiscard]] const rv::test::TempDir& dir() const { return m_dir; }

    IndexedEntry quest;
    IndexedEntry eclair;
    IndexedEntry racerUsa;
    IndexedEntry racerEurope;
    IndexedEntry gone;
    IndexedEntry snesRacer;
    IndexedEntry readme;

private:
    rv::test::TempDir m_dir;
};

/// The two "Harbor Racer" of gb in Title's order: their sort keys tie, the GameId decides.
std::vector<const IndexedEntry*> racers(const CatalogCli& cli) {
    std::vector<const IndexedEntry*> both{&cli.racerUsa, &cli.racerEurope};
    std::sort(both.begin(), both.end(), [](const IndexedEntry* a, const IndexedEntry* b) { return a->id < b->id; });
    return both;
}

std::string idOf(const IndexedEntry& entry) {
    return entry.id.toString();
}

} // namespace

TEST_CASE("cli/catalog: systems lists the systems in System order with their counts, as text and as JSON",
          "[cli][catalog_db]") {
    const LoggingReset loggingReset;
    const CatalogCli cli("rv-cli-systems");
    const RunResult text = cli.run({"systems"});
    CHECK(text.code == ExitCode::Ok);
    CHECK(text.err.empty());
    CHECK(text.out == "snes  Home Console           1 games       0 unavailable       2 entries\n"
                      "gb    Portable Console       4 games       1 unavailable       5 entries\n"
                      "2 systems: 5 games, 1 unavailable, 7 entries\n");

    const RunResult json = cli.run({"systems", "--json"});
    CHECK(json.code == ExitCode::Ok);
    const auto parsed = nlohmann::json::parse(json.out);
    CHECK(parsed.at("count") == 2);
    CHECK(parsed.at("games") == 5);
    CHECK(parsed.at("unavailable") == 1);
    CHECK(parsed.at("entries") == 7);
    const auto& gb = parsed.at("systems").at(1);
    CHECK(gb.at("library") == "esde");
    CHECK(gb.at("name") == "gb");
    CHECK(gb.at("fullName") == "Portable Console");
    CHECK(gb.at("platforms") == nlohmann::json::array({"gb", "test-platform"}));
    CHECK(gb.at("romFolder") == "E:\\roms\\gb");
    CHECK(gb.at("romFolderConfigured") == "%ROMPATH%\\gb");
    CHECK(gb.at("availability") == "available");
    CHECK(gb.at("gamelist") == "read");
    CHECK(gb.at("games") == 4);
    CHECK(gb.at("unavailable") == 1);
    CHECK(gb.at("entries") == 5);
}

TEST_CASE("cli/catalog: games lists games in a total order, a system's, filtered, sorted and a page at a time",
          "[cli][catalog_db]") {
    const LoggingReset loggingReset;
    const CatalogCli cli("rv-cli-games");
    const auto racer = racers(cli);

    const RunResult gb = cli.run({"games", "--system", "gb"});
    CHECK(gb.code == ExitCode::Ok);
    CHECK(gb.err.empty());
    CHECK(gb.out == fmt::format("{}  gb  Harbor Racer\n"
                                "{}  gb  Harbor Racer\n"
                                "{}  gb  Pixel Quest (1997)\n"
                                "{}  gb  \xC3\x89" "clair Nocturne (1992)\n"
                                "4 games\n",
                                idOf(*racer[0]), idOf(*racer[1]), idOf(cli.quest), idOf(cli.eclair)));

    // Every system; the unavailable entries asked for; other states; another order; a page.
    const RunResult every = cli.run({"games", "--include-unavailable"});
    CHECK(linesOf(every.out).size() == 7);
    CHECK(contains(every.out, fmt::format("{}  gb    Gone Pixel [missing-file] [missing]\n", idOf(cli.gone))));
    CHECK(every.out.ends_with("6 games\n"));
    const RunResult missing = cli.run({"games", "--availability", "missing"}); // implies --include-unavailable
    CHECK(missing.out == fmt::format("{}  gb  Gone Pixel [missing-file] [missing]\n1 game\n", idOf(cli.gone)));
    const RunResult other = cli.run({"games", "--state", "not-a-game, folder"});
    CHECK(other.out == fmt::format("{}  snes  Readme [not-a-game]\n1 entry\n", idOf(cli.readme)));
    const RunResult byDate = cli.run({"games", "--sort", "release-date", "--limit", "2"});
    CHECK(byDate.out == fmt::format("{}  gb    \xC3\x89" "clair Nocturne (1992)\n"
                                    "{}  snes  Pixel Racer (1993)\n"
                                    "1 to 2 of 5 games\n",
                                    idOf(cli.eclair), idOf(cli.snesRacer)));
    const RunResult page = cli.run({"games", "--sort", "system", "--offset", "1", "--limit", "2"});
    CHECK(page.out == fmt::format("{}  gb  Harbor Racer\n{}  gb  Harbor Racer\n2 to 3 of 5 games\n", idOf(*racer[0]),
                                  idOf(*racer[1])));
    CHECK(cli.run({"games", "--offset", "9"}).out == "None from position 10: the list holds 5 games\n");

    // JSON: the query, the count, the page, the rows with every field.
    const RunResult json = cli.run({"games", "--system", "gb", "--limit", "3", "--offset", "2", "--json"});
    CHECK(json.code == ExitCode::Ok);
    const auto parsed = nlohmann::json::parse(json.out);
    CHECK(parsed.at("count") == 4);
    CHECK(parsed.at("offset") == 2);
    CHECK(parsed.at("limit") == 3);
    CHECK(parsed.at("query").at("systems") == nlohmann::json::array({"gb"}));
    CHECK(parsed.at("query").at("sort") == "title");
    CHECK(parsed.at("query").at("includeUnavailable") == false);
    REQUIRE(parsed.at("games").size() == 2);
    const auto& row = parsed.at("games").at(0);
    CHECK(row.at("id") == idOf(cli.quest));
    CHECK(row.at("system") == "gb");
    CHECK(row.at("systemFullName") == "Portable Console");
    CHECK(row.at("title") == "Pixel Quest");
    CHECK(row.at("state") == "game");
    CHECK(row.at("availability") == "available");
    CHECK(row.at("releaseDate") == "1997-07-02");
    CHECK(row.at("year") == 1997);
    CHECK(row.at("developer") == "Mirage Soft");
    CHECK(row.at("publisher") == "\xC3\x89" "ditions Pixel");
    CHECK(row.at("genre") == "Role Playing Game");
    CHECK(row.at("players") == "1-4");
    CHECK(row.at("rating") == 0.7);
    CHECK(row.at("favorite") == true);
    CHECK(row.at("description") == "A line.\nAnother line.");
    CHECK(parsed.at("games").at(1).at("releaseDate") == "1992-01-01");
    CHECK(parsed.at("games").at(1).at("players").is_null());

    // Usage errors, before the database is opened.
    for (const std::vector<std::string>& wrong :
         {std::vector<std::string>{"games", "--sort", "price"}, std::vector<std::string>{"games", "--state", "gone"},
          std::vector<std::string>{"games", "--availability", "maybe"},
          std::vector<std::string>{"games", "--limit", "-1"}, std::vector<std::string>{"games", "--offset", "x"},
          std::vector<std::string>{"games", "--system", "a/b"}, std::vector<std::string>{"games", "extra"}}) {
        INFO(fmt::format("{}", fmt::join(wrong, " ")));
        const RunResult result = cli.run(wrong);
        CHECK(result.code == ExitCode::UsageError);
        CHECK(result.out.empty());
        CHECK(contains(result.err, "retroverse-cli games: "));
    }
    // A system the catalog does not have is named.
    const RunResult unknown = cli.run({"games", "--system", "n64"});
    CHECK(unknown.code == ExitCode::UsageError);
    CHECK(unknown.out.empty());
    CHECK(contains(unknown.err, "the catalog has no system 'n64'"));
}

TEST_CASE("cli/catalog: search finds by words in any order, the last one a prefix, diacritics ignored, its syntax "
          "inert",
          "[cli][catalog_db]") {
    const LoggingReset loggingReset;
    const CatalogCli cli("rv-cli-search");
    const RunResult pixel = cli.run({"search", "pixel"});
    CHECK(pixel.code == ExitCode::Ok);
    CHECK(pixel.err.empty());
    CHECK(pixel.out == fmt::format("{}  gb    Pixel Quest (1997)\n{}  snes  Pixel Racer (1993)\n2 games\n",
                                   idOf(cli.quest), idOf(cli.snesRacer)));
    CHECK(cli.run({"search", "racer pix"}).out ==
          fmt::format("{}  snes  Pixel Racer (1993)\n1 game\n", idOf(cli.snesRacer)));
    CHECK(cli.run({"search", "ECLAIR"}).out ==
          fmt::format("{}  gb  \xC3\x89" "clair Nocturne (1992)\n1 game\n", idOf(cli.eclair)));
    CHECK(cli.run({"search", "editions"}).out == fmt::format("{}  gb  Pixel Quest (1997)\n1 game\n", idOf(cli.quest)));
    CHECK(cli.run({"search", "home console"}).out ==
          fmt::format("{}  snes  Pixel Racer (1993)\n1 game\n", idOf(cli.snesRacer)));
    CHECK(cli.run({"search", "pixel", "--system", "gb"}).out ==
          fmt::format("{}  gb  Pixel Quest (1997)\n1 game\n", idOf(cli.quest)));
    CHECK(cli.run({"search", "pixel", "--include-unavailable", "--system", "gb"}).out ==
          fmt::format("{}  gb  Gone Pixel [missing-file] [missing]\n{}  gb  Pixel Quest (1997)\n2 games\n",
                      idOf(cli.gone), idOf(cli.quest)));
    CHECK(cli.run({"search", "h"}).out.ends_with("2 games\n")); // one character: the titles that start with it
    CHECK(cli.run({"search", "zzz"}).out == "0 games\n");
    // FTS5 syntax is text: no error, the words alone.
    for (const std::string_view text : {"pixel OR racer", "title:pixel", "\"pixel", "NEAR(pixel racer)", "pixel*",
                                        "(pixel", "^pixel", "pixel AND NOT"}) {
        INFO(text);
        const RunResult result = cli.run({"search", std::string(text)});
        CHECK(result.code == ExitCode::Ok);
        CHECK(result.err.empty());
    }
    CHECK(cli.run({"search", "--", "-pixel"}).out == pixel.out);
    CHECK(cli.run({"search", "pixel OR racer"}).out == "0 games\n");

    const auto json = nlohmann::json::parse(cli.run({"search", "Pixel qu", "--json"}).out);
    CHECK(json.at("query").at("text") == "Pixel qu");
    CHECK(json.at("query").at("search") == "words");
    CHECK(json.at("query").at("words") == nlohmann::json::array({"pixel", "qu"}));
    CHECK(json.at("count") == 1);
    CHECK(json.at("limit").is_null());
    CHECK(json.at("games").at(0).at("id") == idOf(cli.quest));
    CHECK(nlohmann::json::parse(cli.run({"search", "p", "--json"}).out).at("query").at("search") ==
          "first-character");

    // A search needs its text.
    const RunResult none = cli.run({"search"});
    CHECK(none.code == ExitCode::UsageError);
    CHECK(none.out.empty());
}

TEST_CASE("cli/catalog: show prints everything the catalog holds about a game; an unknown game exits 1",
          "[cli][catalog_db]") {
    const LoggingReset loggingReset;
    const CatalogCli cli("rv-cli-show");
    const RunResult text = cli.run({"show", idOf(cli.quest)});
    CHECK(text.code == ExitCode::Ok);
    CHECK(text.err.empty());
    const std::vector<std::string> lines = linesOf(text.out);
    const auto has = [&](std::string_view line) {
        INFO(line);
        CHECK(std::find(lines.begin(), lines.end(), line) != lines.end());
    };
    has(fmt::format("Game:                 {}", idOf(cli.quest)));
    has("Title:                Pixel Quest");
    has("System:               gb (Portable Console), library esde");
    has("State:                game");
    has("Availability:         available");
    has("Relative path:        Pixel Quest (World).gb");
    has("Path:                 E:\\roms\\gb\\Pixel Quest (World).gb");
    has("Gamelist entry:       ./Pixel Quest (World).gb");
    has("Sort key:             pixel quest");
    has("Release date:         1997-07-02");
    has("Developer:            Mirage Soft");
    has("Publisher:            \xC3\x89" "ditions Pixel");
    has("Players:              1-4");
    has("Rating:               0.7");
    has("Flags:                favorite");
    has("Media:                covers (.png)");
    has("Gamelist element:     <region> eu");
    has("Description:");
    has("  Another line.");
    CHECK(contains(text.out, "File:                 65536 bytes, written "));

    const auto json = nlohmann::json::parse(cli.run({"show", idOf(cli.readme), "--json"}).out);
    CHECK(json.at("id") == idOf(cli.readme));
    CHECK(json.at("library") == "esde");
    CHECK(json.at("system") == "snes");
    CHECK(json.at("systemFullName") == "Home Console");
    CHECK(json.at("relativePath") == "Readme.txt");
    CHECK(json.at("path") == "E:\\roms\\snes\\Readme.txt");
    CHECK(json.at("state") == "not-a-game");
    CHECK(json.at("notAGameReason") == "extension-not-configured");
    CHECK(json.at("file").at("size") == 65536);
    CHECK(json.at("file").at("lastWriteTime").get<std::string>().ends_with("Z"));
    CHECK(json.at("metadata").at("name") == "Readme");
    CHECK(json.at("metadata").at("releaseDate").is_null());
    CHECK(json.at("media").empty());
    CHECK(json.at("flags").at("favorite") == false);
    const auto gone = nlohmann::json::parse(cli.run({"show", idOf(cli.gone), "--json"}).out);
    CHECK(gone.at("file").is_null());
    CHECK(gone.at("availability") == "missing");
    CHECK(contains(cli.run({"show", idOf(cli.gone)}).out, "File:                 not seen by the last import\n"));

    const RunResult unknown = cli.run({"show", "g0000000000000042"});
    CHECK(unknown.code == ExitCode::ProblemsFound);
    CHECK(unknown.out.empty());
    CHECK(unknown.err == "retroverse-cli show: the catalog has no game g0000000000000042\n");
    for (const std::string_view wrong : {"42", "g42", "G0000000000000042", "g0000000000000000"}) {
        INFO(wrong);
        const RunResult result = cli.run({"show", std::string(wrong)});
        CHECK(result.code == ExitCode::UsageError);
        CHECK(result.out.empty());
    }
}

TEST_CASE("cli/catalog: without a database the commands exit 3 and make none; --output and --help",
          "[cli][catalog_db]") {
    const LoggingReset loggingReset;
    const CatalogCli empty("rv-cli-catalog-none", false);
    for (const std::vector<std::string>& command :
         {std::vector<std::string>{"systems"}, std::vector<std::string>{"games"},
          std::vector<std::string>{"search", "pixel"}, std::vector<std::string>{"show", "g0000000000000042"}}) {
        INFO(command.front());
        const RunResult result = empty.run(command);
        CHECK(result.code == ExitCode::RuntimeFailure);
        CHECK(result.out.empty());
        CHECK(contains(result.err, "cannot read the catalog"));
        CHECK(contains(result.err, "'retroverse-cli import' makes it"));
    }
    CHECK_FALSE(std::filesystem::exists(empty.databaseFile()));

    // A database no migration ran on: migrate first.
    {
        auto database = rv::catalog_db::Database::open(empty.databaseFile());
        REQUIRE(database.has_value());
    }
    const RunResult older = empty.run({"games"});
    CHECK(older.code == ExitCode::RuntimeFailure);
    CHECK(contains(older.err, "Run 'retroverse-cli db migrate' first."));

    const CatalogCli cli("rv-cli-catalog-output");
    const std::filesystem::path file = cli.dir().file("out/systems.txt");
    const RunResult written = cli.run({"systems", "--output", rv::core::pathToUtf8(file)});
    CHECK(written.code == ExitCode::Ok);
    CHECK(written.out.empty());
    CHECK(rv::core::readTextFile(file).value() == cli.run({"systems"}).out);
    for (const std::string_view command : {"systems", "games", "search", "show"}) {
        INFO(command);
        const RunResult help = cli.run({std::string(command), "--help"});
        CHECK(help.code == ExitCode::Ok);
        CHECK(help.out.starts_with(fmt::format("Usage: retroverse-cli [global options] {}", command)));
        CHECK(help.err.empty());
    }
}
