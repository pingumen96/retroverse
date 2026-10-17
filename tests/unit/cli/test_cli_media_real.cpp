// The media coverage of the real library through retroverse-cli media --coverage (I2 step 8, ADR-0008 §10, ADR-0003).
// Hidden ([.real-library]): ctest never runs it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots
// of the library before and after, and the folders in environment variables:
//   RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL  the ES-DE home and install folder (library create's --home is the home's
//                                            parent, as ES-DE's own --home)
//   RV_REAL_ROM_ROOT, RV_REAL_MEDIA_ROOT     the ROM and media roots, recorded as library.json's overrides
// The user data directory (library.json, the log) is a temporary folder outside the library.
//
// The expected figures come from an independent oracle: a Python count over the survey's read-only listings of
// 2026-10-16 (ElementTree over every gamelist, os.scandir listings of the ROM and media trees), which applies ES-DE's
// rule on its own: the media name is the ROM path with its last extension removed, names compared case-insensitively,
// the kind's extensions as ES-DE 3.4.1 reads them. Its per-system figures of the games with a gamelist entry are those
// of esde-data.md section 5, except snes' videos (1,462, not 1,466: the survey counted the four .flv files ES-DE never
// reads). ADR-0003's table (2026-10-15) gives its shares over the 40,870 <game> entries; three of its rows are
// reproduced within 0.1 point, four differ by 0.2 to 0.3 point (docs/i2/step-8.md: its computation was not recorded,
// and the oracle agrees with Retroverse entry for entry); the test checks both and prints the comparison.

#include <catch2/catch_test_macros.hpp>

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

// The oracle's figures over every <game> entry, those whose file exists, the games with a gamelist entry.
//                                     covers  back   3dbox  phys   shot   title  marq   mix    fanart manual video  cust
constexpr Figures kGamelistEntries{40870, {37151, 31768, 37012, 33302, 40359, 38842, 36468, 40357, 20002, 23110, 33592, 0},
                                   40561};
constexpr Figures kWithFile{39832, {36549, 31442, 36502, 33003, 39413, 38058, 36003, 39411, 19780, 22803, 33249, 0},
                            39563};
constexpr Figures kScrapedGames{38995, {35719, 30730, 35672, 32223, 38581, 37314, 35175, 38579, 19436, 22453, 32424, 0},
                                38729};

/// esde-data.md section 5 per system: the games with a gamelist entry, and those of them with a cover, a screenshot,
/// a video and a marquee (snes' videos without the four .flv files).
struct SystemRow {
    std::string_view system;
    std::uint64_t games, covers, screenshots, videos, marquees;
};
constexpr std::array<SystemRow, 45> kSurvey{{
    {"amiga", 2189, 1975, 2182, 1669, 1810},     {"amigacd32", 1, 1, 1, 1, 1},
    {"amstradcpc", 2397, 1936, 2397, 2013, 1889}, {"apple2", 662, 505, 653, 491, 513},
    {"atari2600", 569, 569, 569, 567, 569},       {"atari5200", 96, 96, 96, 96, 95},
    {"atari7800", 216, 216, 216, 216, 216},       {"atari800", 2523, 1249, 2460, 392, 858},
    {"atarijaguar", 339, 338, 338, 337, 338},     {"atarilynx", 360, 327, 360, 313, 360},
    {"atarist", 312, 304, 312, 209, 272},         {"c64", 1012, 535, 914, 118, 296},
    {"channelf", 27, 27, 27, 27, 27},             {"colecovision", 1010, 886, 1008, 720, 1010},
    {"dos", 20, 10, 10, 6, 10},                   {"dreamcast", 89, 87, 87, 87, 87},
    {"fds", 190, 190, 190, 190, 190},             {"gb", 4452, 4374, 4448, 4211, 4439},
    {"gba", 4512, 4443, 4504, 4224, 4504},        {"gbc", 2676, 2499, 2664, 2205, 2549},
    {"gc", 538, 536, 536, 536, 536},              {"intellivision", 156, 156, 155, 154, 155},
    {"mame", 76, 36, 36, 35, 36},                 {"mastersystem", 2237, 2237, 2229, 2221, 2237},
    {"megadrive", 1000, 997, 998, 996, 998},      {"n64", 2, 2, 2, 2, 2},
    {"nds", 115, 115, 114, 108, 114},             {"neogeo", 1, 0, 0, 0, 0},
    {"nes", 4065, 4042, 4060, 3356, 4060},        {"ngpc", 36, 28, 34, 35, 28},
    {"odyssey2", 83, 83, 83, 82, 83},             {"pcengine", 4, 1, 1, 1, 1},
    {"pokemini", 11, 11, 11, 11, 11},             {"ps2", 3061, 2993, 2973, 2979, 3001},
    {"ps3", 6, 6, 2, 0, 2},                       {"psp", 12, 12, 12, 12, 12},
    {"psx", 2233, 2228, 2228, 2223, 2227},        {"saturn", 8, 2, 2, 2, 2},
    {"segacd", 1, 1, 1, 1, 1},                    {"snes", 1560, 1535, 1535, 1462, 1507},
    {"trs-80", 1, 0, 0, 0, 0},                    {"vic20", 7, 3, 7, 7, 1},
    {"virtualboy", 126, 125, 123, 106, 125},      {"wii", 3, 2, 2, 2, 2},
    {"windows", 1, 1, 1, 1, 1},
}};

