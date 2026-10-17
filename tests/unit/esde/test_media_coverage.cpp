// The media coverage of the standard fixture (I2 step 8): its 23 entries as its README states them, their media found
// by esde::EsdeMediaResolver in the fixture's media folder, counted by catalog::measureCoverage over the four
// populations; then the media-roots fixture's empty and missing media roots.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/coverage.hpp>
#include <rv/catalog/entry.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/esde/media_resolver.hpp>

#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::catalog::Availability;
using rv::catalog::CatalogEntry;
using rv::catalog::CoveragePopulation;
using rv::catalog::EntryState;
using rv::catalog::MediaKind;

namespace {

rv::catalog::SystemName systemNamed(std::string_view name) {
    auto parsed = rv::catalog::SystemName::parse(name);
    REQUIRE(parsed.has_value());
    return *parsed;
}

rv::test::Fixture materialize(std::string_view name) {
    auto materialized = rv::test::Fixture::materialize(name);
    INFO((materialized ? "materialized" : materialized.error().toString()));
    REQUIRE(materialized.has_value());
    return std::move(*materialized);
}

/// An entry of the snapshot step 7 makes of the standard fixture: its state, whether a gamelist names it, and whether
/// its file is a folder; the file is seen for every state but MissingFile and OutsideSystemFolder.
CatalogEntry entryOf(std::string_view system, std::string_view relativePath, EntryState state, bool gamelist,
                     bool directory = false) {
    CatalogEntry entry;
    entry.system = systemNamed(system);
    entry.state = state;
    if (!relativePath.empty()) {
        auto key = rv::catalog::makeLookupKey(relativePath);
        REQUIRE(key.has_value());
        entry.key = *key;
    }
    if (gamelist) {
        entry.gamelistPath = relativePath.empty() ? std::string("./../snes/Alpha Quest (USA).sfc")
                                                  : "./" + std::string(relativePath);
    }
    if (state == EntryState::MissingFile) {
        entry.availability = Availability::Missing;
    } else if (state != EntryState::OutsideSystemFolder) {
        entry.file = rv::catalog::FileFacts{directory, 0, 0, false};
    }
    return entry;
}

/// The standard fixture's 23 entries (its README's "ROM tree and gamelists").
rv::catalog::LibrarySnapshot standardSnapshot() {
    rv::catalog::LibrarySnapshot snapshot;
    snapshot.entries = {
        entryOf("gb", "Tiny Tennis (Europe).gb", EntryState::Game, true),
        entryOf("gb", "Collection A", EntryState::Folder, true, true),
        entryOf("gb", "Collection A/Pocket Puzzle (World).gb", EntryState::Game, true),
        entryOf("gb", "Collection B/Pocket Puzzle (World).gb", EntryState::Game, false),
        entryOf("gb", "Café Racer (Europe).gb", EntryState::Game, true),
        entryOf("gb", ".Dot Hero (World).gb", EntryState::Game, false),
        entryOf("gb", "Hidden Gem (World).gb", EntryState::Game, false),
        entryOf("gb", "LOUD CART (USA).GB", EntryState::Game, false),
        entryOf("gb", "Collection A/Deep/Bottom Dweller (Japan).gbc", EntryState::Game, false),
        entryOf("gb", "Gone Fishing (USA).gb", EntryState::MissingFile, true),
        entryOf("gb", "", EntryState::OutsideSystemFolder, true),
        entryOf("snes", "Alpha Quest (USA).sfc", EntryState::Game, true),
        entryOf("snes", "Beta Run (Europe).SMC", EntryState::Game, false),
        entryOf("snes", "Mixed Case (USA).Sfc", EntryState::NotAGame, true),
        entryOf("snes", "Hacks", EntryState::Folder, true, true),
        entryOf("snes", "Hacks/Gamma Hack (Hack).sfc", EntryState::Game, true),
        entryOf("snes", "Delta Drift (USA).sfc", EntryState::Game, true),
        entryOf("snes", "Sports Vol. 2", EntryState::Folder, true, true),
        entryOf("snes", "Sports Vol. 2/Ice Lanes (USA).sfc", EntryState::Game, false),
        entryOf("psx", "Twin Tracks (Europe).cue", EntryState::Game, true),
        entryOf("psx", "Twin Tracks (Europe) (Track 1).bin", EntryState::DiscMember, false),
        entryOf("psx", "Twin Tracks (Europe) (Track 2).bin", EntryState::DiscMember, false),
        entryOf("psx", "Solo Disc (USA).chd", EntryState::Game, true),
    };
    snapshot.entries[13].notAGameReason = rv::catalog::NotAGameReason::ExtensionNotConfigured;
    std::sort(snapshot.entries.begin(), snapshot.entries.end(), rv::catalog::entryOrder);
    return snapshot;
}

/// Counts of one population: the members, then per kind in the order of kMediaKinds.
struct Counts {
    std::uint64_t entries;
    std::array<std::uint64_t, rv::catalog::kMediaKindCount> withKind;
    std::uint64_t withAny;
};

void check(const rv::catalog::CoverageCounts& counts, const Counts& expected) {
    CHECK(counts.entries == expected.entries);
    CHECK(counts.unknown == 0);
    CHECK(counts.withKind == expected.withKind);
    CHECK(counts.withAny == expected.withAny);
}

} // namespace

