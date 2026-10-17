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
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/library_source.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/core/tree_walk.hpp>
#include <rv/esde/library_source.hpp>

#include "snapshot_test_support.hpp"
#include "support/file_system.hpp"
#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::catalog::Availability;
using rv::catalog::CatalogEntry;
using rv::catalog::EntryState;
using rv::catalog::LibrarySnapshot;
using rv::catalog::NotAGameReason;
using rv::test::esde::countsOf;
using rv::test::esde::entryAt;
using rv::test::esde::issuesOf;
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
    for (const auto& [system, path] :
         std::vector<std::pair<std::string, std::string>>{{"gb", "Notes.txt"},
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
    const rv::catalog::DiscGroup twin = first.discGroups.front();
    CHECK(twin.references == twin.members); // the sheet names its two tracks, both there
    const CatalogEntry& head = entryAt(first, "psx", "Twin Tracks (Europe).cue");
    REQUIRE(head.file.has_value());
    // A group read from its sheet carries the head's facts as listed.
    CHECK(twin.headSize == head.file->size);
    CHECK(twin.headLastWriteTime == head.file->lastWriteTime);

    // A cache that says something else of the head: used while the size and time match, never read.
    rv::catalog::DiscGroup cached = twin;
    cached.references.pop_back(); // as if the sheet named Track 1 alone
    cached.members.pop_back();
    std::vector<rv::catalog::DiscGroup> cache{cached};
    rv::catalog::LibraryLoadOptions options;
    options.discGroups = cache;
    const LibrarySnapshot fromCache = loadSnapshot(source, profileOf(fixture), options);
    REQUIRE(fromCache.discGroups.size() == 1);
    CHECK(fromCache.discGroups.front().members.size() == 1);
    CHECK(entryAt(fromCache, "psx", "Twin Tracks (Europe) (Track 2).bin").state == EntryState::Game);
    CHECK(source.lastStatistics().sheetsRead == 0);
    CHECK(source.lastStatistics().sheetsFromCache == 1);

    // The cached references, not the cached members, give the members: a group an earlier import stored with Track 1
    // alone (Track 2 was not there then) takes Track 2 back, still without reading the sheet.
    cache.front() = twin;
    cache.front().members.pop_back();
    const LibrarySnapshot rejoined = loadSnapshot(source, profileOf(fixture), options);
    REQUIRE(rejoined.discGroups.size() == 1);
    CHECK(rejoined.discGroups.front() == twin);
    CHECK(entryAt(rejoined, "psx", "Twin Tracks (Europe) (Track 2).bin").state == EntryState::DiscMember);
    CHECK(source.lastStatistics().sheetsRead == 0);

    // Another size: read again, and the group carries the facts as listed now.
    cache.front().headSize += 1;
    const LibrarySnapshot reread = loadSnapshot(source, profileOf(fixture), options);
    CHECK(reread.discGroups.front().members.size() == 2);
    CHECK(reread.discGroups.front().headSize == head.file->size);
    CHECK(source.lastStatistics().sheetsRead == 1);

    // Told to read every sheet (db rebuild-index), a cache whose facts match is read anyway.
    cache.front() = cached;
    options.readEverySheet = true;
    const LibrarySnapshot fresh = loadSnapshot(source, profileOf(fixture), options);
    CHECK(fresh.discGroups.front() == twin);
    CHECK(source.lastStatistics().sheetsRead == 1);
    CHECK(source.lastStatistics().sheetsFromCache == 0);
}

TEST_CASE("esde/library_source: a changed sheet that cannot be read keeps its cached group with its cached facts",
          "[esde][library_source]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    const fs::path sheet = fixture.path("roms/psx/Twin Tracks (Europe).cue");
    rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(0), {}});
    const LibrarySnapshot first = loadSnapshot(source, profileOf(fixture));
    REQUIRE(first.discGroups.size() == 1);
    const rv::catalog::DiscGroup twin = first.discGroups.front();
    const std::vector<rv::catalog::DiscGroup> cache{twin};
    rv::catalog::LibraryLoadOptions options;
    options.discGroups = cache;

    // The sheet now names Track 1 alone (another size), and cannot be read: the cached references stand, with the
    // facts of the sheet they were read from, never those of the sheet listed now.
    const auto text = rv::core::readFileShared(sheet, 1 << 20);
    REQUIRE(text.has_value());
    const std::string original(text->begin(), text->end());
    const std::size_t second = original.find("FILE", original.find("FILE") + 4);
    REQUIRE(second != std::string::npos);
    REQUIRE(rv::test::writeFileAnyLength(sheet, original.substr(0, second)));
    {
        auto denied = rv::test::DataReadDenied::apply(sheet);
        REQUIRE(denied.has_value());
        const LibrarySnapshot unreadable = loadSnapshot(source, profileOf(fixture), options);
        CHECK(source.lastStatistics().sheetsUnreadable == 1);
        REQUIRE(unreadable.discGroups.size() == 1);
        CHECK(unreadable.discGroups.front() == twin); // its references and facts as cached
        CHECK(entryAt(unreadable, "psx", "Twin Tracks (Europe).cue").file->size != twin.headSize);
    }
    // Readable again, with the same cache (what an import stored from the load above): read, the group follows.
    const LibrarySnapshot readable = loadSnapshot(source, profileOf(fixture), options);
    CHECK(source.lastStatistics().sheetsRead == 1);
    REQUIRE(readable.discGroups.size() == 1);
    CHECK(readable.discGroups.front().members.size() == 1);
    CHECK(entryAt(readable, "psx", "Twin Tracks (Europe) (Track 2).bin").state == EntryState::Game);
}

