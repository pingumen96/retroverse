// The media coverage of the real library through retroverse-cli media --coverage (I2 step 8, ADR-0008 §10, ADR-0003),
// over the library snapshot of the ROM scan (I2 step 7, esde::EsdeLibrarySource). Hidden ([.real-library]): ctest never
// runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after, and
// the folders in environment variables:
//   RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL  the ES-DE home and install folder (library create's --home is the home's
//                                            parent, as ES-DE's own --home)
//   RV_REAL_ROM_ROOT, RV_REAL_MEDIA_ROOT     the ROM and media roots, recorded as library.json's overrides
// The user data directory (library.json, the log) is a temporary folder outside the library.
//
// The expected figures come from two independent oracles, run once on the reference machine, their figures frozen here.
//  - The gamelist populations (step 8): a Python count over the survey's read-only listings of 2026-10-16 (ElementTree
//    over every gamelist, os.scandir listings of the ROM and media trees), which applies ES-DE's rule on its own: the
//    media name is the ROM path with its last extension removed, names compared case-insensitively, the kind's
//    extensions as ES-DE 3.4.1 reads them. Its per-system figures of the games with a gamelist entry are those of
//    esde-data.md section 5, except snes' videos (1,462, not 1,466: the survey counted the four .flv files ES-DE never
//    reads). Those "listed games" are ES-DE's, every disc track a game; the snapshot groups nine scraped tracks under
//    their sheet (disc members, in no games population), so its scraped games are those less the tracks.
//  - The games of the snapshot (step 7-8i): scripts/esde_census.py's entries (step 7's oracle of EsdeLibrarySource,
//    its System class driven as its census() drives it: 90,246 games) joined with an os.scandir listing of the media
//    root made apart from the C++, ES-DE's media rule applied in Python as above, on 2026-10-17 (docs/i2/step-8.md,
//    "Integration with step 7"). It gives the gamelist populations above again, entry for entry.
// ADR-0003's table (2026-10-15) states shares over the 40,870 <game> entries, but four of its seven media rows cannot
// be reproduced to 0.1 point over them, nor over any single population (docs/i2/step-8.md). The test checks the table
// as docs/i2/step-8.md proposes to amend it at the merge (the shares over every <game> entry, within 0.1 point), prints
// the figures as written beside them, and checks the per-system figures ADR-0003 quotes (Atari 800, Commodore 64),
// which the <game> entries reproduce as written.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <windows.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <rv/catalog/media.hpp>
#include <rv/cli/command.hpp>
#include <rv/cli/output.hpp>
#include <rv/core/json.hpp>
#include <rv/core/log.hpp>
#include <rv/core/paths.hpp>
#include <rv/core/text.hpp>

#include "support/temp_dir.hpp"

namespace fs = std::filesystem;
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

/// The counts of one population per kind, in the order of kMediaKinds, then the entries with any kind.
struct Figures {
    std::uint64_t entries;
    std::array<std::uint64_t, rv::catalog::kMediaKindCount> withKind;
    std::uint64_t withAny;
};

/// `a` less `b`, count by count.
constexpr Figures minus(const Figures& a, const Figures& b) {
    Figures result{a.entries - b.entries, {}, a.withAny - b.withAny};
    for (std::size_t i = 0; i < result.withKind.size(); ++i) {
        result.withKind[i] = a.withKind[i] - b.withKind[i];
    }
    return result;
}

// The oracles' figures: the members, then covers, backcovers, 3dboxes, physicalmedia, screenshots, titlescreens,
// marquees, miximages, fanart, manuals, videos, custom, then the members with any kind.
// Every <game> entry, those whose file exists (step 8).
constexpr Figures kGamelistEntries{
    40870, {37151, 31768, 37012, 33302, 40359, 38842, 36468, 40357, 20002, 23110, 33592, 0}, 40561};
constexpr Figures kWithFile{
    39832, {36549, 31442, 36502, 33003, 39413, 38058, 36003, 39411, 19780, 22803, 33249, 0}, 39563};
/// The games with a gamelist entry as ES-DE lists them, every disc track a game (step 8: the survey's "listed games").
constexpr Figures kListedGames{
    38995, {35719, 30730, 35672, 32223, 38581, 37314, 35175, 38579, 19436, 22453, 32424, 0}, 38729};
