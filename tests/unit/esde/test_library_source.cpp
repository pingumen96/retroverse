// EsdeLibrarySource (I2 step 7): the snapshot of the standard fixture with every count and case of its README, the
// settings variants, read-only towards the library, the disc group cache, cancellation and the system filter.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/library_source.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/esde/library_source.hpp>

#include "snapshot_test_support.hpp"
#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::catalog::Availability;
using rv::catalog::CatalogEntry;
using rv::catalog::EntryState;
using rv::catalog::LibrarySnapshot;
using rv::catalog::NotAGameReason;
using rv::test::esde::countsOf;
using rv::test::esde::entryAt;
using rv::test::esde::loadSnapshot;
using rv::test::esde::materialize;
using rv::test::esde::profileOf;
using rv::test::esde::StateCounts;

TEST_CASE("esde/library_source: the standard fixture as its README counts it", "[esde][library_source]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(0), {}});
    const LibrarySnapshot snapshot = loadSnapshot(source, profileOf(fixture));

    // Every system of the configuration, by name, with its gamelist.
    REQUIRE(snapshot.systems.size() == 3);
    CHECK(snapshot.systems[0].name.str() == "gb");
    CHECK(snapshot.systems[1].name.str() == "psx");
    CHECK(snapshot.systems[2].name.str() == "snes");
    for (const auto& system : snapshot.systems) {
        CHECK(system.availability == Availability::Available);
        CHECK(system.gamelist == rv::catalog::GamelistState::Read);
    }
    CHECK(snapshot.systems[2].alternativeEmulator == "Fake Emulator (Hidden Console)");
    CHECK(snapshot.systems[0].alternativeEmulator.empty());

    CHECK(countsOf(snapshot, "gb") == StateCounts{11, 8, 1, 0, 0, 1, 1});
    CHECK(countsOf(snapshot, "snes") == StateCounts{8, 5, 0, 1, 0, 2, 0});
    CHECK(countsOf(snapshot, "psx") == StateCounts{4, 2, 0, 0, 2, 0, 0});
    CHECK(snapshot.entries.size() == 23);
    CHECK(snapshot.unavailableFolders.empty());
    CHECK(std::is_sorted(snapshot.entries.begin(), snapshot.entries.end(), rv::catalog::entryOrder));

    // Games without a gamelist entry, titled from their file stem.
    std::vector<std::string> unscraped;
    for (const CatalogEntry& entry : snapshot.entries) {
        if (entry.state == EntryState::Game && !entry.fromGamelist()) {
            unscraped.push_back(entry.title);
        }
    }
    std::sort(unscraped.begin(), unscraped.end());
    CHECK(unscraped == std::vector<std::string>{".Dot Hero (World)", "Beta Run (Europe)", "Bottom Dweller (Japan)",
                                                "Hidden Gem (World)", "Ice Lanes (USA)", "LOUD CART (USA)",
                                                "Pocket Puzzle (World)"});

    // gb.
    const CatalogEntry& tennis = entryAt(snapshot, "gb", "Tiny Tennis (Europe).gb");
    CHECK(tennis.state == EntryState::Game);
    CHECK(tennis.title == "Tiny Tennis");
    CHECK(tennis.gamelistPath == "./Tiny Tennis (Europe).gb");
    CHECK(tennis.flags.favorite);
    CHECK(tennis.flags.playCount == 3);
    REQUIRE(tennis.file.has_value());
    CHECK(tennis.file->size == 32768);
    CHECK_FALSE(tennis.file->directory);
    CHECK(tennis.availability == Availability::Available);
    const CatalogEntry& collection = entryAt(snapshot, "gb", "Collection A");
    CHECK(collection.state == EntryState::Folder);
    REQUIRE(collection.file.has_value());
    CHECK(collection.file->directory);
    CHECK(entryAt(snapshot, "gb", "Collection A/Pocket Puzzle (World).gb").title == "Pocket Puzzle");
    const CatalogEntry& namesake = entryAt(snapshot, "gb", "Collection B/Pocket Puzzle (World).gb");
    CHECK(namesake.state == EntryState::Game);
    CHECK_FALSE(namesake.fromGamelist());
    CHECK(entryAt(snapshot, "gb", "Café Racer (Europe).gb").altEmulator == "Fake Emulator (Game Folder)");
    const CatalogEntry& dot = entryAt(snapshot, "gb", ".Dot Hero (World).gb");
    CHECK(dot.state == EntryState::Game);
    REQUIRE(dot.file.has_value());
    CHECK(dot.file->hidden);
    const CatalogEntry& hiddenGem = entryAt(snapshot, "gb", "Hidden Gem (World).gb");
    CHECK(hiddenGem.state == EntryState::Game);
    REQUIRE(hiddenGem.file.has_value());
    CHECK(hiddenGem.file->hidden);
    CHECK(entryAt(snapshot, "gb", "LOUD CART (USA).GB").state == EntryState::Game);
    CHECK(entryAt(snapshot, "gb", "Collection A/Deep/Bottom Dweller (Japan).gbc").file->size == 196608);
    const CatalogEntry& gone = entryAt(snapshot, "gb", "Gone Fishing (USA).gb");
    CHECK(gone.state == EntryState::MissingFile);
    CHECK(gone.availability == Availability::Missing);
    CHECK_FALSE(gone.file.has_value());
    CHECK(gone.title == "Gone Fishing");
    const CatalogEntry* outside = nullptr;
    for (const CatalogEntry& entry : snapshot.entries) {
        if (entry.state == EntryState::OutsideSystemFolder) {
            outside = &entry;
        }
    }
    REQUIRE(outside != nullptr);
    CHECK(outside->system.str() == "gb");
    CHECK(outside->key.empty());
    CHECK(outside->gamelistPath == "./../snes/Alpha Quest (USA).sfc");
    CHECK(outside->title == "Wrong Way");

    // snes.
    const CatalogEntry& mixed = entryAt(snapshot, "snes", "Mixed Case (USA).Sfc");
    CHECK(mixed.state == EntryState::NotAGame);
    CHECK(mixed.notAGameReason == NotAGameReason::ExtensionNotConfigured);
    CHECK(mixed.fromGamelist());
    const CatalogEntry& delta = entryAt(snapshot, "snes", "Delta Drift (USA).sfc");
    CHECK(delta.state == EntryState::Game);
    CHECK(delta.gamelistPath == "./delta drift (usa).sfc");
    CHECK(entryAt(snapshot, "snes", "Hacks").state == EntryState::Folder);
    CHECK(entryAt(snapshot, "snes", "Sports Vol. 2").state == EntryState::Folder);
    CHECK(entryAt(snapshot, "snes", "Sports Vol. 2").title == "Sports, Volume 2");
    CHECK(entryAt(snapshot, "snes", "Sports Vol. 2/Ice Lanes (USA).sfc").state == EntryState::Game);

    // psx: the cue sheet and its tracks.
    const CatalogEntry& twin = entryAt(snapshot, "psx", "Twin Tracks (Europe).cue");
    CHECK(twin.state == EntryState::Game);
    for (const char* track : {"Twin Tracks (Europe) (Track 1).bin", "Twin Tracks (Europe) (Track 2).bin"}) {
        const CatalogEntry& member = entryAt(snapshot, "psx", track);
        CHECK(member.state == EntryState::DiscMember);
        CHECK(member.discHead.relativePath() == "Twin Tracks (Europe).cue");
    }
    const CatalogEntry& solo = entryAt(snapshot, "psx", "Solo Disc (USA).chd");
    CHECK(solo.state == EntryState::Game);
    CHECK(solo.flags.hidden);
    REQUIRE(snapshot.discGroups.size() == 1);
    const rv::catalog::DiscGroup& group = snapshot.discGroups.front();
    CHECK(group.system.str() == "psx");
    CHECK(group.kind == rv::catalog::DiscGroupKind::Cue);
    CHECK(group.head.relativePath() == "Twin Tracks (Europe).cue");
    REQUIRE(group.members.size() == 2);
    CHECK(group.members[0].relativePath() == "Twin Tracks (Europe) (Track 1).bin");
    CHECK(group.members[1].relativePath() == "Twin Tracks (Europe) (Track 2).bin");

    // The files that are no entry.
    for (const auto& [system, path] : std::vector<std::pair<std::string, std::string>>{
             {"gb", "Notes.txt"},
             {"gb", "gamelist.xml"},
             {"snes", "manual.pdf"},
             {"psx", "Solo Disc (USA).commands"},
             {"psx", "Loud Disc (Japan).CUE"}}) {
        INFO(system << " " << path);
        CHECK(rv::test::esde::findEntry(snapshot, system, path) == nullptr);
    }

    // The legacy gamelist inside gb's folder was never read: Tiny Tennis keeps its own name.
    for (const CatalogEntry& entry : snapshot.entries) {
        CHECK(entry.title != "Legacy Tennis");
    }
    // The configuration's quirk (psx's "CUE") and the gamelist's issue (the entry outside the folder) are the
    // snapshot's; nothing else went wrong.
    std::vector<std::string> codes;
    for (const auto& issue : snapshot.issues) {
        codes.push_back(issue.code);
    }
    CHECK(codes == std::vector<std::string>{"extension-without-dot", "gamelist-path-outside-folder"});

    const rv::esde::EsdeLoadStatistics& statistics = source.lastStatistics();
    CHECK(statistics.systemsConfigured == 3);
    CHECK(statistics.systemsScanned == 3);
    CHECK(statistics.gamelistsRead == 3);
    CHECK(statistics.sheetsRead == 1);
    CHECK(statistics.sheetsFromCache == 0);
}

