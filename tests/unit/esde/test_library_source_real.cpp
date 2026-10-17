// The real library through rv::esde::EsdeLibrarySource (I2 step 7, ADR-0008 §10). Hidden ([.real-library]): ctest never
// runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after, and
// the folders in environment variables:
//   RV_REAL_ESDE_HOME     the ES-DE home (settings, gamelists)
//   RV_REAL_ESDE_INSTALL  the ES-DE install folder (es_systems.xml, the MAME lists)
//   RV_REAL_ROM_ROOT      ES-DE's ROMDirectory, given as the library's ROM root (the same folder)
// The expected figures are the reference library's (ADR-0008, the survey of 2026-10-16, and scripts/esde_census.py):
// 103,067 games as ES-DE lists them, psx 2,519 entries and 12,820 disc members, ps2 3,593 games, 1,038 missing files,
// 837 ps2 entries whose extension ps2 does not configure. A library scraped or edited since changes them; the test
// prints its census table (the format of scripts/esde_census.py, so the two compare line by line) and the time of the
// load (informative: the gate times the import).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/esde/library_source.hpp>

namespace fs = std::filesystem;
using rv::catalog::EntryState;

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

/// The columns of scripts/esde_census.py's table.
constexpr std::array<std::string_view, 10> kColumns{"esde",    "entries", "games",   "missing", "notgame",
                                                    "members", "folders", "outside", "groups",  "unavail"};

struct Row {
    std::array<std::size_t, kColumns.size()> values{};
    bool operator==(const Row&) const = default;
};

/// A gamelist path as resolved for a listed file ("./a/b.gb" reads "a/b.gb"): what tells a path that differs from its
/// file in case only, which ES-DE shows as a second entry.
std::string resolvedPath(std::string path) {
    for (char& c : path) {
        c = c == '\\' ? '/' : c;
    }
    while (path.starts_with("./")) {
        path.erase(0, 2);
    }
    return path;
}

/// The census table of a snapshot, by system: the systems whose folder is there or that have entries.
std::map<std::string, Row> censusOf(const rv::catalog::LibrarySnapshot& snapshot,
                                    std::map<std::string, std::map<std::string, std::size_t>>& notAGame) {
    std::map<std::string, Row> rows;
    for (const rv::catalog::SystemInfo& system : snapshot.systems) {
        if (system.availability == rv::catalog::Availability::Available) {
            rows[system.name.str()];
        }
    }
    for (const rv::catalog::CatalogEntry& entry : snapshot.entries) {
        Row& row = rows[entry.system.str()];
        ++row.values[1];
        // ES-DE's count: the games and members on disk, and its second entry for a gamelist path that differs from its
        // file in case only.
        const bool listed = entry.availability == rv::catalog::Availability::Available;
        const bool game = listed && (entry.state == EntryState::Game || entry.state == EntryState::DiscMember);
        row.values[0] += game ? 1 : 0;
        if (game && entry.fromGamelist() && resolvedPath(entry.gamelistPath) != entry.key.relativePath()) {
            ++row.values[0];
        }
        switch (entry.state) {
        case EntryState::Game:
            ++row.values[2];
            break;
        case EntryState::MissingFile:
            ++row.values[3];
            break;
        case EntryState::NotAGame:
            ++row.values[4];
            ++notAGame[entry.system.str()][std::string(rv::catalog::toString(entry.notAGameReason))];
            break;
        case EntryState::DiscMember:
            ++row.values[5];
            break;
        case EntryState::Folder:
            ++row.values[6];
            break;
        case EntryState::OutsideSystemFolder:
            ++row.values[7];
            break;
        }
    }
    for (const rv::catalog::DiscGroup& group : snapshot.discGroups) {
        ++rows[group.system.str()].values[8];
    }
    for (const rv::catalog::UnavailableFolder& folder : snapshot.unavailableFolders) {
        ++rows[folder.system.str()].values[9];
    }
    return rows;
}

/// The table as scripts/esde_census.py prints it.
std::string tableText(const std::map<std::string, Row>& rows,
                      const std::map<std::string, std::map<std::string, std::size_t>>& notAGame) {
    std::string text = fmt::format("{:<16}", "system");
    for (const std::string_view column : kColumns) {
        text += fmt::format("{:>9}", column);
    }
    text += '\n';
    Row total;
    for (const auto& [name, row] : rows) {
        text += fmt::format("{:<16}", name);
        for (std::size_t i = 0; i < kColumns.size(); ++i) {
            text += fmt::format("{:>9}", row.values[i]);
            total.values[i] += row.values[i];
        }
        text += '\n';
    }
    text += fmt::format("{:<16}", "total");
    for (const std::size_t value : total.values) {
        text += fmt::format("{:>9}", value);
    }
    text += '\n';
    for (const auto& [name, reasons] : notAGame) {
        for (const auto& [reason, count] : reasons) {
            text += fmt::format("not a game: {} {} {}\n", name, reason, count);
        }
    }
    return text;
}

