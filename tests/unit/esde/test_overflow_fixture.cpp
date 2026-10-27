// The overflow fixture (I4b step 0, tests/fixtures/esde/overflow/README.md): a big-box computer system of 1,700 titles
// and an arcade of 60 sets, more than their rooms hold (ADR-0015 §7's expansion rooms), as the ROM scan and the library
// snapshot read them, every game with a portrait cover.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <map>
#include <string>
#include <utility>

#include <rv/assets/image_io.hpp>
#include <rv/catalog/entry.hpp>
#include <rv/catalog/media.hpp>
#include <rv/catalog/snapshot.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>

#include "snapshot_test_support.hpp"
#include "support/fixture.hpp"

using rv::catalog::CatalogEntry;
using rv::catalog::EntryState;
using rv::catalog::LibrarySnapshot;
using rv::catalog::MediaKind;
using rv::test::esde::countsOf;
using rv::test::esde::loadSnapshot;
using rv::test::esde::materialize;
using rv::test::esde::profileOf;
using rv::test::esde::StateCounts;

TEST_CASE("esde/overflow_fixture: a big-box system of 1,700 titles and an arcade of 60", "[esde][fixture][overflow]") {
    const rv::test::Fixture fixture = materialize("esde/overflow");
    rv::esde::EsdeLibrarySource source({std::chrono::milliseconds(0), {}});
    const LibrarySnapshot snapshot = loadSnapshot(source, profileOf(fixture));
    std::map<std::string, std::string> fullNames;
    for (const rv::catalog::SystemInfo& system : snapshot.systems) {
        fullNames[system.name.str()] = system.fullName;
    }
    CHECK(fullNames ==
          std::map<std::string, std::string>{{"amiga", "Lumen Desk Computer"}, {"mame", "Arcade Board Library"}});
    // entries, games, missing, not a game, disc members, folders, outside
    CHECK(countsOf(snapshot, "amiga") == StateCounts{1700, 1700, 0, 0, 0, 0, 0});
    CHECK(countsOf(snapshot, "mame") == StateCounts{60, 60, 0, 0, 0, 0, 0});

    // Every game has its cover, portrait; one per system decoded at its size.
    rv::esde::EsdeMediaResolver resolver(fixture.path("media"));
    std::map<std::string, std::size_t> covers;
    std::map<std::string, std::pair<int, int>> sizes;
    for (const CatalogEntry& entry : snapshot.entries) {
        if (entry.state != EntryState::Game) {
            continue;
        }
        auto presence = resolver.presence(entry.system, entry.key, false);
        REQUIRE(presence.has_value());
        if (!(*presence)[MediaKind::Cover].present) {
            continue;
        }
        ++covers[entry.system.str()];
        if (!sizes.contains(entry.system.str())) {
            auto path = resolver.path(entry.system, entry.key, false, MediaKind::Cover, (*presence)[MediaKind::Cover]);
            REQUIRE(path.has_value());
            const auto image = rv::assets::loadImage(*path);
            REQUIRE(image.has_value());
            sizes[entry.system.str()] = {image->width, image->height};
        }
    }
    CHECK(covers == std::map<std::string, std::size_t>{{"amiga", 1700}, {"mame", 60}});
    CHECK(sizes == std::map<std::string, std::pair<int, int>>{{"amiga", {160, 220}}, {"mame", {140, 200}}});
}
