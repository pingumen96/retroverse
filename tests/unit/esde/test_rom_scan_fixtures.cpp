// The ROM scan and the library snapshot (I2 step 7) on the fixtures made for them, each as its README counts it:
// nested-roms (namesakes, deep nesting, game directories, links, a folder that cannot be listed), multi-disc (cue
// sheets and playlists, chains, a cycle, encodings, the cache), arcade (MAME BIOS and device sets, names), noload
// (noload.txt, hidden folders, ParseGamelistOnly) and long-path (paths past MAX_PATH).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
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
using rv::test::esde::codesOf;
using rv::test::esde::countsOf;
using rv::test::esde::entryAt;
using rv::test::esde::findEntry;
using rv::test::esde::issuesOf;
using rv::test::esde::loadSnapshot;
using rv::test::esde::materialize;
using rv::test::esde::pathsIn;
using rv::test::esde::profileOf;
using rv::test::esde::StateCounts;

namespace {

rv::esde::EsdeLibrarySource quickSource() {
    return rv::esde::EsdeLibrarySource({std::chrono::milliseconds(0), {}});
}

LibrarySnapshot load(const rv::test::Fixture& fixture, const rv::catalog::LibraryLoadOptions& options = {}) {
    rv::esde::EsdeLibrarySource source = quickSource();
    return loadSnapshot(source, profileOf(fixture), options);
}

void setSetting(const rv::test::Fixture& fixture, std::string_view key, bool from, bool to) {
    const auto text = [&](bool value) {
        return "name=\"" + std::string(key) + "\" value=\"" + (value ? "true" : "false") + "\"";
    };
    REQUIRE(fixture.replaceText("ES-DE/settings/es_settings.xml", text(from), text(to)));
}

/// A file held open without sharing while the object lives: readers get a sharing violation.
class ExclusiveLock {
public:
    explicit ExclusiveLock(const fs::path& file)
        : m_handle(CreateFileW(rv::core::extendedLengthPath(file).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr)) {}
    ~ExclusiveLock() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }
    }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;
    [[nodiscard]] bool locked() const noexcept { return m_handle != INVALID_HANDLE_VALUE; }

private:
    HANDLE m_handle;
};

std::vector<std::string> titlesOf(const LibrarySnapshot& snapshot, std::string_view system, EntryState state) {
    std::vector<std::string> titles;
    for (const CatalogEntry& entry : snapshot.entries) {
        if (entry.system.str() == system && entry.state == state) {
            titles.push_back(entry.title);
        }
    }
    std::sort(titles.begin(), titles.end());
    return titles;
}

} // namespace