/// The reference library's systems with entries (scripts/esde_census.py on 2026-10-17): ES-DE's count, entries, games,
/// missing files, not games, disc members, folders, outside the system folder, disc groups, unavailable folders.
const std::map<std::string, Row> kExpected{
    {"amiga", {{4278, 4294, 4278, 0, 0, 0, 16, 0, 0, 0}}},
    {"amigacd32", {{201, 201, 201, 0, 0, 0, 0, 0, 0, 0}}},
    {"amstradcpc", {{2416, 2416, 2416, 0, 0, 0, 0, 0, 0, 0}}},
    {"apple2", {{668, 668, 668, 0, 0, 0, 0, 0, 0, 0}}},
    {"atari2600", {{579, 579, 579, 0, 0, 0, 0, 0, 0, 0}}},
    {"atari5200", {{96, 96, 96, 0, 0, 0, 0, 0, 0, 0}}},
    {"atari7800", {{1095, 1095, 1095, 0, 0, 0, 0, 0, 0, 0}}},
    {"atari800", {{3111, 3761, 3111, 649, 0, 0, 1, 0, 0, 0}}},
    {"atarijaguar", {{712, 713, 712, 0, 0, 0, 1, 0, 0, 0}}},
    {"atarilynx", {{562, 562, 562, 0, 0, 0, 0, 0, 0, 0}}},
    {"atarist", {{314, 314, 314, 0, 0, 0, 0, 0, 0, 0}}},
    {"c64", {{13026, 13031, 13026, 0, 0, 0, 5, 0, 0, 0}}},
    {"channelf", {{28, 28, 28, 0, 0, 0, 0, 0, 0, 0}}},
    {"colecovision", {{1165, 1167, 1165, 0, 0, 0, 2, 0, 0, 0}}},
    {"dos", {{74, 89, 74, 1, 0, 0, 14, 0, 0, 0}}},
    {"dreamcast", {{278, 278, 278, 0, 0, 0, 0, 0, 0, 0}}},
    {"fds", {{194, 194, 194, 0, 0, 0, 0, 0, 0, 0}}},
    {"gb", {{8611, 8621, 8611, 1, 0, 0, 9, 0, 0, 0}}},
    {"gba", {{4863, 4879, 4863, 0, 0, 0, 16, 0, 0, 0}}},
    {"gbc", {{2841, 2850, 2841, 0, 0, 0, 9, 0, 0, 0}}},
    {"gc", {{608, 679, 608, 67, 0, 0, 4, 0, 0, 0}}},
    {"intellivision", {{160, 160, 160, 0, 0, 0, 0, 0, 0, 0}}},
    {"mame", {{6862, 7083, 6862, 187, 33, 0, 1, 0, 0, 0}}},
    {"mastersystem", {{3022, 3025, 3022, 0, 0, 0, 3, 0, 0, 0}}},
    {"megadrive", {{4615, 4623, 4615, 0, 0, 0, 8, 0, 0, 0}}},
    {"n64", {{1889, 1889, 1889, 0, 0, 0, 0, 0, 0, 0}}},
    {"nds", {{1893, 1973, 1893, 79, 0, 0, 1, 0, 0, 0}}},
    {"neogeo", {{2, 10, 2, 7, 0, 0, 1, 0, 0, 0}}},
    {"nes", {{7012, 7033, 7012, 0, 0, 0, 21, 0, 0, 0}}},
    {"ngpc", {{204, 204, 204, 0, 0, 0, 0, 0, 0, 0}}},
    {"odyssey2", {{83, 83, 83, 0, 0, 0, 0, 0, 0, 0}}},
    {"pc", {{21, 21, 21, 0, 0, 0, 0, 0, 0, 0}}},
    {"pcengine", {{1538, 1540, 1538, 0, 0, 0, 2, 0, 0, 0}}},
    {"pcfx", {{12, 12, 12, 0, 0, 0, 0, 0, 0, 0}}},
    {"pokemini", {{11, 11, 11, 0, 0, 0, 0, 0, 0, 0}}},
    {"ps2", {{3593, 4475, 3593, 45, 837, 0, 0, 0, 0, 0}}},
    {"ps3", {{13, 13, 13, 0, 0, 0, 0, 0, 0, 0}}},
    {"psp", {{12, 12, 12, 0, 0, 0, 0, 0, 0, 0}}},
    {"psx", {{15339, 15341, 2519, 2, 0, 12820, 0, 0, 2512, 0}}},
    {"saturn", {{286, 287, 285, 0, 0, 1, 1, 0, 1, 0}}},
    {"segacd", {{1, 1, 1, 0, 0, 0, 0, 0, 0, 0}}},
    {"snes", {{7797, 7835, 7797, 0, 0, 0, 38, 0, 0, 0}}},
    {"switch", {{229, 229, 229, 0, 0, 0, 0, 0, 0, 0}}},
    {"trs-80", {{211, 212, 211, 0, 0, 0, 1, 0, 0, 0}}},
    {"vic20", {{2042, 2042, 2042, 0, 0, 0, 0, 0, 0, 0}}},
    {"virtualboy", {{429, 430, 429, 0, 0, 0, 1, 0, 0, 0}}},
    {"wii", {{70, 70, 70, 0, 0, 0, 0, 0, 0, 0}}},
    {"windows", {{1, 1, 1, 0, 0, 0, 0, 0, 0, 0}}},
};

} // namespace

