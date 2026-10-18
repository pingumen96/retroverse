// The room selection on the real SNES collection (I3 step 2, ADR-0010 §3). Hidden ([.real-library]): ctest never runs
// it; scripts/real-library.ps1 does, with ES-DE closed, read-only snapshots of the library before and after, and the
// folders in the RV_REAL_* variables (test_import_real.cpp lists them). The system snes alone is imported into a new
// database under the build tree (build/<preset>/real-library/room-selection), its room snapshot read on a reader
// (SqliteCatalogQuery::roomEntries), its media checked against the real media root (esde::checkRoomMedia: the root's
// own listing) and its media paths derived (esde::roomMediaPaths: each cover's file is then looked up, never opened),
// and the room selected (world::selectRoom) twice, with one result. The figures are printed for docs/i3/step-2.md
// (the line starting "Room selection on the real snes collection:") beside the read-only survey of 2026-10-18 (7,797
// games, 1,535 with a cover, 890 distinct display titles among them: 727 USA once deduplicated, 57 Europe only, 106
// other), which grouped the covered games by their display title exactly: the same grouping is computed here, and every
// title key that joins several display titles is printed, as is every title whose region the two ways rank apart. The
// checks hold whatever the collection holds (this machine's figures are not versioned): the media known, every
// covered game a title or a duplicate of one, no more titles than display titles, every derived cover found.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include <rv/catalog/import_service.hpp>
#include <rv/catalog_db/catalog_query.hpp>
#include <rv/catalog_db/database.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/text.hpp>
#include <rv/esde/library_source.hpp>
#include <rv/esde/media_resolver.hpp>
#include <rv/esde/settings.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/title_key.hpp>
#include <rv/world/world_config.hpp>

#include "import_test_support.hpp"
#include "support/fixture.hpp"

namespace fs = std::filesystem;
using namespace rv::catalog;
using Clock = std::chrono::steady_clock;

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

double millisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// The survey's regions of a version: whether its region tag names USA, else Europe (No-Intro names or GoodTools
/// codes, read as world::readVersionTags() reads them, but World apart).
enum class SurveyRegion { Usa, Europe, Other };

SurveyRegion surveyRegion(const RoomEntry& entry) {
    const rv::world::VersionTags tags = rv::world::readVersionTags(rv::world::fileNameOf(entry.key.relativePath()));
    if (!tags.regionTag) {
        return SurveyRegion::Other;
    }
    const std::string& tag = *tags.regionTag;
    const bool goodTools = tag.find(' ') == std::string::npos && tag.find(',') == std::string::npos && tag != "USA" &&
                           tag != "Europe" && tag != "World" && tag.size() <= 4;
    if (goodTools ? (tag.find('U') != std::string::npos && tag != "UK") || tag == "4"
                  : tag.find("USA") != std::string::npos) {
        return SurveyRegion::Usa;
    }
    if (goodTools ? tag.find('E') != std::string::npos : tag.find("Europe") != std::string::npos) {
        return SurveyRegion::Europe;
    }
    return SurveyRegion::Other;
}

} // namespace