TEST_CASE("esde/rom_scan: nested-roms", "[esde][rom_scan]") {
    const rv::test::Fixture fixture = materialize("esde/nested-roms");
    // A name ending with a dot, written through its \\?\ form (the materializer's paths drop the dot).
    {
        auto odd = rv::core::extendedLengthPathBelow(fixture.path("roms/xbox360"), "Odd Name.");
        REQUIRE(odd.has_value());
        const HANDLE file =
            CreateFileW(odd->c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        REQUIRE(file != INVALID_HANDLE_VALUE);
        CloseHandle(file);
    }
    const LibrarySnapshot snapshot = load(fixture);

    CHECK(countsOf(snapshot, "nes") == StateCounts{10, 6, 1, 0, 0, 3, 0});
    CHECK(countsOf(snapshot, "ps3") == StateCounts{3, 3, 0, 0, 0, 0, 0});
    CHECK(countsOf(snapshot, "xbox360") == StateCounts{4, 4, 0, 0, 0, 0, 0});
    REQUIRE(snapshot.systems.size() == 3);
    CHECK(snapshot.systems[0].gamelist == rv::catalog::GamelistState::Read);
    CHECK(snapshot.systems[1].gamelist == rv::catalog::GamelistState::Absent);

    // Namesakes: three files, one key each, one with metadata.
    CHECK(entryAt(snapshot, "nes", "Hero Quest (USA).nes").title == "Hero Quest (USA)");
    CHECK(entryAt(snapshot, "nes", "Shelf A/Hero Quest (USA).nes").title == "Hero Quest (USA)");
    CHECK(entryAt(snapshot, "nes", "Shelf B/Hero Quest (USA).nes").title == "Hero Quest");
    CHECK(entryAt(snapshot, "nes", "L1/L2/L3/L4/L5/L6/Deep Diver (World).nes").title == "Deep Diver");
    CHECK(entryAt(snapshot, "nes", "Vol. 1.5/Point Five (Europe).NES").state == EntryState::Game);
    const CatalogEntry& lost = entryAt(snapshot, "nes", "L1/L2/Lost Level (USA).nes");
    CHECK(lost.state == EntryState::MissingFile);
    CHECK(lost.availability == Availability::Missing);
    CHECK(pathsIn(snapshot, "nes", EntryState::Folder) == std::vector<std::string>{"Empty Shelf", "L1/L2", "Shelf A"});

    // A directory named with a configured extension: one game, never entered.
    const CatalogEntry& collection = entryAt(snapshot, "nes", "Collection.zip");
    CHECK(collection.state == EntryState::Game);
    CHECK(collection.title == "Collection Box");
    REQUIRE(collection.file.has_value());
    CHECK(collection.file->directory);
    CHECK(collection.file->size == 0);
    CHECK(findEntry(snapshot, "nes", "Collection.zip/Inside One (USA).nes") == nullptr);
    CHECK(findEntry(snapshot, "nes", "Collection.zip/Inside Two (USA).nes") == nullptr);
    CHECK(findEntry(snapshot, "nes", ".nes") == nullptr);
    CHECK(findEntry(snapshot, "nes", "Shelf B") == nullptr);
    CHECK(findEntry(snapshot, "nes", "Readme.txt") == nullptr);

    CHECK(titlesOf(snapshot, "ps3", EntryState::Game) ==
          std::vector<std::string>{"Helper", "Puzzle Box (USA)", "Space Odyssey (Europe)"});
    CHECK(entryAt(snapshot, "ps3", "Space Odyssey (Europe).ps3").file->directory);
    CHECK(entryAt(snapshot, "ps3", "Tools/Helper.ps3").file->directory);
    CHECK(titlesOf(snapshot, "xbox360", EntryState::Game) ==
          std::vector<std::string>{"Odd Name", "Ring World", "content", "default"});
    CHECK(findEntry(snapshot, "xbox360", "Ring World") == nullptr);
    CHECK(findEntry(snapshot, "xbox360", "Odd Name.") != nullptr);

    CHECK(codesOf(snapshot.issues) == std::vector<std::string>{"gamelist-duplicate-path", "rom-name-empty",
                                                               "gamelist-kind-mismatch", "gamelist-kind-mismatch",
                                                               "gamelist-entry-inside-game",
                                                               "gamelist-folder-not-found"});
    for (const rv::catalog::Issue& issue : snapshot.issues) {
        CHECK(issue.system == "nes");
        CHECK_FALSE(issue.file.empty());
    }
}

TEST_CASE("esde/rom_scan: links are followed unless they loop; a folder that cannot be listed is unavailable",
          "[esde][rom_scan]") {
    const rv::test::Fixture fixture = materialize("esde/nested-roms");
    auto loop = rv::test::Junction::create(fixture.path("roms/nes/Loop"), fixture.path("roms/nes"));
    REQUIRE(loop.has_value());
    auto linked = rv::test::Junction::create(fixture.path("roms/nes/Linked"), fixture.path("extra"));
    REQUIRE(linked.has_value());
    auto denied = rv::test::ListingDenied::apply(fixture.path("roms/nes/L1/L2"));
    REQUIRE(denied.has_value());

    const LibrarySnapshot snapshot = load(fixture);
    REQUIRE(snapshot.unavailableFolders.size() == 2);
    CHECK(snapshot.unavailableFolders[0].relativePath == "L1/L2");
    CHECK(snapshot.unavailableFolders[0].reason == rv::catalog::SourceUnavailableReason::NotListable);
    CHECK(snapshot.unavailableFolders[1].relativePath == "Loop");
    CHECK(snapshot.unavailableFolders[1].reason == rv::catalog::SourceUnavailableReason::NotListable);
    CHECK(issuesOf(snapshot.issues, "rom-folder-unavailable").size() == 2);

    // Through the junction: one more game.
    CHECK(entryAt(snapshot, "nes", "Linked/Linked Game (USA).nes").state == EntryState::Game);
    // Below the folder that cannot be listed: what the gamelist alone tells, never missing.
    const CatalogEntry& deep = entryAt(snapshot, "nes", "L1/L2/L3/L4/L5/L6/Deep Diver (World).nes");
    CHECK(deep.state == EntryState::Game);
    CHECK(deep.availability == Availability::SourceUnavailable);
    CHECK(deep.unavailableReason == rv::catalog::SourceUnavailableReason::NotListable);
    CHECK_FALSE(deep.file.has_value());
    CHECK(deep.title == "Deep Diver");
    const CatalogEntry& lost = entryAt(snapshot, "nes", "L1/L2/Lost Level (USA).nes");
    CHECK(lost.state == EntryState::Game);
    CHECK(lost.availability == Availability::SourceUnavailable);
    CHECK(entryAt(snapshot, "nes", "L1/L2").state == EntryState::Folder);
    CHECK(entryAt(snapshot, "nes", "L1/L2").availability == Availability::Available);
    CHECK(countsOf(snapshot, "nes") == StateCounts{11, 8, 0, 0, 0, 3, 0});
}

TEST_CASE("esde/rom_scan: a missing ROM folder keeps its gamelist's entries as source unavailable",
          "[esde][rom_scan]") {
    const rv::test::Fixture fixture = materialize("esde/nested-roms");
    rv::test::removeTreeAnyLength(fixture.path("roms/nes"));
    const LibrarySnapshot snapshot = load(fixture);
    const rv::catalog::SystemInfo* nes = snapshot.findSystem(rv::catalog::SystemName::parse("nes").value());
    REQUIRE(nes != nullptr);
    CHECK(nes->availability == Availability::SourceUnavailable);
    CHECK(nes->unavailableReason == rv::catalog::SourceUnavailableReason::FolderMissing);
    CHECK(nes->gamelist == rv::catalog::GamelistState::Read);
    // One entry per key: the game entries, and the folder entries of keys without one.
    for (const CatalogEntry& entry : snapshot.entries) {
        if (entry.system.str() == "nes") {
            CHECK(entry.availability == Availability::SourceUnavailable);
            CHECK(entry.unavailableReason == rv::catalog::SourceUnavailableReason::FolderMissing);
            CHECK_FALSE(entry.file.has_value());
        }
    }
    CHECK(pathsIn(snapshot, "nes", EntryState::Game) ==
          std::vector<std::string>{"Collection.zip", "Collection.zip/Inside One (USA).nes",
                                   "L1/L2/L3/L4/L5/L6/Deep Diver (World).nes", "L1/L2/Lost Level (USA).nes",
                                   "Shelf B/Hero Quest (USA).nes"});
    // "./Shelf B" names no configured extension: not a game, from the gamelist alone.
    CHECK(entryAt(snapshot, "nes", "Shelf B").notAGameReason == NotAGameReason::ExtensionNotConfigured);
    CHECK(pathsIn(snapshot, "nes", EntryState::Folder) ==
          std::vector<std::string>{"Empty Shelf", "Gone Shelf", "L1/L2", "Shelf A"});
    CHECK(issuesOf(snapshot.issues, "system-folder-missing").size() == 1);
}

TEST_CASE("esde/rom_scan: multi-disc", "[esde][rom_scan][disc_groups]") {
    const rv::test::Fixture fixture = materialize("esde/multi-disc");
    // The Windows-1252 cue sheet, written here (hostile encodings are generated at test time).
    REQUIRE(rv::test::writeFileAnyLength(fixture.path("roms/psx/Caf\xC3\xA9 Noir (France).cue"),
                                         "FILE \"Caf\xE9 Noir (France).bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n"
                                         "    INDEX 01 00:00:00\r\n"));
    rv::esde::EsdeLibrarySource source = quickSource();
    const LibrarySnapshot snapshot = loadSnapshot(source, profileOf(fixture));

    CHECK(countsOf(snapshot, "psx") == StateCounts{33, 15, 1, 0, 17, 0, 0});
    CHECK(countsOf(snapshot, "ps2") == StateCounts{3, 2, 0, 1, 0, 0, 0});
    CHECK(snapshot.discGroups.size() == 15);

    const auto headOf = [&](std::string_view member) {
        return entryAt(snapshot, "psx", member).discHead.relativePath();
    };
    const auto membersOf = [&](std::string_view head) {
        std::vector<std::string> members;
        for (const rv::catalog::DiscGroup& group : snapshot.discGroups) {
            if (group.head.relativePath() == head) {
                for (const rv::catalog::LookupKey& member : group.members) {
                    members.push_back(member.relativePath());
                }
            }
        }
        return members;
    };
    CHECK(membersOf("Twin Peak (Europe).cue") ==
          std::vector<std::string>{"Twin Peak (Europe) (Track 1).bin", "Twin Peak (Europe) (Track 2).bin"});
    // The chain: the playlist's members are the cue sheets, whose members are the tracks.
    CHECK(membersOf("Saga (USA).m3u") ==
          std::vector<std::string>{"Saga (USA) (Disc 1).cue", "Saga (USA) (Disc 2).cue"});
    CHECK(headOf("Saga (USA) (Disc 1).cue") == "Saga (USA).m3u");
    CHECK(headOf("Saga (USA) (Disc 2).bin") == "Saga (USA) (Disc 2).cue");
    CHECK(entryAt(snapshot, "psx", "Saga (USA).m3u").state == EntryState::Game);
    // The cycle: Loop A stays a game, Loop B its member, Loop B's own track Loop B's.
    CHECK(entryAt(snapshot, "psx", "Loop A (World).m3u").state == EntryState::Game);
    CHECK(headOf("Loop B (World).m3u") == "Loop A (World).m3u");
    CHECK(headOf("Loop B (World).bin") == "Loop B (World).m3u");
    CHECK(issuesOf(snapshot.issues, "disc-group-cycle").size() == 1);
    // The oversized sheet: read as nothing, its track stays a game.
    CHECK(entryAt(snapshot, "psx", "Huge Sheet (USA).cue").state == EntryState::Game);
    CHECK(entryAt(snapshot, "psx", "Huge Sheet (USA).bin").state == EntryState::Game);
    CHECK(issuesOf(snapshot.issues, "disc-sheet-unreadable").size() == 1);
    // Windows-1252, another case, "..\", unquoted names.
    CHECK(headOf("Caf\xC3\xA9 Noir (France).bin") == "Caf\xC3\xA9 Noir (France).cue");
    CHECK(headOf("Quiet Storm (USA).bin") == "Quiet Storm (USA).cue");
    CHECK(headOf("Tracks/Echo (Japan).bin") == "Discs/Echo (Japan).cue");
    CHECK(headOf("Plain Words (USA).bin") == "Plain Words (USA).cue");
    // A track two sheets name: in both groups, shown under the first head.
    CHECK(membersOf("Shared B (USA).cue") == std::vector<std::string>{"Shared (USA) (Track 1).bin"});
    CHECK(headOf("Shared (USA) (Track 1).bin") == "Shared A (USA).cue");
    // A reference to nothing.
    CHECK(entryAt(snapshot, "psx", "Broken Link (USA).cue").state == EntryState::Game);
    CHECK(issuesOf(snapshot.issues, "disc-reference-missing").size() == 1);
    // Heads without an entry of their own show a member's metadata, along a chain too.
    const CatalogEntry& hollow = entryAt(snapshot, "psx", "Hollow Vale (Italy) (Disc 1).cue");
    CHECK(hollow.state == EntryState::Game);
    CHECK_FALSE(hollow.fromGamelist());
    CHECK(hollow.title == "Hollow Vale");
    CHECK(hollow.metadata.developer == "Quiet Fox");
    CHECK(entryAt(snapshot, "psx", "Rift (USA).cue").title == "Rift");
    CHECK(entryAt(snapshot, "psx", "Rift (USA).m3u").title == "Rift");
    CHECK(entryAt(snapshot, "psx", "Lost Saga (USA).m3u").state == EntryState::MissingFile);
    // ps2: .cue is not configured, so its track is a game and no group forms.
    const CatalogEntry& moon = entryAt(snapshot, "ps2", "Moon Race (Europe).cue");
    CHECK(moon.state == EntryState::NotAGame);
    CHECK(moon.notAGameReason == NotAGameReason::ExtensionNotConfigured);
    CHECK(entryAt(snapshot, "ps2", "Moon Race (Europe).bin").state == EntryState::Game);
    CHECK(source.lastStatistics().sheetsRead == 16); // 17 heads (13 cue sheets, 4 playlists), the oversized one unread
    CHECK(source.lastStatistics().sheetsUnreadable == 1);

    // The groups handed back as the cache: nothing read again, the same snapshot.
    std::vector<rv::catalog::CachedDiscGroup> cache;
    for (const rv::catalog::DiscGroup& group : snapshot.discGroups) {
        const CatalogEntry& head = entryAt(snapshot, group.system.str(), group.head.relativePath());
        cache.push_back({group, head.file->size, head.file->lastWriteTime});
    }
    rv::catalog::LibraryLoadOptions options;
    options.discGroups = cache;
    const LibrarySnapshot again = loadSnapshot(source, profileOf(fixture), options);
    CHECK(again.entries == snapshot.entries);
    CHECK(again.discGroups == snapshot.discGroups);
    CHECK(source.lastStatistics().sheetsFromCache == 15);
    // The heads without a group are read again: Broken Link, and the oversized sheet, again unreadable.
    CHECK(source.lastStatistics().sheetsRead == 1);
    CHECK(source.lastStatistics().sheetsUnreadable == 1);
}

TEST_CASE("esde/rom_scan: a cue sheet that cannot be read keeps its cached group", "[esde][rom_scan][disc_groups]") {
    const rv::test::Fixture fixture = materialize("esde/multi-disc");
    rv::esde::EsdeLibrarySource source = quickSource();
    const LibrarySnapshot first = loadSnapshot(source, profileOf(fixture));
    const CatalogEntry& twin = entryAt(first, "psx", "Twin Peak (Europe).cue");
    rv::catalog::DiscGroup group;
    for (const rv::catalog::DiscGroup& candidate : first.discGroups) {
        if (candidate.head == twin.key) {
            group = candidate;
        }
    }
    REQUIRE(group.members.size() == 2);
    // A cache recorded for another size: the sheet must be read, and cannot be.
    std::vector<rv::catalog::CachedDiscGroup> cache{{group, twin.file->size + 1, twin.file->lastWriteTime}};
    rv::catalog::LibraryLoadOptions options;
    options.discGroups = cache;
    const ExclusiveLock lock(fixture.path("roms/psx/Twin Peak (Europe).cue"));
    REQUIRE(lock.locked());
    const LibrarySnapshot locked = loadSnapshot(source, profileOf(fixture), options);
    CHECK(entryAt(locked, "psx", "Twin Peak (Europe) (Track 1).bin").state == EntryState::DiscMember);
    CHECK(entryAt(locked, "psx", "Twin Peak (Europe) (Track 2).bin").state == EntryState::DiscMember);
    bool kept = false;
    for (const rv::catalog::Issue& issue : issuesOf(locked.issues, "disc-sheet-unreadable")) {
        kept = kept || (issue.file.ends_with("Twin Peak (Europe).cue") &&
                        issue.reason.find("the group read before is kept") != std::string::npos);
    }
    CHECK(kept);

    // Without a cache, the tracks stay what the listing makes them.
    const LibrarySnapshot uncached = loadSnapshot(source, profileOf(fixture));
    CHECK(entryAt(uncached, "psx", "Twin Peak (Europe) (Track 1).bin").state == EntryState::Game);
}

TEST_CASE("esde/rom_scan: a sheet's reference below a folder that cannot be listed is not missing",
          "[esde][rom_scan][disc_groups]") {
    const rv::test::Fixture fixture = materialize("esde/multi-disc");
    auto denied = rv::test::ListingDenied::apply(fixture.path("roms/psx/Tracks"));
    REQUIRE(denied.has_value());
    const LibrarySnapshot snapshot = load(fixture);
    REQUIRE(snapshot.unavailableFolders.size() == 1);
    CHECK(snapshot.unavailableFolders[0].relativePath == "Tracks");
    // Echo's track is not known: no member, and no reference reported missing but Broken Link's.
    CHECK(entryAt(snapshot, "psx", "Discs/Echo (Japan).cue").state == EntryState::Game);
    const auto missing = issuesOf(snapshot.issues, "disc-reference-missing");
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].file.ends_with("Broken Link (USA).cue"));
}