TEST_CASE("esde/library_source: the settings variants of the standard fixture", "[esde][library_source]") {
    struct Variant {
        bool showHiddenFiles;
        bool showHiddenGames;
        std::size_t gbGames;
        std::size_t psxGames;
        std::vector<std::string> excluded; // system/path, by ShowHiddenFiles or ShowHiddenGames
        std::size_t esdeShows;
    };
    const std::vector<Variant> variants{
        {true, true, 8, 2, {}, 18},
        {false, true, 6, 2, {"gb/.Dot Hero (World).gb", "gb/Hidden Gem (World).gb"}, 16},
        {true, false, 8, 1, {"psx/Solo Disc (USA).chd"}, 17},
        {false, false, 6, 1, {"gb/.Dot Hero (World).gb", "gb/Hidden Gem (World).gb", "psx/Solo Disc (USA).chd"}, 15},
    };
    for (const Variant& variant : variants) {
        INFO("ShowHiddenFiles " << variant.showHiddenFiles << ", ShowHiddenGames " << variant.showHiddenGames);
        const rv::test::Fixture fixture = materialize("esde/standard");
        const auto set = [&](std::string_view key, bool value) {
            if (!value) {
                REQUIRE(fixture.replaceText("ES-DE/settings/es_settings.xml",
                                            "name=\"" + std::string(key) + "\" value=\"true\"",
                                            "name=\"" + std::string(key) + "\" value=\"false\""));
            }
        };
        set("ShowHiddenFiles", variant.showHiddenFiles);
        set("ShowHiddenGames", variant.showHiddenGames);
        rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(0), {}});
        const LibrarySnapshot snapshot = loadSnapshot(source, profileOf(fixture));
        CHECK(snapshot.entries.size() == 23);
        CHECK(countsOf(snapshot, "gb").games == variant.gbGames);
        CHECK(countsOf(snapshot, "snes").games == 5);
        CHECK(countsOf(snapshot, "psx").games == variant.psxGames);
        std::vector<std::string> excluded;
        std::size_t esdeShows = 0;
        std::size_t unscraped = 0;
        for (const CatalogEntry& entry : snapshot.entries) {
            if (entry.state == EntryState::NotAGame && entry.notAGameReason == NotAGameReason::ExcludedBySetting) {
                excluded.push_back(entry.system.str() + "/" + entry.key.relativePath());
                const bool file = entry.key.relativePath().ends_with(".chd");
                CHECK(entry.excludingSetting == (file ? "ShowHiddenGames" : "ShowHiddenFiles"));
            }
            esdeShows += entry.state == EntryState::Game || entry.state == EntryState::DiscMember ? 1 : 0;
            unscraped += entry.state == EntryState::Game && !entry.fromGamelist() ? 1 : 0;
        }
        // ES-DE also shows a second Delta Drift for its gamelist entry spelled in another case.
        CHECK(esdeShows + 1 == variant.esdeShows);
        CHECK(unscraped == (variant.showHiddenFiles ? 7u : 5u));
        CHECK(excluded == variant.excluded);
    }
}

