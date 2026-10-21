// world::selectRoom (rv/world/room_selection.hpp, ADR-0010 §3, I3 step 2) beyond the oracle's table
// (test_title_key.cpp): the eligibility rules (include "all", no cover in the system, a single ROM without media), the
// distinct titles over every game with a cover or not (I4 step 4, ADR-0013 §3's count), unknown media deciding
// nothing, the games the room considers (hidden games, missing files, folders that could not be listed), each title's
// priority from its versions, the ties of the version choice, a selection independent of the snapshot's order, and
// the snapshots refused.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/world_config.hpp>

using Catch::Matchers::ContainsSubstring;
using rv::catalog::GameId;
using rv::catalog::RoomEntry;
using rv::world::Eligibility;
using rv::world::RoomSelection;
using rv::world::UnplacedReason;

namespace {

GameId game(std::uint64_t value) {
    return *GameId::fromValue(value);
}

/// A game of the system with a gamelist title, and a cover unless `cover` is off.
RoomEntry entry(std::uint64_t id, std::string_view path, std::string_view title, bool cover = true) {
    RoomEntry made;
    made.id = game(id);
    made.key = *rv::catalog::makeLookupKey(path);
    made.title = std::string(title);
    made.sortKey = *rv::catalog::sortKeyOf(made.title);
    if (cover) {
        made.media[rv::catalog::MediaKind::Cover] = rv::catalog::MediaPresence{true, ".png"};
    }
    return made;
}

rv::world::RoomInput inputOf(std::vector<RoomEntry> entries, bool mediaKnown = true) {
    rv::world::RoomInput input;
    input.library = *rv::catalog::LibraryId::parse("esde");
    input.snapshot.system = *rv::catalog::SystemName::parse("snes");
    input.snapshot.mediaKnown = mediaKnown;
    input.snapshot.mediaUnknownReason = mediaKnown ? "" : "the media root does not exist";
    std::sort(entries.begin(), entries.end(), [](const RoomEntry& a, const RoomEntry& b) { return a.id < b.id; });
    input.snapshot.entries = std::move(entries);
    return input;
}

rv::world::WorldConfig includeAll() {
    rv::world::WorldConfig config;
    config.include = rv::world::PlacementInclude::All;
    return config;
}

std::vector<std::uint64_t> titleIds(const RoomSelection& selection) {
    std::vector<std::uint64_t> ids;
    for (const rv::world::RoomTitle& title : selection.titles) {
        ids.push_back(title.game.value());
    }
    return ids;
}

/// The unplaced games' ids with a reason.
std::vector<std::uint64_t> unplacedIds(const RoomSelection& selection, UnplacedReason reason) {
    std::vector<std::uint64_t> ids;
    for (const rv::world::UnplacedGame& unplaced : selection.unplaced) {
        if (unplaced.reason == reason) {
            ids.push_back(unplaced.game.value());
        }
    }
    return ids;
}

/// A library of covered and uncovered games, a hack among them.
std::vector<RoomEntry> mixedLibrary() {
    return {entry(1, "Pixel Quest (USA).sfc", "Pixel Quest"), entry(2, "Pixel Quest (Europe).sfc", "Pixel Quest"),
            entry(3, "Pixel Quest (USA) [h1].sfc", "Pixel Quest (USA) [h1]", false),
            entry(4, "Star Harbor (Japan).sfc", "Star Harbor (Japan)", false),
            entry(5, "Moon Garden (USA).sfc", "Moon Garden")};
}

} // namespace

TEST_CASE("world/select_room: the covered games, by default", "[world][select_room]") {
    const auto selection = rv::world::selectRoom(inputOf(mixedLibrary()), rv::world::WorldConfig{});
    REQUIRE(selection.has_value());
    CHECK(selection->room.toString() == "room:snes");
    CHECK(selection->eligibility == Eligibility::Covered);
    CHECK(selection->titleKeyRule == "title-key/1");
    CHECK(selection->catalogSortKeyVersion == rv::catalog::kSortKeyVersion);
    CHECK(titleIds(*selection) == std::vector<std::uint64_t>{5, 1});
    CHECK(unplacedIds(*selection, UnplacedReason::NoCover) == std::vector<std::uint64_t>{3, 4});
    CHECK(unplacedIds(*selection, UnplacedReason::DuplicateTitle) == std::vector<std::uint64_t>{2});
    CHECK(selection->unplaced.front().game == game(2));
    CHECK(selection->unplaced.front().representative == game(1));
    CHECK_FALSE(selection->unplaced.back().representative.isValid());
    // Pixel Quest (the hack a version of it), Star Harbor and Moon Garden: the titles under include "all".
    CHECK(selection->distinctTitles == 3);
}