TEST_CASE("esde/rom_scan: arcade", "[esde][rom_scan][mame]") {
    const rv::test::Fixture fixture = materialize("esde/arcade");
    SECTION("as committed") {
        const LibrarySnapshot snapshot = load(fixture);
        CHECK(countsOf(snapshot, "mame") == StateCounts{11, 8, 1, 2, 0, 0, 0});
        CHECK(countsOf(snapshot, "neogeo") == StateCounts{3, 2, 0, 1, 0, 0, 0});
        CHECK(countsOf(snapshot, "bonus") == StateCounts{2, 2, 0, 0, 0, 0, 0});
        CHECK(entryAt(snapshot, "mame", "zapzone.zip").title == "Zap Zone");
        const CatalogEntry& japan = entryAt(snapshot, "mame", "zapzonej.zip");
        CHECK(japan.title == "Zap Zone");
        CHECK(japan.fromGamelist());
        CHECK(entryAt(snapshot, "mame", "zapzone (alt).zip").title == "Zap Zone");
        CHECK(entryAt(snapshot, "mame", "rocketpig.zip").title == "Rocket Pig Deluxe");
        CHECK(entryAt(snapshot, "mame", "empty1.zip").title == "empty1");
        CHECK(entryAt(snapshot, "mame", "dup1.zip").title == "Second Name & Co.");
        CHECK(entryAt(snapshot, "mame", "unknownset.7z").title == "unknownset");
        const CatalogEntry& bios = entryAt(snapshot, "mame", "fakebios.zip");
        CHECK(bios.state == EntryState::NotAGame);
        CHECK(bios.notAGameReason == NotAGameReason::BiosOrDeviceSet);
        CHECK(bios.title == "Fake BIOS");
        CHECK(entryAt(snapshot, "mame", "fakedev.zip").notAGameReason == NotAGameReason::BiosOrDeviceSet);
        CHECK(entryAt(snapshot, "mame", "Upper/FAKEBIOS.zip").state == EntryState::Game);
        CHECK(findEntry(snapshot, "mame", "(clone).zip") == nullptr);
        CHECK(entryAt(snapshot, "mame", "gonegame.zip").state == EntryState::MissingFile);
        CHECK(entryAt(snapshot, "neogeo", "neobios.zip").notAGameReason == NotAGameReason::BiosOrDeviceSet);
        CHECK(entryAt(snapshot, "neogeo", "glimmer.zip").title == "Glimmer");
        CHECK(entryAt(snapshot, "bonus", "fakebios.zip").state == EntryState::Game);
        CHECK(entryAt(snapshot, "bonus", "zapzone.zip").title == "zapzone");
        CHECK(codesOf(snapshot.issues) == std::vector<std::string>{"rom-name-empty"});
    }
    SECTION("MAMENameStripExtraInfo off") {
        setSetting(fixture, "MAMENameStripExtraInfo", true, false);
        const LibrarySnapshot snapshot = load(fixture);
        CHECK(entryAt(snapshot, "mame", "zapzone.zip").title == "Zap Zone (World, set 1)");
        CHECK(entryAt(snapshot, "mame", "zapzonej.zip").title == "Zap Zone (Japan) [bootleg]");
        CHECK(entryAt(snapshot, "mame", "zapzone (alt).zip").title == "Zap Zone (World, set 1)");
        CHECK(entryAt(snapshot, "neogeo", "glimmer.zip").title == "Glimmer (Neo Geo MVS)");
        CHECK(findEntry(snapshot, "mame", "(clone).zip") == nullptr);
    }
    SECTION("no mamenames.xml: no list at all") {
        REQUIRE(fs::remove(fixture.path("Programs/ES-DE/resources/MAME/mamenames.xml")));
        const LibrarySnapshot snapshot = load(fixture);
        CHECK(countsOf(snapshot, "mame") == StateCounts{11, 10, 1, 0, 0, 0, 0});
        CHECK(countsOf(snapshot, "neogeo") == StateCounts{3, 3, 0, 0, 0, 0, 0});
        CHECK(entryAt(snapshot, "mame", "zapzone (alt).zip").title == "zapzone");
        CHECK(entryAt(snapshot, "mame", "fakebios.zip").title == "Fake BIOS");
        CHECK(issuesOf(snapshot.issues, "mame-list-missing").size() == 1);
    }
    SECTION("the ES-DE home's resources come first") {
        REQUIRE(rv::test::writeFileAnyLength(fixture.path("ES-DE/resources/MAME/mamebioses.xml"),
                                             "<bios>zapzone</bios>\n"));
        const LibrarySnapshot snapshot = load(fixture);
        CHECK(entryAt(snapshot, "mame", "zapzone.zip").notAGameReason == NotAGameReason::BiosOrDeviceSet);
        CHECK(entryAt(snapshot, "neogeo", "zapzone.zip").notAGameReason == NotAGameReason::BiosOrDeviceSet);
        CHECK(entryAt(snapshot, "mame", "fakebios.zip").state == EntryState::Game);
        CHECK(entryAt(snapshot, "mame", "fakedev.zip").notAGameReason == NotAGameReason::BiosOrDeviceSet);
    }
}