TEST_CASE("esde/library_source: reading the library writes nothing under it", "[esde][library_source]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    const auto before = rv::test::esde::treeState(fixture.root());
    rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(0), {}});
    (void)loadSnapshot(source, profileOf(fixture));
    CHECK(rv::test::esde::treeState(fixture.root()) == before);
}

TEST_CASE("esde/library_source: only the systems asked for, and cancellation", "[esde][library_source]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    {
        rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(0), {"psx"}});
        const LibrarySnapshot snapshot = loadSnapshot(source, profileOf(fixture));
        REQUIRE(snapshot.systems.size() == 1);
        CHECK(snapshot.systems[0].name.str() == "psx");
        CHECK(snapshot.entries.size() == 4);
    }
    {
        rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(0), {"nosuchsystem"}});
        const auto snapshot = source.load(profileOf(fixture), {});
        REQUIRE_FALSE(snapshot.has_value());
        CHECK(snapshot.error().code == rv::ErrorCode::InvalidArgument);
    }
    {
        const std::atomic<bool> cancel{true};
        rv::catalog::LibraryLoadOptions options;
        options.cancel = &cancel;
        rv::esde::EsdeLibrarySource source;
        const auto snapshot = source.load(profileOf(fixture), options);
        REQUIRE_FALSE(snapshot.has_value());
        CHECK(snapshot.error().code == rv::ErrorCode::Cancelled);
    }
    {
        rv::catalog::LibraryProfile profile = profileOf(fixture);
        profile.esdeHome = fixture.path("No ES-DE here");
        rv::esde::EsdeLibrarySource source;
        const auto snapshot = source.load(profile, {});
        REQUIRE_FALSE(snapshot.has_value());
        CHECK(snapshot.error().code == rv::ErrorCode::NotFound);
    }
}