/// The scraped disc tracks the snapshot groups under their sheet, no games of it (step 7-8i): eight psx .bin tracks
/// referenced by a cue sheet (FIFA Football 2003, Final Fantasy IX disc 1, International Track & Field, Metal Slug X,
/// This Is Football 2 twice, Wild Arms, The X-Files disc 1), each with a cover, a screenshot, a video and a marquee,
/// and saturn's Sega Rally Championship track 01, without media.
constexpr Figures kScrapedTracks{9, {8, 7, 8, 8, 8, 8, 8, 8, 6, 3, 8, 0}, 8};
/// The scraped games of the snapshot: 38,986.
constexpr Figures kScrapedGames = minus(kListedGames, kScrapedTracks);
/// Every game of the snapshot, scraped or not (step 7-8i's oracle).
constexpr Figures kGames{
    90246, {36010, 30869, 35960, 32397, 39237, 37888, 35397, 39221, 19482, 22618, 32581, 0}, 39404};
/// Every entry of the snapshot, the report's "entries" (step 7's figure, its states as the oracle gives them): the
/// games, 12,821 disc members, 1,038 missing files, 870 entries that are no game, 155 folders.
constexpr std::uint64_t kSnapshotEntries = 90246 + 12821 + 1038 + 870 + 155;
static_assert(kSnapshotEntries == 105130);

/// One system's games of a population, and those of them with a cover, a screenshot, a video and a marquee.
struct SystemRow {
    std::string_view system;
    std::uint64_t games, covers, screenshots, videos, marquees;
};

/// esde-data.md section 5 per system: the games with a gamelist entry as ES-DE lists them (snes' videos without the
/// four .flv files). The snapshot's scraped games are these less kScrapedTrackRows.
constexpr std::array<SystemRow, 45> kSurvey{{
    {"amiga", 2189, 1975, 2182, 1669, 1810},
    {"amigacd32", 1, 1, 1, 1, 1},
    {"amstradcpc", 2397, 1936, 2397, 2013, 1889},
    {"apple2", 662, 505, 653, 491, 513},
    {"atari2600", 569, 569, 569, 567, 569},
    {"atari5200", 96, 96, 96, 96, 95},
    {"atari7800", 216, 216, 216, 216, 216},
    {"atari800", 2523, 1249, 2460, 392, 858},
    {"atarijaguar", 339, 338, 338, 337, 338},
    {"atarilynx", 360, 327, 360, 313, 360},
    {"atarist", 312, 304, 312, 209, 272},
    {"c64", 1012, 535, 914, 118, 296},
    {"channelf", 27, 27, 27, 27, 27},
    {"colecovision", 1010, 886, 1008, 720, 1010},
    {"dos", 20, 10, 10, 6, 10},
    {"dreamcast", 89, 87, 87, 87, 87},
    {"fds", 190, 190, 190, 190, 190},
    {"gb", 4452, 4374, 4448, 4211, 4439},
    {"gba", 4512, 4443, 4504, 4224, 4504},
    {"gbc", 2676, 2499, 2664, 2205, 2549},
    {"gc", 538, 536, 536, 536, 536},
    {"intellivision", 156, 156, 155, 154, 155},
    {"mame", 76, 36, 36, 35, 36},
    {"mastersystem", 2237, 2237, 2229, 2221, 2237},
    {"megadrive", 1000, 997, 998, 996, 998},
    {"n64", 2, 2, 2, 2, 2},
    {"nds", 115, 115, 114, 108, 114},
    {"neogeo", 1, 0, 0, 0, 0},
    {"nes", 4065, 4042, 4060, 3356, 4060},
    {"ngpc", 36, 28, 34, 35, 28},
    {"odyssey2", 83, 83, 83, 82, 83},
    {"pcengine", 4, 1, 1, 1, 1},
    {"pokemini", 11, 11, 11, 11, 11},
    {"ps2", 3061, 2993, 2973, 2979, 3001},
    {"ps3", 6, 6, 2, 0, 2},
    {"psp", 12, 12, 12, 12, 12},
    {"psx", 2233, 2228, 2228, 2223, 2227},
    {"saturn", 8, 2, 2, 2, 2},
    {"segacd", 1, 1, 1, 1, 1},
    {"snes", 1560, 1535, 1535, 1462, 1507},
    {"trs-80", 1, 0, 0, 0, 0},
    {"vic20", 7, 3, 7, 7, 1},
    {"virtualboy", 126, 125, 123, 106, 125},
    {"wii", 3, 2, 2, 2, 2},
    {"windows", 1, 1, 1, 1, 1},
}};
/// The scraped disc tracks (kScrapedTracks) per system.
constexpr std::array<SystemRow, 2> kScrapedTrackRows{{{"psx", 8, 8, 8, 8, 8}, {"saturn", 1, 0, 0, 0, 0}}};