TEST_CASE("esde/rom_scan: noload and hidden folders", "[esde][rom_scan]") {
    const rv::test::Fixture fixture = materialize("esde/noload");
    const auto reasonOf = [](const LibrarySnapshot& snapshot, std::string_view system, std::string_view path) {
        const CatalogEntry& entry = entryAt(snapshot, system, path);
        if (entry.state != EntryState::NotAGame) {
            return std::string(rv::catalog::toString(entry.state));
        }
        return entry.excludingSetting.empty() ? std::string(rv::catalog::toString(entry.notAGameReason))
                                              : entry.excludingSetting;
    };
    SECTION("as committed") {
        const LibrarySnapshot snapshot = load(fixture);
        CHECK(countsOf(snapshot, "gba") == StateCounts{9, 5, 0, 4, 0, 0, 0});
        CHECK(countsOf(snapshot, "n64") == StateCounts{2, 0, 0, 2, 0, 0, 0});
        CHECK(countsOf(snapshot, "pcengine") == StateCounts{1, 1, 0, 0, 0, 0, 0});
        CHECK(reasonOf(snapshot, "gba", "Skip Me") == "noload-txt");
        CHECK(entryAt(snapshot, "gba", "Skip Me").file->directory);
        CHECK(reasonOf(snapshot, "gba", "Skip Me/Hidden Away (USA).gba") == "noload-txt");
        CHECK(entryAt(snapshot, "gba", "Skip Me/Hidden Away (USA).gba").title == "Hidden Away");
        CHECK(reasonOf(snapshot, "gba", "Skip Me/Deeper/Lost (USA).gba") == "noload-txt");
        CHECK(reasonOf(snapshot, "gba", "Upper/Shouting (USA).gba") == "noload-txt");
        CHECK(reasonOf(snapshot, "gba", ".secret/Sneaky (USA).gba") == "game");
        CHECK(reasonOf(snapshot, "n64", "Sky Pilot (USA).z64") == "noload-txt");
        CHECK(reasonOf(snapshot, "n64", "Tiny Wings (Japan).n64") == "noload-txt");
        CHECK(reasonOf(snapshot, "pcengine", "Bounce Kid (Japan).pce") == "game");
        CHECK(codesOf(snapshot.issues) == std::vector<std::string>{"system-not-loaded"});
        CHECK(snapshot.issues.front().system == "n64");
    }
    SECTION("ShowHiddenFiles off") {
        setSetting(fixture, "ShowHiddenFiles", true, false);
        const LibrarySnapshot snapshot = load(fixture);
        CHECK(countsOf(snapshot, "gba") == StateCounts{9, 2, 0, 7, 0, 0, 0});
        CHECK(reasonOf(snapshot, "gba", ".secret/Sneaky (USA).gba") == "ShowHiddenFiles");
        CHECK(reasonOf(snapshot, "gba", ".secret/Sneaky Two (USA).gba") == "ShowHiddenFiles");
        CHECK(reasonOf(snapshot, "gba", ".secret/Inner/Found (USA).gba") == "game");
        CHECK(reasonOf(snapshot, "gba", ".secret/Inner/Unlisted (USA).gba") == "ShowHiddenFiles");
        CHECK(reasonOf(snapshot, "gba", "Keep (USA).gba") == "game");
    }
    SECTION("folders with the hidden attribute, ShowHiddenFiles off") {
        setSetting(fixture, "ShowHiddenFiles", true, false);
        REQUIRE(rv::test::setHidden(fixture.path("roms/gba/Skip Me"), true));
        REQUIRE(rv::test::setHidden(fixture.path("roms/pcengine"), true));
        const LibrarySnapshot snapshot = load(fixture);
        // Hidden before noload: ES-DE skips the hidden folder before it looks for a noload.txt in it.
        CHECK(reasonOf(snapshot, "gba", "Skip Me") == "ShowHiddenFiles");
        CHECK(reasonOf(snapshot, "gba", "Skip Me/Hidden Away (USA).gba") == "ShowHiddenFiles");
        CHECK(reasonOf(snapshot, "gba", "Skip Me/Deeper/Lost (USA).gba") == "ShowHiddenFiles");
        // A hidden system folder: ES-DE does not load the system.
        CHECK(reasonOf(snapshot, "pcengine", "Bounce Kid (Japan).pce") == "ShowHiddenFiles");
        std::vector<std::string> notLoaded;
        for (const rv::catalog::Issue& issue : issuesOf(snapshot.issues, "system-not-loaded")) {
            notLoaded.push_back(issue.system);
        }
        CHECK(notLoaded == std::vector<std::string>{"n64", "pcengine"});
    }
    SECTION("ParseGamelistOnly on") {
        setSetting(fixture, "ParseGamelistOnly", false, true);
        const LibrarySnapshot snapshot = load(fixture);
        CHECK(countsOf(snapshot, "gba") == StateCounts{9, 3, 0, 5, 0, 1, 0});
        CHECK(countsOf(snapshot, "n64") == StateCounts{2, 1, 0, 1, 0, 0, 0});
        CHECK(countsOf(snapshot, "pcengine") == StateCounts{1, 0, 0, 1, 0, 0, 0});
        CHECK(reasonOf(snapshot, "gba", "Skip Me") == "folder");
        CHECK(reasonOf(snapshot, "gba", "Skip Me/Hidden Away (USA).gba") == "game");
        CHECK(reasonOf(snapshot, "gba", "Keep (USA).gba") == "ParseGamelistOnly");
        CHECK(reasonOf(snapshot, "n64", "Sky Pilot (USA).z64") == "game");
        CHECK(reasonOf(snapshot, "n64", "Tiny Wings (Japan).n64") == "ParseGamelistOnly");
        CHECK(snapshot.issues.empty());
    }
}