TEST_CASE("esde/media coverage: the standard fixture over the four populations", "[esde][media][coverage]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    rv::esde::EsdeMediaResolver resolver(fixture.path("media"));
    const auto coverage = rv::catalog::measureCoverage(standardSnapshot(), resolver);
    REQUIRE(coverage.has_value());
    REQUIRE(coverage->systems.size() == 3);
    CHECK(coverage->issues.empty());

    //                                   cover back 3d phys shot title marq mix fan man video custom
    const auto gb = *coverage->find(systemNamed("gb"));
    // Tiny Tennis (cover, screenshot, video), Pocket Puzzle A (cover), Café Racer (video), Gone Fishing (cover), the
    // entry outside gb's folder (none).
    check(gb[CoveragePopulation::GamelistEntries], {5, {3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 2, 0}, 4});
    check(gb[CoveragePopulation::GamelistEntriesWithFile], {3, {2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 2, 0}, 3});
    check(gb[CoveragePopulation::ScrapedGames], {3, {2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 2, 0}, 3});
    // The eight games: LOUD CART's mkv adds a video; Pocket Puzzle B, .Dot Hero, Hidden Gem, Bottom Dweller have none.
    check(gb[CoveragePopulation::Games], {8, {2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 3, 0}, 4});

    const auto snes = *coverage->find(systemNamed("snes"));
    const std::array<std::uint64_t, rv::catalog::kMediaKindCount> alphaOnly{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    // Alpha Quest has all 12 kinds; Mixed Case (no game, its file there), Gamma Hack and Delta Drift none; the folders
    // Hacks and Sports Vol. 2 have covers but are in no population; Beta Run's flv is no video.
    check(snes[CoveragePopulation::GamelistEntries], {4, alphaOnly, 1});
    check(snes[CoveragePopulation::GamelistEntriesWithFile], {4, alphaOnly, 1});
    check(snes[CoveragePopulation::ScrapedGames], {3, alphaOnly, 1});
    check(snes[CoveragePopulation::Games], {5, alphaOnly, 1});

    const auto psx = *coverage->find(systemNamed("psx"));
    // Twin Tracks (cover, video) and Solo Disc; the tracks are disc members, in no population.
    for (const CoveragePopulation population : rv::catalog::kCoveragePopulations) {
        check(psx[population], {2, {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0}, 1});
    }

    check(coverage->total[CoveragePopulation::GamelistEntries], {11, {5, 1, 1, 1, 2, 1, 1, 1, 1, 1, 4, 1}, 6});
    check(coverage->total[CoveragePopulation::Games], {15, {4, 1, 1, 1, 2, 1, 1, 1, 1, 1, 5, 1}, 6});
    CHECK(coverage->total[CoveragePopulation::Games].share(MediaKind::Video) == 5.0 / 15.0);
    CHECK(coverage->total[CoveragePopulation::GamelistEntries].share(MediaKind::Cover) == 5.0 / 11.0);
}

TEST_CASE("esde/media coverage: an empty media root covers nothing; a missing one leaves every share unknown",
          "[esde][media][coverage]") {
    const rv::test::Fixture fixture = materialize("esde/media-roots");
    rv::catalog::LibrarySnapshot snapshot;
    snapshot.entries = {
        entryOf("gb", "Pocket Pal (World).gb", EntryState::Game, false),
        entryOf("nes", "Lost Lagoon (Europe).nes", EntryState::MissingFile, true),
        entryOf("nes", "Rocket Ranger (USA).nes", EntryState::Game, true),
        entryOf("nes", "Unscraped Hop (World).nes", EntryState::Game, false),
    };

    rv::esde::EsdeMediaResolver empty(fixture.path("media"));
    const auto none = rv::catalog::measureCoverage(snapshot, empty);
    REQUIRE(none.has_value());
    CHECK(none->issues.empty());
    const auto nes = *none->find(systemNamed("nes"));
    check(nes[CoveragePopulation::GamelistEntries], {2, {}, 0});
    check(nes[CoveragePopulation::GamelistEntriesWithFile], {1, {}, 0});
    check(nes[CoveragePopulation::Games], {2, {}, 0});
    CHECK(nes[CoveragePopulation::Games].share(MediaKind::Cover) == 0.0);
    check(none->total[CoveragePopulation::Games], {3, {}, 0});

    rv::esde::EsdeMediaResolver missing(fixture.path("unplugged/media"));
    const auto unknown = rv::catalog::measureCoverage(snapshot, missing);
    REQUIRE(unknown.has_value());
    const auto& games = unknown->total[CoveragePopulation::Games];
    CHECK(games.entries == 3);
    CHECK(games.unknown == 3);
    CHECK_FALSE(games.share(MediaKind::Cover).has_value());
    REQUIRE(unknown->issues.size() == 2); // gb and nes
    CHECK(unknown->issues[0].system == "gb");
    CHECK(unknown->issues[0].code == rv::catalog::kCoverageIssueMediaUnknown);
    CHECK(unknown->issues[1].reason.find("the media root does not exist") != std::string::npos);
}