/// Every game of the snapshot per system (step 7-8i's oracle; the games are step 7's census per system), in name order
/// (ordinal), as the report lists its systems: the 45 systems with a gamelist and pc, pcfx and switch, whose games have
/// no gamelist entry and no media folder.
constexpr std::array<SystemRow, 48> kGameRows{{
    {"amiga", 4278, 1976, 2185, 1669, 1811},
    {"amigacd32", 201, 1, 1, 1, 1},
    {"amstradcpc", 2416, 1936, 2397, 2013, 1889},
    {"apple2", 668, 505, 653, 491, 513},
    {"atari2600", 579, 569, 570, 567, 569},
    {"atari5200", 96, 96, 96, 96, 95},
    {"atari7800", 1095, 217, 217, 216, 217},
    {"atari800", 3111, 1435, 2994, 459, 979},
    {"atarijaguar", 712, 339, 339, 337, 339},
    {"atarilynx", 562, 327, 360, 313, 360},
    {"atarist", 314, 304, 312, 209, 272},
    {"c64", 13026, 540, 930, 118, 296},
    {"channelf", 28, 27, 27, 27, 27},
    {"colecovision", 1165, 886, 1008, 720, 1010},
    {"dos", 74, 10, 10, 6, 10},
    {"dreamcast", 278, 88, 88, 87, 88},
    {"fds", 194, 190, 190, 190, 190},
    {"gb", 8611, 4376, 4450, 4213, 4441},
    {"gba", 4863, 4446, 4508, 4227, 4507},
    {"gbc", 2841, 2501, 2667, 2206, 2551},
    {"gc", 608, 536, 536, 536, 536},
    {"intellivision", 160, 156, 155, 154, 155},
    {"mame", 6862, 36, 36, 35, 36},
    {"mastersystem", 3022, 2238, 2231, 2221, 2238},
    {"megadrive", 4615, 998, 1000, 996, 999},
    {"n64", 1889, 2, 2, 2, 2},
    {"nds", 1893, 115, 114, 108, 114},
    {"neogeo", 2, 0, 0, 0, 0},
    {"nes", 7012, 4043, 4061, 3357, 4061},
    {"ngpc", 204, 28, 34, 35, 28},
    {"odyssey2", 83, 83, 83, 82, 83},
    {"pc", 21, 0, 0, 0, 0},
    {"pcengine", 1538, 1, 1, 1, 1},
    {"pcfx", 12, 0, 0, 0, 0},
    {"pokemini", 11, 11, 11, 11, 11},
    {"ps2", 3593, 3085, 3063, 3067, 3093},
    {"ps3", 13, 6, 2, 0, 2},
    {"psp", 12, 12, 12, 12, 12},
    {"psx", 2519, 2221, 2221, 2216, 2220},
    {"saturn", 285, 2, 2, 2, 2},
    {"segacd", 1, 1, 1, 1, 1},
    {"snes", 7797, 1535, 1536, 1463, 1508},
    {"switch", 229, 0, 0, 0, 0},
    {"trs-80", 211, 0, 0, 0, 0},
    {"vic20", 2042, 4, 8, 8, 2},
    {"virtualboy", 429, 125, 123, 106, 125},
    {"wii", 70, 2, 2, 2, 2},
    {"windows", 1, 1, 1, 1, 1},
}};

/// ADR-0003's table: a kind, its share in percent of the 40,870 <game> entries as amended at the merge (checked to 0.1
/// point, the roadmap's criterion), and as written on 2026-10-15 (printed beside it, not checked).
struct AdrRow {
    std::string_view label;
    rv::catalog::MediaKind kind;
    double amended;
    double written;
};
constexpr std::array<AdrRow, 7> kAdr0003{{
    {"Screenshot", rv::catalog::MediaKind::Screenshot, 98.75, 98.5},
    {"Cover", rv::catalog::MediaKind::Cover, 90.90, 90.6},
    {"Marquee", rv::catalog::MediaKind::Marquee, 89.23, 89.3},
    {"Video", rv::catalog::MediaKind::Video, 82.19, 82.4},
    {"Physical media image", rv::catalog::MediaKind::PhysicalMedia, 81.48, 81.2},
    {"Back cover", rv::catalog::MediaKind::BackCover, 77.73, 77.8},
    {"Manual", rv::catalog::MediaKind::Manual, 56.55, 56.6},
}};
constexpr double kAdr0003Tolerance = 0.1;