/// ADR-0003's table: a kind, its share in percent over the 40,870 entries, and how far the measured share may lie from
/// it (0.1 point, or the difference docs/i2/step-8.md explains).
struct AdrRow {
    std::string_view label;
    rv::catalog::MediaKind kind;
    double percent;
    double tolerance;
};
constexpr std::array<AdrRow, 7> kAdr0003{{
    {"Screenshot", rv::catalog::MediaKind::Screenshot, 98.5, 0.3},       // 98.75: +0.25, explained
    {"Cover", rv::catalog::MediaKind::Cover, 90.6, 0.35},                // 90.90: +0.30, explained
    {"Marquee", rv::catalog::MediaKind::Marquee, 89.3, 0.1},             // 89.23
    {"Video", rv::catalog::MediaKind::Video, 82.4, 0.25},                // 82.19: -0.21, explained
    {"Physical media image", rv::catalog::MediaKind::PhysicalMedia, 81.2, 0.3}, // 81.48: +0.28, explained
    {"Back cover", rv::catalog::MediaKind::BackCover, 77.8, 0.1},        // 77.73
    {"Manual", rv::catalog::MediaKind::Manual, 56.6, 0.1},               // 56.55
}};

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
    std::cout << "cli/media real library: coverage in " << seconds << " s (the gamelists read, the ROM folders listed "
              << "down the gamelists' folders, " << report["listed"]["systemFolders"] << " media folders listed, "
              << report["listed"]["files"] << " files)\n";
    for (const char* population : {"gamelist-entries", "gamelist-entries-with-file", "scraped-games"}) {
        std::cout << "  " << population << ": " << total[population].dump() << "\n";
    }

    checkFigures(total["gamelist-entries"], kGamelistEntries, "gamelist-entries");
    checkFigures(total["gamelist-entries-with-file"], kWithFile, "gamelist-entries-with-file");
    checkFigures(total["scraped-games"], kScrapedGames, "scraped-games");
    CHECK(report["listed"]["systemFolders"] == 45); // every system with a gamelist; trs-80 has no media folder
    CHECK(report["listed"]["files"] == 364810);

    // Per system, the survey's table.
    REQUIRE(report["systems"].size() == kSurvey.size());
    for (std::size_t i = 0; i < kSurvey.size(); ++i) {
        const SystemRow& row = kSurvey[i];
        const nlohmann::json& system = report["systems"][i];
        INFO(row.system);
        CHECK(system["system"] == std::string(row.system));
        const std::string folder = row.system == "trs-80" ? "missing" : "listed";
        CHECK(system["mediaFolder"] == folder);
        const nlohmann::json& games = system["populations"]["scraped-games"];
        CHECK(games["entries"] == row.games);
        CHECK(games["withKind"]["covers"] == row.covers);
        CHECK(games["withKind"]["screenshots"] == row.screenshots);
        CHECK(games["withKind"]["videos"] == row.videos);
        CHECK(games["withKind"]["marquees"] == row.marquees);
    }

    // ADR-0003's table, over the same 40,870 entries.
    std::cout << "  ADR-0003 over " << total["gamelist-entries"]["entries"] << " gamelist entries:\n";
    const double entries = total["gamelist-entries"]["entries"].get<double>();
    for (const AdrRow& row : kAdr0003) {
        const double measured =
            100.0 * total["gamelist-entries"]["withKind"][std::string(rv::catalog::toString(row.kind))].get<double>() /
            entries;
        std::cout << fmt::format("    {:<22} ADR-0003 {:5.1f}%, measured {:6.2f}% ({:+.2f})\n", row.label, row.percent,
                                 measured, measured - row.percent);
        INFO(row.label << ": measured " << measured);
        CHECK(std::abs(measured - row.percent) <= row.tolerance);
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