TEST_CASE("esde/library_source: disc groups come from the cache while their head keeps its size and time",
          "[esde][library_source]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(0), {}});
    const LibrarySnapshot first = loadSnapshot(source, profileOf(fixture));
    REQUIRE(first.discGroups.size() == 1);
    const CatalogEntry& head = entryAt(first, "psx", "Twin Tracks (Europe).cue");
    REQUIRE(head.file.has_value());

    // A cache that says something else of the head: used while the size and time match, never read.
    rv::catalog::CachedDiscGroup cached{first.discGroups.front(), head.file->size, head.file->lastWriteTime};
    cached.group.members.pop_back(); // only Track 1, as an earlier import read it
    std::vector<rv::catalog::CachedDiscGroup> cache{cached};
    rv::catalog::LibraryLoadOptions options;
    options.discGroups = cache;
    const LibrarySnapshot fromCache = loadSnapshot(source, profileOf(fixture), options);
    REQUIRE(fromCache.discGroups.size() == 1);
    CHECK(fromCache.discGroups.front().members.size() == 1);
    CHECK(entryAt(fromCache, "psx", "Twin Tracks (Europe) (Track 2).bin").state == EntryState::Game);
    CHECK(source.lastStatistics().sheetsRead == 0);
    CHECK(source.lastStatistics().sheetsFromCache == 1);

    // Another size: read again.
    cache.front().headSize += 1;
    const LibrarySnapshot reread = loadSnapshot(source, profileOf(fixture), options);
    CHECK(reread.discGroups.front().members.size() == 2);
    CHECK(source.lastStatistics().sheetsRead == 1);
}