/// The per-system figures ADR-0003 quotes as written, in whole percents of the system's <game> entries: Atari 800 47%
/// covers and 15% videos, Commodore 64 53% and 12%.
struct AdrSystemRow {
    std::string_view system;
    long covers;
    long videos;
};
constexpr std::array<AdrSystemRow, 2> kAdr0003Systems{{{"atari800", 47, 15}, {"c64", 53, 12}}};

void checkFigures(const nlohmann::json& counts, const Figures& expected, std::string_view population) {
    INFO(population);
    CHECK(counts["entries"] == expected.entries);
    CHECK(counts["unknown"] == 0);
    CHECK(counts["withAny"] == expected.withAny);
    for (std::size_t i = 0; i < rv::catalog::kMediaKindCount; ++i) {
        const std::string kind(rv::catalog::toString(rv::catalog::kMediaKinds[i]));
        INFO(kind);
        CHECK(counts["withKind"][kind] == expected.withKind[i]);
    }
}

void checkRow(const nlohmann::json& counts, const SystemRow& expected) {
    CHECK(counts["entries"] == expected.games);
    CHECK(counts["unknown"] == 0);
    CHECK(counts["withKind"]["covers"] == expected.covers);
    CHECK(counts["withKind"]["screenshots"] == expected.screenshots);
    CHECK(counts["withKind"]["videos"] == expected.videos);
    CHECK(counts["withKind"]["marquees"] == expected.marquees);
}