TEST_CASE("world/select_room: the distinct titles count every game with a cover or not", "[world][select_room]") {
    // ADR-0013 §3: the count measures the collection, not the scraping: 1 game with a cover, 40 distinct titles.
    std::vector<RoomEntry> entries{entry(1, "Title 00 (USA).sfc", "Title 00")};
    for (std::uint64_t i = 1; i < 40; ++i) {
        const std::string title = "Title " + std::string(i < 10 ? "0" : "") + std::to_string(i);
        entries.push_back(entry(1 + i, title + " (USA).sfc", title, false));
    }
    entries.push_back(entry(100, "Title 07 (Europe).sfc", "Title 07", false)); // a version: no title more
    RoomEntry missing = entry(101, "Gone (USA).sfc", "Gone", false);              // its file gone: none
    missing.state = rv::catalog::EntryState::MissingFile;
    missing.availability = rv::catalog::Availability::Missing;
    entries.push_back(missing);
    RoomEntry hidden = entry(102, "Secret (USA).sfc", "Secret", false); // hidden as ES-DE hides it: none
    hidden.hidden = true;
    entries.push_back(hidden);
    const auto selection = rv::world::selectRoom(inputOf(entries), rv::world::WorldConfig{});
    REQUIRE(selection.has_value());
    CHECK(selection->eligibility == Eligibility::Covered);
    CHECK(titleIds(*selection) == std::vector<std::uint64_t>{1});
    CHECK(selection->distinctTitles == 40);
    const auto all = rv::world::selectRoom(inputOf(entries), includeAll());
    REQUIRE(all.has_value());
    CHECK(all->titles.size() == 40);
    CHECK(all->distinctTitles == 40);
}

TEST_CASE("world/select_room: include all makes every game eligible", "[world][select_room]") {
    const auto selection = rv::world::selectRoom(inputOf(mixedLibrary()), includeAll());
    REQUIRE(selection.has_value());
    CHECK(selection->eligibility == Eligibility::All);
    // The hack joins its title as a version that loses to the release; the uncovered game is a title of its own.
    CHECK(titleIds(*selection) == std::vector<std::uint64_t>{5, 1, 4});
    CHECK(unplacedIds(*selection, UnplacedReason::DuplicateTitle) == std::vector<std::uint64_t>{2, 3});
    CHECK(unplacedIds(*selection, UnplacedReason::NoCover).empty());
}

TEST_CASE("world/select_room: the small-library rule: no cover in the system makes every game eligible",
          "[world][select_room]") {
    SECTION("a single ROM without media (golden G06's room)") {
        const auto selection =
            rv::world::selectRoom(inputOf({entry(7, "Lone Comet.sfc", "Lone Comet", false)}), rv::world::WorldConfig{});
        REQUIRE(selection.has_value());
        CHECK(selection->eligibility == Eligibility::NoCoverInSystem);
        CHECK(titleIds(*selection) == std::vector<std::uint64_t>{7});
        CHECK(selection->unplaced.empty());
        CHECK(selection->distinctTitles == 1);
    }
    SECTION("several games, none covered: one per title all the same") {
        std::vector<RoomEntry> entries{entry(1, "Pixel Quest (USA).sfc", "Pixel Quest", false),
                                       entry(2, "Pixel Quest (Europe).sfc", "Pixel Quest", false),
                                       entry(3, "Moon Garden (Japan).sfc", "Moon Garden", false)};
        const auto selection = rv::world::selectRoom(inputOf(entries), rv::world::WorldConfig{});
        REQUIRE(selection.has_value());
        CHECK(selection->eligibility == Eligibility::NoCoverInSystem);
        CHECK(titleIds(*selection) == std::vector<std::uint64_t>{3, 1});
        CHECK(unplacedIds(*selection, UnplacedReason::DuplicateTitle) == std::vector<std::uint64_t>{2});
    }
    SECTION("a cover of a missing file counts: the others are uncovered") {
        RoomEntry missing = entry(9, "Gone (USA).sfc", "Gone");
        missing.state = rv::catalog::EntryState::MissingFile;
        missing.availability = rv::catalog::Availability::Missing;
        const auto selection = rv::world::selectRoom(
            inputOf({entry(1, "Lone Comet.sfc", "Lone Comet", false), missing}), rv::world::WorldConfig{});
        REQUIRE(selection.has_value());
        CHECK(selection->eligibility == Eligibility::Covered);
        CHECK(selection->titles.empty());
        CHECK(unplacedIds(*selection, UnplacedReason::NoCover) == std::vector<std::uint64_t>{1});
        CHECK(unplacedIds(*selection, UnplacedReason::Unavailable) == std::vector<std::uint64_t>{9});
    }
    SECTION("a cover of a hidden game does not count") {
        RoomEntry hidden = entry(9, "Secret (USA).sfc", "Secret");
        hidden.hidden = true;
        const auto selection = rv::world::selectRoom(inputOf({entry(1, "Lone Comet.sfc", "Lone Comet", false), hidden}),
                                                     rv::world::WorldConfig{});
        REQUIRE(selection.has_value());
        CHECK(selection->eligibility == Eligibility::NoCoverInSystem);
        CHECK(titleIds(*selection) == std::vector<std::uint64_t>{1});
        CHECK(selection->unplaced.empty());
    }
    SECTION("an empty system") {
        const auto selection = rv::world::selectRoom(inputOf({}), rv::world::WorldConfig{});
        REQUIRE(selection.has_value());
        CHECK(selection->eligibility == Eligibility::NoCoverInSystem);
        CHECK(selection->titles.empty());
        CHECK(selection->unplaced.empty());
        CHECK(selection->distinctTitles == 0);
    }
}