TEST_CASE("esde/library_source: the real library", "[esde][library_source][.real-library]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    if (!home || !install || !romRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL and RV_REAL_ROM_ROOT (scripts/real-library.ps1 does)");
    }
    rv::catalog::LibraryProfile profile;
    profile.esdeHome = *home;
    profile.installFolder = *install;
    profile.romRoot = *romRoot;

    rv::esde::EsdeLibrarySource source;
    auto loaded = source.load(profile, {});
    INFO((loaded ? std::string("loaded") : loaded.error().toString()));
    REQUIRE(loaded.has_value());
    const rv::catalog::LibrarySnapshot& snapshot = *loaded;
    const rv::esde::EsdeLoadStatistics& statistics = source.lastStatistics();

    std::map<std::string, std::map<std::string, std::size_t>> notAGame;
    const std::map<std::string, Row> rows = censusOf(snapshot, notAGame);
    std::map<std::string, std::size_t> issueCodes;
    for (const rv::catalog::Issue& issue : snapshot.issues) {
        ++issueCodes[issue.code];
    }
    const auto seconds = [](std::chrono::nanoseconds time) {
        return std::chrono::duration<double>(time).count();
    };
    std::cout << "esde/library_source real library, census table:\n" << tableText(rows, notAGame);
    std::cout << fmt::format("loaded in {:.2f} s (configuration {:.2f} s, scan {:.2f} s, gamelists {:.2f} s, sheets "
                             "{:.2f} s); {} systems, {} scanned, {} files and {} folders listed, {} gamelists, {} "
                             "sheets read, {} unreadable\n",
                             seconds(statistics.totalTime), seconds(statistics.configurationTime),
                             seconds(statistics.scanTime), seconds(statistics.gamelistTime),
                             seconds(statistics.sheetTime), statistics.systemsConfigured, statistics.systemsScanned,
                             statistics.filesListed, statistics.foldersListed, statistics.gamelistsRead,
                             statistics.sheetsRead, statistics.sheetsUnreadable);
    for (const auto& [code, count] : issueCodes) {
        std::cout << "issue " << code << ": " << count << "\n";
    }
    for (const rv::catalog::Issue& issue : snapshot.issues) {
        UNSCOPED_INFO(rv::catalog::toString(issue.severity)
                      << " " << issue.code << " " << issue.system << ": " << issue.reason);
    }

    // The figures of ADR-0008 and the survey.
    Row total;
    for (const auto& [name, row] : rows) {
        for (std::size_t i = 0; i < kColumns.size(); ++i) {
            total.values[i] += row.values[i];
        }
    }
    CHECK(total.values[0] == 103067); // as ES-DE lists them
    CHECK(rows.at("psx").values[2] == 2519);
    CHECK(rows.at("psx").values[5] == 12820);
    CHECK(rows.at("ps2").values[2] == 3593);
    CHECK(total.values[3] == 1038);
    CHECK(notAGame["ps2"]["extension-not-configured"] == 837);
    CHECK(notAGame["mame"]["bios-or-device-set"] == 33);
    CHECK(total.values[6] == 155); // every <folder> entry names a folder that is there
    CHECK(total.values[7] == 0);
    CHECK(snapshot.unavailableFolders.empty());

    // Every system, against the census: the systems with entries as expected, the others empty.
    for (const auto& [name, row] : rows) {
        INFO(name);
        const auto expected = kExpected.find(name);
        if (expected == kExpected.end()) {
            CHECK(row == Row{});
        } else {
            CHECK(row == expected->second);
        }
    }
    for (const auto& [name, row] : kExpected) {
        INFO(name);
        CHECK(rows.contains(name));
    }
    CHECK(snapshot.systems.size() == 195);
    std::size_t available = 0;
    for (const rv::catalog::SystemInfo& system : snapshot.systems) {
        available += system.availability == rv::catalog::Availability::Available ? 1 : 0;
    }
    CHECK(available == 191);

    // The snapshot's own rules: entries in order, one per system and relative path, every game titled, every member's
    // head a game of its system (or a member itself, along a chain).
    CHECK(std::is_sorted(snapshot.entries.begin(), snapshot.entries.end(), rv::catalog::entryOrder));
    std::set<std::pair<std::string, std::string>> paths;
    std::set<std::pair<std::string, std::string>> games;
    for (const rv::catalog::CatalogEntry& entry : snapshot.entries) {
        if (!entry.key.empty()) {
            CHECK(paths.emplace(entry.system.str(), entry.key.relativePath()).second);
        }
        if (entry.state == EntryState::Game || entry.state == EntryState::DiscMember) {
            CHECK_FALSE(entry.title.empty());
            games.emplace(entry.system.str(), entry.key.relativePath());
        }
    }
    for (const rv::catalog::CatalogEntry& entry : snapshot.entries) {
        if (entry.state == EntryState::DiscMember) {
            CHECK(games.contains({entry.system.str(), entry.discHead.relativePath()}));
        }
    }
}