TEST_CASE("esde/rom_scan: paths longer than MAX_PATH", "[esde][rom_scan]") {
    const rv::test::Fixture fixture = materialize("esde/long-path");
    const std::string folder =
        "A Very Long Folder Name Made To Exceed The Classic Windows Path Limit Of Two Hundred And "
        "Sixty (Part One)/Another Long Folder Name For The Same Purpose So That The Whole Path Is "
        "Well Past It (Part Two)";
    const std::string disc = "Long Disc Of An Invented Game With Many Words In Its Title (Europe)";
    REQUIRE(rv::test::writeFileAnyLength(fixture.path("roms/psx/" + folder + "/" + disc + ".cue"),
                                         "FILE \"" + disc + " (Track 1).bin\" BINARY\n  TRACK 01 MODE2/2352\n"));
    const LibrarySnapshot snapshot = load(fixture);
    CHECK(countsOf(snapshot, "snes") == StateCounts{3, 2, 1, 0, 0, 0, 0});
    CHECK(countsOf(snapshot, "psx") == StateCounts{2, 1, 0, 0, 1, 0, 0});
    const std::string road = folder + "/Long Road Of An Invented Game With Many Words In Its Title (USA) (Rev 1).sfc";
    CHECK(road.size() > 260);
    const CatalogEntry& entry = entryAt(snapshot, "snes", road);
    CHECK(entry.title == "Long Road");
    CHECK(entry.file->size == 32768);
    CHECK((rv::core::pathToUtf8(fixture.path("roms/snes")) + "/" + road).size() > 260);
    CHECK(entryAt(snapshot, "psx", folder + "/" + disc + " (Track 1).bin").discHead.relativePath() ==
          folder + "/" + disc + ".cue");
    CHECK(snapshot.issues.empty());
}