TEST_CASE("world/select_room: unknown media decide nothing", "[world][select_room]") {
    // Whatever the entries hold (none of them covered here, which known media would make NoCoverInSystem) and whatever
    // the configuration: no selection, the reason returned.
    for (const bool all : {false, true}) {
        const auto selection = rv::world::selectRoom(inputOf({entry(1, "Lone Comet.sfc", "Lone Comet", false)}, false),
                                                     all ? includeAll() : rv::world::WorldConfig{});
        REQUIRE_FALSE(selection.has_value());
        CHECK(selection.error().code == rv::ErrorCode::Unavailable);
        CHECK_THAT(selection.error().message, ContainsSubstring("the media root does not exist"));
        CHECK_THAT(selection.error().message, ContainsSubstring("decides nothing"));
        CHECK(selection.error().context == "snes");
    }
    rv::world::RoomInput noReason = inputOf({}, false);
    noReason.snapshot.mediaUnknownReason.clear();
    const auto selection = rv::world::selectRoom(noReason, rv::world::WorldConfig{});
    REQUIRE_FALSE(selection.has_value());
    CHECK_THAT(selection.error().message, ContainsSubstring("no reason given"));
}

TEST_CASE("world/select_room: the games the room considers", "[world][select_room]") {
    RoomEntry hidden = entry(2, "Hidden (USA).sfc", "Hidden");
    hidden.hidden = true;
    RoomEntry missing = entry(3, "Missing (USA).sfc", "Missing");
    missing.state = rv::catalog::EntryState::MissingFile;
    missing.availability = rv::catalog::Availability::Missing;
    RoomEntry unlisted = entry(4, "Unlisted (USA).sfc", "Unlisted");
    unlisted.availability = rv::catalog::Availability::SourceUnavailable;
    RoomEntry notAGame = entry(5, "Readme.txt", "Readme");
    notAGame.state = rv::catalog::EntryState::NotAGame;
    RoomEntry folder = entry(6, "Collection", "Collection");
    folder.state = rv::catalog::EntryState::Folder;
    folder.directory = true;
    RoomEntry member = entry(7, "Disc Game (Track 2).bin", "Disc Game (Track 2)");
    member.state = rv::catalog::EntryState::DiscMember;
    RoomEntry hiddenMissing = missing;
    hiddenMissing.id = game(8);
    hiddenMissing.key = *rv::catalog::makeLookupKey("Hidden Missing (USA).sfc");
    hiddenMissing.hidden = true;
    const auto selection = rv::world::selectRoom(inputOf({entry(1, "Lone Comet (USA).sfc", "Lone Comet"), hidden,
                                                          missing, unlisted, notAGame, folder, member, hiddenMissing}),
                                                 rv::world::WorldConfig{});
    REQUIRE(selection.has_value());
    // A game whose folder could not be listed is not known to be gone: selected.
    CHECK(titleIds(*selection) == std::vector<std::uint64_t>{1, 4});
    CHECK(unplacedIds(*selection, UnplacedReason::Unavailable) == std::vector<std::uint64_t>{3});
    // Hidden games and entries that are no games are not listed at all.
    CHECK(selection->unplaced.size() == 1);
}

TEST_CASE("world/select_room: a title's priority comes from its versions", "[world][select_room]") {
    RoomEntry usa = entry(1, "Pixel Quest (USA).sfc", "Pixel Quest");
    usa.esdePlayCount = 2;
    usa.rating = rv::catalog::Rating{6};
    RoomEntry europe = entry(2, "Pixel Quest (Europe).sfc", "Pixel Quest");
    europe.esdePlayCount = 3;
    europe.sessionCount = 4;
    europe.favorite = true;
    europe.rating = rv::catalog::Rating{9};
    RoomEntry uncovered = entry(3, "Pixel Quest (Japan).sfc", "Pixel Quest", false);
    uncovered.esdePlayCount = 100; // not eligible: counts for nothing
    uncovered.favorite = true;
    RoomEntry other = entry(4, "Moon Garden (USA).sfc", "Moon Garden");
    other.esdePlayCount = 8;
    const auto selection = rv::world::selectRoom(inputOf({usa, europe, uncovered, other}), rv::world::WorldConfig{});
    REQUIRE(selection.has_value());
    REQUIRE(selection->titles.size() == 2);
    const rv::world::RoomTitle& title = selection->titles.front();
    CHECK(title.game == game(1));
    CHECK(title.favorite);
    CHECK(title.playCount == 9);
    CHECK(title.ratingTenths == 9);
    CHECK(title.sortKey == "pixel quest");
    CHECK(selection->titles.back().playCount == 8);
    CHECK_FALSE(selection->titles.back().ratingTenths.has_value());
}