/// The report's system named `name`, or nullptr.
const nlohmann::json* systemOf(const nlohmann::json& report, std::string_view name) {
    for (const nlohmann::json& system : report["systems"]) {
        if (system["system"] == std::string(name)) {
            return &system;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("cli/media: the real library's media coverage against the oracle and ADR-0003",
          "[cli][media][.real-library]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }
    const LoggingReset loggingReset;
    const rv::test::TempDir dir("rv-cli-media-real");
    rv::core::PathEnvironment environment;
    environment.executableDir = dir.path();
    environment.sourceAssetDir = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    const auto run = [&](std::vector<std::string> arguments) {
        arguments.insert(arguments.begin(), {"--data-dir", rv::core::pathToUtf8(dir.file("data"))});
        Output out = Output::memory();
        Output err = Output::memory();
        const ExitCode code = rv::cli::run(arguments, out, err, environment);
        return std::make_tuple(code, out.text(), err.text());
    };
    const auto [created, createdOut, createdErr] =
        run({"library", "create", "--home", rv::core::pathToUtf8(home->parent_path()), "--install",
             rv::core::pathToUtf8(*install), "--rom-root", rv::core::pathToUtf8(*romRoot), "--media-root",
             rv::core::pathToUtf8(*mediaRoot)});
    INFO(createdErr);
    REQUIRE(created == ExitCode::Ok);

    const auto start = std::chrono::steady_clock::now();
    const auto [code, out, err] = run({"media", "--coverage", "--json"});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    INFO(err);
    CHECK(code == ExitCode::Ok);
    auto parsed = rv::core::parseJson(out, "stdout");
    REQUIRE(parsed.has_value());
    const nlohmann::json& report = *parsed;
    const nlohmann::json& total = report["total"];
    std::cout << "cli/media real library: coverage in " << seconds << " s (the library snapshot read, "
              << report["entries"] << " entries: every ROM folder listed, the gamelists, the cue sheets and playlists; "
              << report["listed"]["systems"] << " systems looked up in the media root, " << report["listed"]["files"]
              << " files; " << report["listed"]["caseChecks"] << " folders asked for their case rule, "
              << report["listed"]["fileChecks"] << " media files looked up on disk)\n";
    for (const char* population : {"gamelist-entries", "gamelist-entries-with-file", "scraped-games", "games"}) {
        std::cout << "  " << population << ": " << total[population].dump() << "\n";
    }

    CHECK(report["source"] == "rom-scan");
    CHECK(report["entries"] == kSnapshotEntries);
    checkFigures(total["gamelist-entries"], kGamelistEntries, "gamelist-entries");
    checkFigures(total["gamelist-entries-with-file"], kWithFile, "gamelist-entries-with-file");
    checkFigures(total["scraped-games"], kScrapedGames, "scraped-games");
    checkFigures(total["games"], kGames, "games");
    // Every system with a member: the 45 with a gamelist (trs-80 has no media folder), pc, pcfx and switch (neither).
    CHECK(report["listed"]["systems"] == 48);
    CHECK(report["listed"]["files"] == 364810);
    // No media folder has case-sensitive names: the listings answered every entry.
    CHECK(report["listed"]["fileChecks"] == 0);

    // Per system, every game of the snapshot.
    REQUIRE(report["systems"].size() == kGameRows.size());
    for (std::size_t i = 0; i < kGameRows.size(); ++i) {
        const SystemRow& row = kGameRows[i];
        const nlohmann::json& system = report["systems"][i];
        INFO(row.system);
        CHECK(system["system"] == std::string(row.system));
        const bool noFolder =
            row.system == "trs-80" || row.system == "pc" || row.system == "pcfx" || row.system == "switch";
        CHECK(system["mediaFolder"] == (noFolder ? "missing" : "listed"));
        checkRow(system["populations"]["games"], row);
    }
    // Per system, the survey's table of the scraped games, less the scraped tracks the snapshot groups.
    for (SystemRow row : kSurvey) {
        INFO(row.system);
        for (const SystemRow& tracks : kScrapedTrackRows) {
            if (tracks.system == row.system) {
                row.games -= tracks.games;
                row.covers -= tracks.covers;
                row.screenshots -= tracks.screenshots;
                row.videos -= tracks.videos;
                row.marquees -= tracks.marquees;
            }
        }
        const nlohmann::json* system = systemOf(report, row.system);
        REQUIRE(system != nullptr);
        checkRow((*system)["populations"]["scraped-games"], row);
    }
    for (const SystemRow& row : kGameRows) {
        const bool scraped = std::any_of(kSurvey.begin(), kSurvey.end(),
                                         [&](const SystemRow& survey) { return survey.system == row.system; });
        if (!scraped) {
            INFO(row.system);
            const nlohmann::json* system = systemOf(report, row.system);
            REQUIRE(system != nullptr);
            CHECK((*system)["populations"]["gamelist-entries"]["entries"] == 0);
        }
    }

    // ADR-0003's table, over the same 40,870 entries.
    std::cout << "  ADR-0003 over " << total["gamelist-entries"]["entries"] << " gamelist entries:\n";
    const double entries = total["gamelist-entries"]["entries"].get<double>();
    for (const AdrRow& row : kAdr0003) {
        const double measured =
            100.0 * total["gamelist-entries"]["withKind"][std::string(rv::catalog::toString(row.kind))].get<double>() /
            entries;
        std::cout << fmt::format("    {:<22} measured {:6.2f}%, amended {:6.2f}% ({:+.2f}), as written {:5.1f}% "
                                 "({:+.2f})\n",
                                 row.label, measured, row.amended, measured - row.amended, row.written,
                                 measured - row.written);
        INFO(row.label << ": measured " << measured);
        CHECK(std::abs(measured - row.amended) <= kAdr0003Tolerance);
    }
    for (const AdrSystemRow& row : kAdr0003Systems) {
        const auto system =
            std::find_if(report["systems"].begin(), report["systems"].end(),
                         [&](const nlohmann::json& item) { return item["system"] == std::string(row.system); });
        REQUIRE(system != report["systems"].end());
        const nlohmann::json& counts = (*system)["populations"]["gamelist-entries"];
        const double systemEntries = counts["entries"].get<double>();
        const double covers = 100.0 * counts["withKind"]["covers"].get<double>() / systemEntries;
        const double videos = 100.0 * counts["withKind"]["videos"].get<double>() / systemEntries;
        std::cout << fmt::format("    {:<22} covers {:5.2f}% (ADR-0003 {}%), videos {:5.2f}% (ADR-0003 {}%) of {} "
                                 "entries\n",
                                 row.system, covers, row.covers, videos, row.videos, counts["entries"].dump());
        INFO(row.system << ": covers " << covers << "%, videos " << videos << "%");
        CHECK(std::lround(covers) == row.covers);
        CHECK(std::lround(videos) == row.videos);
    }

    // Nothing but information about the media: the four .flv files.
    for (const auto& issue : report["issues"]) {
        CHECK(issue["severity"] != "error");
        if (std::string_view(issue["code"].get_ref<const std::string&>()).starts_with("media-")) {
            CHECK(issue["code"] == "media-ignored-extension");
            CHECK(issue["system"] == "snes");
        }
    }
}
