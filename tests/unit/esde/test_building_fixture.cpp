// The building's fixture (I4 step 2, tests/fixtures/esde/building/README.md): its systems over four floors and the
// ground, as the ROM scan and the library snapshot read them (games, disc groups, the custom system and its platform
// tag, the excluded kodi), their media as esde::EsdeMediaResolver finds them, every cover decoding at its shape, the
// clips there; and the same after additions/ is laid over the root (a new system, titles added to three).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <rv/assets/image_io.hpp>
#include <rv/catalog/entry.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>

#include "snapshot_test_support.hpp"
#include "support/fixture.hpp"

namespace fs = std::filesystem;
using rv::catalog::CatalogEntry;
using rv::catalog::EntryMedia;
using rv::catalog::EntryState;
using rv::catalog::LibrarySnapshot;
using rv::catalog::MediaKind;
using rv::test::esde::countsOf;
using rv::test::esde::entryAt;
using rv::test::esde::loadSnapshot;
using rv::test::esde::materialize;
using rv::test::esde::profileOf;
using rv::test::esde::StateCounts;

namespace {

LibrarySnapshot load(const rv::test::Fixture& fixture) {
    rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(0), {}});
    return loadSnapshot(source, profileOf(fixture));
}

/// Per system: the games with a cover, with a clip, and the cover files' shapes (landscape, portrait, square).
struct MediaCounts {
    std::size_t covers = 0;
    std::size_t videos = 0;
    std::size_t landscape = 0;
    std::size_t portrait = 0;
    std::size_t square = 0;

    bool operator==(const MediaCounts&) const = default;
};

std::ostream& operator<<(std::ostream& out, const MediaCounts& counts) {
    return out << "{covers " << counts.covers << ", videos " << counts.videos << ", landscape " << counts.landscape
               << ", portrait " << counts.portrait << ", square " << counts.square << "}";
}

std::map<std::string, MediaCounts> mediaCounts(const rv::test::Fixture& fixture, const LibrarySnapshot& snapshot) {
    rv::esde::EsdeMediaResolver resolver(fixture.path("media"));
    std::map<std::string, MediaCounts> counts;
    for (const CatalogEntry& entry : snapshot.entries) {
        if (entry.state != EntryState::Game) {
            continue;
        }
        auto presence = resolver.presence(entry.system, entry.key, false);
        INFO(entry.key.relativePath());
        REQUIRE(presence.has_value());
        MediaCounts& system = counts[entry.system.str()];
        if ((*presence)[MediaKind::Video].present) {
            ++system.videos;
        }
        if (!(*presence)[MediaKind::Cover].present) {
            continue;
        }
        ++system.covers;
        auto path = resolver.path(entry.system, entry.key, false, MediaKind::Cover, (*presence)[MediaKind::Cover]);
        REQUIRE(path.has_value());
        const auto image = rv::assets::loadImage(*path);
        REQUIRE(image.has_value());
        (image->width > image->height ? system.landscape : image->width < image->height ? system.portrait
                                                                                         : system.square) += 1;
    }
    return counts;
}

} // namespace