TEST_CASE("world/select_room: the ties of the version choice", "[world][select_room]") {
    // Equal tags: the lookup key decides (ordinal), then, for names that fold to one key, the relative path.
    const auto byKey = rv::world::selectRoom(
        inputOf({entry(1, "Pixel Quest (World).sfc", "Pixel Quest"), entry(2, "Pixel Quest (USA).sfc", "Pixel Quest")}),
        rv::world::WorldConfig{});
    REQUIRE(byKey.has_value());
    CHECK(titleIds(*byKey) == std::vector<std::uint64_t>{2});
    const auto byPath = rv::world::selectRoom(
        inputOf({entry(1, "pixel quest (USA).sfc", "Pixel Quest"), entry(2, "PIXEL QUEST (USA).sfc", "Pixel Quest")}),
        rv::world::WorldConfig{});
    REQUIRE(byPath.has_value());
    CHECK(titleIds(*byPath) == std::vector<std::uint64_t>{2});
    // The folder a game lies in counts for the key, not for the tags: the name's own tags decide.
    const auto nested = rv::world::selectRoom(inputOf({entry(1, "Europe (USA)/Pixel Quest (Europe).sfc", "Pixel Quest"),
                                                       entry(2, "Japan/Pixel Quest (USA).sfc", "Pixel Quest")}),
                                              rv::world::WorldConfig{});
    REQUIRE(nested.has_value());
    CHECK(titleIds(*nested) == std::vector<std::uint64_t>{2});
}

TEST_CASE("world/select_room: the selection does not depend on the snapshot's order", "[world][select_room]") {
    std::vector<RoomEntry> entries = mixedLibrary();
    for (std::uint64_t i = 10; i < 40; ++i) {
        entries.push_back(entry(i, "Game " + std::to_string(i % 7) + (i % 2 == 0 ? " (USA).sfc" : " (Europe).sfc"),
                                "Game " + std::to_string(i % 7), i % 3 != 0));
    }
    const rv::world::RoomInput input = inputOf(entries);
    const auto expected = rv::world::selectRoom(input, rv::world::WorldConfig{});
    REQUIRE(expected.has_value());
    rv::world::RoomInput reversed = input;
    std::reverse(reversed.snapshot.entries.begin(), reversed.snapshot.entries.end());
    const auto again = rv::world::selectRoom(reversed, rv::world::WorldConfig{});
    REQUIRE(again.has_value());
    CHECK(*again == *expected);
    CHECK(rv::world::writeRoomSelection(*again).value() == rv::world::writeRoomSelection(*expected).value());
}

TEST_CASE("world/select_room: snapshots refused", "[world][select_room]") {
    SECTION("a game twice") {
        rv::world::RoomInput input = inputOf({entry(1, "A.sfc", "A")});
        input.snapshot.entries.push_back(entry(1, "B.sfc", "B"));
        const auto selection = rv::world::selectRoom(input, rv::world::WorldConfig{});
        REQUIRE_FALSE(selection.has_value());
        CHECK(selection.error().code == rv::ErrorCode::InvalidArgument);
        CHECK_THAT(selection.error().message, ContainsSubstring("g0000000000000001 twice"));
    }
    SECTION("no system") {
        rv::world::RoomInput input = inputOf({});
        input.snapshot.system = rv::catalog::SystemName();
        const auto selection = rv::world::selectRoom(input, rv::world::WorldConfig{});
        REQUIRE_FALSE(selection.has_value());
        CHECK(selection.error().code == rv::ErrorCode::InvalidArgument);
    }
    SECTION("a title that is not UTF-8") {
        RoomEntry bad = entry(3, "Bad.sfc", "Bad");
        bad.title = "Bad \xFF title";
        const auto selection = rv::world::selectRoom(inputOf({bad}), rv::world::WorldConfig{});
        REQUIRE_FALSE(selection.has_value());
        CHECK(selection.error().code == rv::ErrorCode::InvalidArgument);
        CHECK_THAT(selection.error().message, ContainsSubstring("g0000000000000003"));
    }
}