TEST_CASE("world/room_selection: the real snes collection", "[.real-library][real-library][room_selection]") {
    const auto home = pathVariable(L"RV_REAL_ESDE_HOME");
    const auto install = pathVariable(L"RV_REAL_ESDE_INSTALL");
    const auto romRoot = pathVariable(L"RV_REAL_ROM_ROOT");
    const auto mediaRoot = pathVariable(L"RV_REAL_MEDIA_ROOT");
    if (!home || !install || !romRoot || !mediaRoot) {
        SKIP("set RV_REAL_ESDE_HOME, RV_REAL_ESDE_INSTALL, RV_REAL_ROM_ROOT and RV_REAL_MEDIA_ROOT "
             "(scripts/real-library.ps1 does)");
    }
    LibraryProfile profile;
    profile.esdeHome = *home;
    profile.installFolder = *install;
    profile.romRoot = *romRoot;
    profile.mediaRoot = *mediaRoot;
    const SystemName snes = SystemName::parse("snes").value();

    // A new database under the build tree, never in the library, with the system imported as retroverse-cli import
    // --system snes imports it.
    const fs::path folder = rv::test::testProgramsDir().parent_path() / "real-library" / "room-selection";
    std::error_code removed;
    fs::remove_all(folder, removed);
    REQUIRE_FALSE(removed);
    rv::test::ImportCatalog catalog = rv::test::ImportCatalog::open(folder / "retroverse.sqlite");
    const auto settings = rv::esde::readSettings(profile.esdeHome);
    REQUIRE(settings.has_value());
    {
        rv::esde::EsdeLibrarySourceOptions sourceOptions;
        sourceOptions.systems = {"snes"};
        rv::esde::EsdeLibrarySource source(sourceOptions);
        rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(*settings, profile));
        ImportOptions options;
        options.systems = {snes};
        const ImportResult imported = rv::test::importOk(catalog, source, media, profile, options);
        REQUIRE(imported.statistics.mediaUnknown == 0);
    }

    auto query = rv::catalog_db::SqliteCatalogQuery::open(catalog.database->openReader().value());
    REQUIRE(query.has_value());
    const Clock::time_point read = Clock::now();
    auto snapshot = (*query)->roomEntries(snes);
    const double readMs = millisecondsSince(read);
    REQUIRE(snapshot.has_value());
    rv::esde::EsdeMediaResolver media(rv::esde::resolveMediaRoot(*settings, profile));
    rv::esde::checkRoomMedia(*snapshot, media);
    REQUIRE(snapshot->mediaKnown);
    CHECK(media.stats().systemsListed == 0);

    // The media paths, and each derived cover's file looked up (its folder never listed).
    const Clock::time_point derived = Clock::now();
    const std::vector<rv::esde::RoomMediaPaths> paths = rv::esde::roomMediaPaths(*snapshot);
    const double pathsMs = millisecondsSince(derived);
    std::size_t covers = 0;
    std::size_t coversFound = 0;
    for (const rv::esde::RoomMediaPaths& entry : paths) {
        if (const std::string& cover = entry[MediaKind::Cover]; !cover.empty()) {
            ++covers;
            const auto file = rv::core::extendedLengthPathBelow(*mediaRoot, cover);
            std::error_code error;
            coversFound += file && fs::is_regular_file(*file, error) ? 1 : 0;
        }
    }
    CHECK(coversFound == covers);

    const Clock::time_point selected = Clock::now();
    const auto selection = rv::world::selectRoom(rv::world::RoomInput{profile.id, *snapshot}, rv::world::WorldConfig{});
    const double selectMs = millisecondsSince(selected);
    REQUIRE(selection.has_value());
    const auto again = rv::world::selectRoom(rv::world::RoomInput{profile.id, *snapshot}, rv::world::WorldConfig{});
    REQUIRE(again.has_value());
    CHECK(*again == *selection);
    CHECK(rv::world::writeRoomSelection(*again).value() == rv::world::writeRoomSelection(*selection).value());
    CHECK(selection->eligibility == rv::world::Eligibility::Covered);

    // The entries, the games and the covered games.
    std::map<GameId, const RoomEntry*> byId;
    std::map<EntryState, std::size_t> states;
    std::size_t games = 0;
    std::size_t hidden = 0;
    std::size_t covered = 0;
    std::size_t unavailableCovered = 0;
    for (const RoomEntry& entry : snapshot->entries) {
        byId[entry.id] = &entry;
        ++states[entry.state];
        const bool game = entry.state == EntryState::Game || entry.state == EntryState::MissingFile;
        if (game && entry.hidden) {
            ++hidden;
        } else if (game) {
            ++games;
            if (entry.media[MediaKind::Cover].present) {
                ++covered;
                unavailableCovered += entry.state == EntryState::MissingFile ? 1 : 0;
            }
        }
    }
    std::map<rv::world::UnplacedReason, std::size_t> reasons;
    for (const rv::world::UnplacedGame& game : selection->unplaced) {
        ++reasons[game.reason];
    }
    const std::size_t titles = selection->titles.size();
    CHECK(titles + reasons[rv::world::UnplacedReason::DuplicateTitle] + unavailableCovered == covered);
    CHECK(titles + selection->unplaced.size() == games);

    // The representatives' regions (ADR-0010's ranks).
    std::map<rv::world::RegionRank, std::size_t> ranks;
    std::map<std::string, std::size_t> regionTags;
    for (const rv::world::RoomTitle& title : selection->titles) {
        const RoomEntry& entry = *byId.at(title.game);
        const rv::world::VersionTags tags = rv::world::readVersionTags(rv::world::fileNameOf(entry.key.relativePath()));
        ++ranks[tags.region];
        ++regionTags[tags.regionTag.value_or("(none)")];
    }

    // The survey's way: the covered games grouped by their display title exactly, the USA release preferred, then
    // Europe, then the rest (missing files left out: the survey found none).
    std::map<std::string, std::vector<const RoomEntry*>> byTitle;
    for (const RoomEntry& entry : snapshot->entries) {
        if (entry.state == EntryState::Game && !entry.hidden && entry.media[MediaKind::Cover].present) {
            byTitle[entry.title].push_back(&entry);
        }
    }
    // Also read strictly: a version is USA only when its region tag is exactly "USA", Europe exactly "Europe".
    std::map<SurveyRegion, std::size_t> surveyRegions;
    std::map<SurveyRegion, std::size_t> strictRegions;
    std::map<std::string, std::set<std::string>> titlesByKey;
    for (const auto& [title, versions] : byTitle) {
        SurveyRegion best = SurveyRegion::Other;
        SurveyRegion strict = SurveyRegion::Other;
        for (const RoomEntry* version : versions) {
            best = std::min(best, surveyRegion(*version));
            const auto tag = rv::world::readVersionTags(rv::world::fileNameOf(version->key.relativePath())).regionTag;
            strict = std::min(strict, tag == "USA" ? SurveyRegion::Usa
                                      : tag == "Europe" ? SurveyRegion::Europe
                                                        : SurveyRegion::Other);
        }
        ++surveyRegions[best];
        ++strictRegions[strict];
        titlesByKey[rv::world::titleKey(title).value()].insert(title);
    }
    // One title per title key of the eligible games, never more than the display titles.
    CHECK(titles == titlesByKey.size());
    CHECK(titles <= byTitle.size());

    std::cout << fmt::format(
        "Room selection on the real snes collection: {} entries ({} games, {} missing files, {} not games, {} disc "
        "members, {} folders), {} games considered ({} hidden left out), {} with a cover; {} titles chosen ({} "
        "USA or World, {} Europe, {} other); unplaced: {} duplicate-title, {} no-cover, {} unavailable. Survey's "
        "grouping (display titles exactly): {} titles ({} USA, {} Europe only, {} other). Times: snapshot {:.1f} ms, "
        "media paths {:.1f} ms, selection {:.1f} ms; {} covers derived, {} found.\n",
        snapshot->entries.size(), states[EntryState::Game], states[EntryState::MissingFile],
        states[EntryState::NotAGame], states[EntryState::DiscMember], states[EntryState::Folder], games, hidden,
        covered, titles, ranks[rv::world::RegionRank::UsaOrWorld], ranks[rv::world::RegionRank::Europe],
        ranks[rv::world::RegionRank::Other], reasons[rv::world::UnplacedReason::DuplicateTitle],
        reasons[rv::world::UnplacedReason::NoCover], reasons[rv::world::UnplacedReason::Unavailable], byTitle.size(),
        surveyRegions[SurveyRegion::Usa], surveyRegions[SurveyRegion::Europe], surveyRegions[SurveyRegion::Other],
        readMs, pathsMs, selectMs, covers, coversFound);
    std::cout << fmt::format("Survey's grouping with the region tags read strictly (exactly USA, exactly Europe): {} "
                             "USA, {} Europe only, {} other\n",
                             strictRegions[SurveyRegion::Usa], strictRegions[SurveyRegion::Europe],
                             strictRegions[SurveyRegion::Other]);
    std::cout << "Region tags of the representatives:";
    for (const auto& [tag, count] : regionTags) {
        std::cout << fmt::format(" [{}] {}", tag, count);
    }
    std::cout << "\n";

    // What the title key joins that the display titles keep apart.
    std::size_t joined = 0;
    for (const auto& [key, joinedTitles] : titlesByKey) {
        if (joinedTitles.size() > 1) {
            ++joined;
            std::cout << fmt::format("Title key \"{}\" joins:", key);
            for (const std::string& title : joinedTitles) {
                std::cout << fmt::format(" \"{}\"", title);
            }
            std::cout << "\n";
        }
    }
    // Where the representative's region differs from the survey's best one for its display titles.
    std::size_t regionDifferences = 0;
    for (const rv::world::RoomTitle& title : selection->titles) {
        const RoomEntry& representative = *byId.at(title.game);
        SurveyRegion best = SurveyRegion::Other;
        for (const std::string& displayTitle : titlesByKey[title.titleKey]) {
            for (const RoomEntry* version : byTitle[displayTitle]) {
                best = std::min(best, surveyRegion(*version));
            }
        }
        const SurveyRegion chosen = surveyRegion(representative);
        if (chosen != best) {
            ++regionDifferences;
            std::cout << fmt::format("Representative of \"{}\": {} ({}), the survey's best region {}; its versions:",
                                     title.titleKey, rv::world::fileNameOf(representative.key.relativePath()),
                                     static_cast<int>(chosen), static_cast<int>(best));
            for (const std::string& displayTitle : titlesByKey[title.titleKey]) {
                for (const RoomEntry* version : byTitle[displayTitle]) {
                    std::cout << fmt::format(" \"{}\"", rv::world::fileNameOf(version->key.relativePath()));
                }
            }
            std::cout << "\n";
        }
    }
    std::cout << fmt::format("{} title keys join several display titles; {} representatives of another region than the "
                             "survey's best\n",
                             joined, regionDifferences);
}