TEST_CASE("esde/library_source: a track that comes back joins its cached group", "[esde][library_source]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    const fs::path track = fixture.path("roms/psx/Twin Tracks (Europe) (Track 1).bin");
    const auto bytes = rv::core::readFileShared(track, 1 << 20);
    REQUIRE(bytes.has_value());
    REQUIRE(fs::remove(track));
    rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(0), {}});

    // Track 1 is gone: the group holds Track 2 alone, the sheet's references both, and the missing one is reported.
    const LibrarySnapshot first = loadSnapshot(source, profileOf(fixture));
    REQUIRE(first.discGroups.size() == 1);
    const rv::catalog::DiscGroup& group = first.discGroups.front();
    CHECK(group.members.size() == 1);
    REQUIRE(group.references.size() == 2);
    CHECK(group.references[0].relativePath() == "Twin Tracks (Europe) (Track 1).bin");
    CHECK(issuesOf(first.issues, "disc-reference-missing").size() == 1);
    const CatalogEntry& head = entryAt(first, "psx", "Twin Tracks (Europe).cue");
    REQUIRE(group.headSize == head.file->size);
    std::vector<rv::catalog::DiscGroup> cache{group};
    rv::catalog::LibraryLoadOptions options;
    options.discGroups = cache;

    // Still gone, from the cache: the same group, the same issue (it stands while the file is not there).
    const LibrarySnapshot stillGone = loadSnapshot(source, profileOf(fixture), options);
    CHECK(stillGone.discGroups == first.discGroups);
    CHECK(issuesOf(stillGone.issues, "disc-reference-missing").size() == 1);
    CHECK(source.lastStatistics().sheetsRead == 0);

    // Back, the sheet untouched: a member again without the sheet being read.
    REQUIRE(rv::test::writeFileAnyLength(track, *bytes));
    const LibrarySnapshot back = loadSnapshot(source, profileOf(fixture), options);
    CHECK(source.lastStatistics().sheetsRead == 0);
    CHECK(source.lastStatistics().sheetsFromCache == 1);
    REQUIRE(back.discGroups.size() == 1);
    CHECK(back.discGroups.front().members.size() == 2);
    CHECK(entryAt(back, "psx", "Twin Tracks (Europe) (Track 1).bin").state == EntryState::DiscMember);
    CHECK(issuesOf(back.issues, "disc-reference-missing").empty());
}