TEST_CASE("esde/building_fixture: the building's systems, games and media", "[esde][fixture][building]") {
    const rv::test::Fixture fixture = materialize("esde/building");
    const LibrarySnapshot snapshot = load(fixture);
    std::map<std::string, std::string> fullNames;
    std::map<std::string, std::vector<std::string>> platforms;
    for (const rv::catalog::SystemInfo& system : snapshot.systems) {
        fullNames[system.name.str()] = system.fullName;
        platforms[system.name.str()] = system.platforms;
    }
    // ES-DE's systems with invented full names, the custom one with gb's platform tag; megadrive's folder is not there.
    CHECK(fullNames == std::map<std::string, std::string>{{"amiga", "Lumen Desk Computer"},
                                                          {"gb", "Pocket Lumen"},
                                                          {"kodi", "Media Center Shortcuts"},
                                                          {"mame", "Arcade Board Library"},
                                                          {"megadrive", "Lumen Drive Home System"},
                                                          {"pocketlab", "Pocket Lab Homebrew"},
                                                          {"pokemini", "Pico Pocket"},
                                                          {"psx", "Orbit Disc System"},
                                                          {"snes", "Lumen 16 Home System"}});
    CHECK(platforms["pocketlab"] == std::vector<std::string>{"gb"});
    CHECK(platforms["kodi"] == std::vector<std::string>{"pcwindows"});

    // entries, games, missing, not a game, disc members, folders, outside
    CHECK(countsOf(snapshot, "snes") == StateCounts{42, 42, 0, 0, 0, 0, 0});
    CHECK(countsOf(snapshot, "psx") == StateCounts{45, 40, 0, 0, 5, 0, 0});
    CHECK(countsOf(snapshot, "gb") == StateCounts{32, 32, 0, 0, 0, 0, 0});
    CHECK(countsOf(snapshot, "mame") == StateCounts{25, 25, 0, 0, 0, 0, 0});
    CHECK(countsOf(snapshot, "pokemini") == StateCounts{8, 8, 0, 0, 0, 0, 0});
    CHECK(countsOf(snapshot, "amiga") == StateCounts{20, 20, 0, 0, 0, 0, 0});
    CHECK(countsOf(snapshot, "pocketlab") == StateCounts{6, 6, 0, 0, 0, 0, 0});
    CHECK(countsOf(snapshot, "kodi") == StateCounts{2, 2, 0, 0, 0, 0, 0});
    CHECK(countsOf(snapshot, "megadrive") == StateCounts{0, 0, 0, 0, 0, 0, 0});
    // The two multi-disc games: their playlists are the games, their discs members.
    const CatalogEntry& twin = entryAt(snapshot, "psx", "Twin Harbor Saga (USA).m3u");
    CHECK(twin.state == EntryState::Game);
    CHECK(twin.title == "Twin Harbor Saga (USA)"); // no gamelist entry: the file's name
    CHECK(entryAt(snapshot, "psx", "Starfall Requiem (Europe) (Disc 3).chd").state == EntryState::DiscMember);

    const std::map<std::string, MediaCounts> media = mediaCounts(fixture, snapshot);
    CHECK(media.at("snes") == MediaCounts{42, 2, 41, 1, 0});
    CHECK(media.at("psx") == MediaCounts{40, 2, 0, 13, 27});
    CHECK(media.at("gb") == MediaCounts{32, 1, 16, 16, 0});
    CHECK(media.at("mame") == MediaCounts{15, 1, 0, 15, 0});
    CHECK(media.at("pokemini") == MediaCounts{8, 0, 0, 0, 8});
    CHECK(media.at("amiga") == MediaCounts{}); // no media at all
    CHECK(media.at("pocketlab") == MediaCounts{6, 0, 6, 0, 0});
    CHECK_FALSE(fs::exists(fixture.path("media/amiga")));
}

TEST_CASE("esde/building_fixture: additions/ adds a system and titles to three", "[esde][fixture][building]") {
    const rv::test::Fixture fixture = materialize("esde/building");
    // The additions' gamelists are the first ones with their new entries appended.
    for (const std::string_view system : {"snes", "psx", "pokemini"}) {
        INFO(system);
        const auto first = rv::core::readTextFile(fixture.path("ES-DE/gamelists/" + std::string(system) + "/gamelist.xml"));
        const auto later =
            rv::core::readTextFile(fixture.path("additions/ES-DE/gamelists/" + std::string(system) + "/gamelist.xml"));
        REQUIRE(first.has_value());
        REQUIRE(later.has_value());
        CHECK(later->starts_with(first->substr(0, first->rfind("</gameList>"))));
    }
    REQUIRE(fixture.applyOverlay("additions"));
    const LibrarySnapshot snapshot = load(fixture);
    CHECK(countsOf(snapshot, "megadrive") == StateCounts{12, 12, 0, 0, 0, 0, 0});
    CHECK(countsOf(snapshot, "snes") == StateCounts{47, 47, 0, 0, 0, 0, 0});
    CHECK(countsOf(snapshot, "psx") == StateCounts{50, 45, 0, 0, 5, 0, 0});
    CHECK(countsOf(snapshot, "pokemini") == StateCounts{32, 32, 0, 0, 0, 0, 0});
    const std::map<std::string, MediaCounts> media = mediaCounts(fixture, snapshot);
    CHECK(media.at("megadrive").covers == 12);
    CHECK(media.at("snes").covers == 47);
    CHECK(media.at("psx").covers == 45);
    CHECK(media.at("pokemini").covers == 32);
}