TEST_CASE("esde/library_source: a gamelist that fails is read again, then given up on", "[esde][library_source]") {
    const rv::test::Fixture fixture = materialize("esde/standard");
    const fs::path gamelist = fixture.path("ES-DE/gamelists/gb/gamelist.xml");
    SECTION("cut short: failed after the retry, its files listed without metadata") {
        const auto text = rv::core::readFileShared(gamelist, 1 << 20);
        REQUIRE(text.has_value());
        REQUIRE(rv::core::writeTextFileAtomic(gamelist, text->substr(0, text->size() / 2)));
        rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(0), {}});
        const LibrarySnapshot snapshot = loadSnapshot(source, profileOf(fixture));
        const rv::catalog::SystemInfo* gb = snapshot.findSystem(rv::catalog::SystemName::parse("gb").value());
        REQUIRE(gb != nullptr);
        CHECK(gb->gamelist == rv::catalog::GamelistState::Failed);
        CHECK(source.lastStatistics().gamelistRetries == 1);
        // The files are there, without metadata; nothing is taken as missing, nothing is outside.
        CHECK(countsOf(snapshot, "gb") == StateCounts{8, 8, 0, 0, 0, 0, 0});
        CHECK(entryAt(snapshot, "gb", "Tiny Tennis (Europe).gb").title == "Tiny Tennis (Europe)");
        const auto failed = rv::test::esde::issuesOf(snapshot.issues, "gamelist-failed");
        REQUIRE(failed.size() == 1);
        CHECK(failed[0].severity == rv::catalog::IssueSeverity::Error);
        CHECK(failed[0].system == "gb");
        CHECK(rv::core::pathFromUtf8(failed[0].file).lexically_normal() == gamelist.lexically_normal());
        // The other systems are read as ever.
        CHECK(countsOf(snapshot, "snes") == StateCounts{8, 5, 0, 1, 0, 2, 0});
    }
    SECTION("locked while ES-DE writes it: read on the second try") {
        rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(2000), {}});
        HANDLE locked = CreateFileW(rv::core::extendedLengthPath(gamelist).c_str(), GENERIC_READ, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(locked != INVALID_HANDLE_VALUE);
        // Released well within the delay, long after the first read (the whole load of the fixture takes a few ms).
        std::thread release([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            CloseHandle(locked);
        });
        const auto snapshot = source.load(profileOf(fixture), {});
        release.join();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->findSystem(rv::catalog::SystemName::parse("gb").value())->gamelist ==
              rv::catalog::GamelistState::Read);
        CHECK(source.lastStatistics().gamelistRetries == 1);
        CHECK(countsOf(*snapshot, "gb") == StateCounts{11, 8, 1, 0, 0, 1, 1});
    }
    SECTION("the wait before the second try ends when the load is cancelled") {
        rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(60000), {}});
        HANDLE locked = CreateFileW(rv::core::extendedLengthPath(gamelist).c_str(), GENERIC_READ, 0, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(locked != INVALID_HANDLE_VALUE);
        std::atomic<bool> cancel{false};
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            cancel = true;
        });
        rv::catalog::LibraryLoadOptions options;
        options.cancel = &cancel;
        const auto start = std::chrono::steady_clock::now();
        const auto snapshot = source.load(profileOf(fixture), options);
        const auto took = std::chrono::steady_clock::now() - start;
        canceller.join();
        CloseHandle(locked);
        REQUIRE_FALSE(snapshot.has_value());
        CHECK(snapshot.error().code == rv::ErrorCode::Cancelled);
        CHECK(took < std::chrono::seconds(30));
    }
}

TEST_CASE("esde/library_source: a system defined twice is the first definition that finds a game",
          "[esde][library_source]") {
    const rv::test::Fixture fixture = materialize("esde/custom-systems");
    {
        rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(0), {"gb"}});
        const LibrarySnapshot snapshot = loadSnapshot(source, profileOf(fixture));
        REQUIRE(snapshot.systems.size() == 1);
        CHECK(snapshot.systems[0].fullName == "Game Boy (custom)");
        CHECK(snapshot.systems[0].romFolder.lexically_normal() == fixture.path("roms/gb-custom").lexically_normal());
        CHECK(rv::test::esde::pathsIn(snapshot, "gb", EntryState::Game) ==
              std::vector<std::string>{"Custom Climb (World).gbx"});
        CHECK(rv::test::esde::issuesOf(snapshot.issues, "system-fallback-used").empty());
    }
    // The custom definition finds no game: ES-DE loads the bundled one.
    REQUIRE(fs::remove(fixture.path("roms/gb-custom/Custom Climb (World).gbx")));
    rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(0), {"gb"}});
    const LibrarySnapshot snapshot = loadSnapshot(source, profileOf(fixture));
    REQUIRE(snapshot.systems.size() == 1);
    CHECK(snapshot.systems[0].romFolder.lexically_normal() == fixture.path("roms/gb").lexically_normal());
    CHECK(rv::test::esde::pathsIn(snapshot, "gb", EntryState::Game) ==
          std::vector<std::string>{"Bundled Bounce (World).gb"});
    CHECK(rv::test::esde::issuesOf(snapshot.issues, "system-fallback-used").size() == 1);
    CHECK(source.lastStatistics().systemsScanned == 2);

    // A custom folder that is not there at all, whose gamelist still names games: ES-DE skips the definition all the
    // same (the games the gamelist alone tells of are none it would list).
    rv::test::removeTreeAnyLength(fixture.path("roms/gb-custom"));
    REQUIRE(rv::test::writeFileAnyLength(fixture.path("ES-DE/gamelists/gb/gamelist.xml"),
                                         "<gameList><game><path>./Bundled Bounce (World).gb</path><name>Bounce</name>"
                                         "</game></gameList>"));
    const LibrarySnapshot missing = loadSnapshot(source, profileOf(fixture));
    REQUIRE(missing.systems.size() == 1);
    CHECK(missing.systems[0].romFolder.lexically_normal() == fixture.path("roms/gb").lexically_normal());
    CHECK(missing.systems[0].availability == rv::catalog::Availability::Available);
    CHECK(entryAt(missing, "gb", "Bundled Bounce (World).gb").title == "Bounce");
    CHECK(rv::test::esde::issuesOf(missing.issues, "system-fallback-used").size() == 1);
}
