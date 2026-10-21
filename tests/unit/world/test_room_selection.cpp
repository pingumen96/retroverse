// RoomSelection (rv/world/room_selection.hpp, ADR-0010 §3): the priority order, and the canonical form the generator's
// input digest hashes, read back exactly and refused when the titles are out of priority order.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include <rv/world/generation.hpp>
#include <rv/world/room_input.hpp>
#include <rv/world/room_selection.hpp>

using Catch::Matchers::ContainsSubstring;
using rv::catalog::GameId;
using rv::world::Eligibility;
using rv::world::RoomSelection;
using rv::world::RoomTitle;
using rv::world::UnplacedReason;

namespace {

GameId game(std::uint64_t value) {
    return *GameId::fromValue(value);
}

rv::world::RoomId snesRoom() {
    return *rv::world::RoomId::forSystem(*rv::catalog::SystemName::parse("snes"));
}

RoomTitle title(std::uint64_t id, std::string sortKey, bool favorite, std::uint64_t plays, std::optional<int> rating) {
    return RoomTitle{game(id), sortKey, std::move(sortKey), favorite, plays, rating};
}

RoomSelection sampleSelection() {
    RoomSelection selection;
    selection.room = snesRoom();
    selection.eligibility = Eligibility::Covered;
    selection.titleKeyRule = "title-key/1";
    selection.catalogSortKeyVersion = 1;
    selection.titles = {title(5, "zelda", true, 3, 9), title(6, "mario", false, 40, std::nullopt),
                        title(7, "aero", false, 2, 10), title(8, "basket", false, 2, std::nullopt),
                        title(9, "basket", false, 2, std::nullopt)};
    selection.unplaced = {{game(12), snesRoom(), UnplacedReason::NoCover, {}},
                          {game(11), snesRoom(), UnplacedReason::DuplicateTitle, game(7)}};
    // The five titles and the game without a cover, a title of its own.
    selection.distinctTitles = 6;
    return selection;
}

} // namespace

TEST_CASE("world/room_selection: the priority order", "[world][room_selection]") {
    std::vector<RoomTitle> titles = sampleSelection().titles;
    std::reverse(titles.begin(), titles.end());
    std::sort(titles.begin(), titles.end(), rv::world::priorityLess);
    std::vector<std::uint64_t> order;
    for (const RoomTitle& entry : titles) {
        order.push_back(entry.game.value());
    }
    // The favourite, then the most played, then the rated one, then by sort key and GameId.
    CHECK(order == std::vector<std::uint64_t>{5, 6, 7, 8, 9});
    // A strict order: nothing comes before itself.
    for (const RoomTitle& entry : titles) {
        CHECK_FALSE(rv::world::priorityLess(entry, entry));
    }
    CHECK(rv::world::priorityLess(title(1, "b", false, 0, 0), title(2, "a", false, 0, std::nullopt)));
}

TEST_CASE("world/room_selection: the canonical form round-trips", "[world][room_selection]") {
    const RoomSelection selection = sampleSelection();
    auto text = rv::world::writeRoomSelection(selection);
    REQUIRE(text.has_value());
    CHECK(*text ==
          "{\"catalogSortKeyVersion\":1,\"distinctTitles\":6,\"eligibility\":\"covered\",\"room\":\"room:snes\","
          "\"titleKeyRule\":\"title-key/1\",\"titles\":[\n"
          "{\"favorite\":true,\"game\":\"g0000000000000005\",\"playCount\":3,\"rating\":9,\"sortKey\":\"zelda\","
          "\"titleKey\":\"zelda\"},\n"
          "{\"favorite\":false,\"game\":\"g0000000000000006\",\"playCount\":40,\"rating\":null,\"sortKey\":\"mario\","
          "\"titleKey\":\"mario\"},\n"
          "{\"favorite\":false,\"game\":\"g0000000000000007\",\"playCount\":2,\"rating\":10,\"sortKey\":\"aero\","
          "\"titleKey\":\"aero\"},\n"
          "{\"favorite\":false,\"game\":\"g0000000000000008\",\"playCount\":2,\"rating\":null,\"sortKey\":\"basket\","
          "\"titleKey\":\"basket\"},\n"
          "{\"favorite\":false,\"game\":\"g0000000000000009\",\"playCount\":2,\"rating\":null,\"sortKey\":\"basket\","
          "\"titleKey\":\"basket\"}\n"
          "],\"unplaced\":[\n"
          "{\"game\":\"g000000000000000b\",\"reason\":\"duplicate-title\",\"representative\":\"g0000000000000007\","
          "\"room\":\"room:snes\"},\n"
          "{\"game\":\"g000000000000000c\",\"reason\":\"no-cover\",\"room\":\"room:snes\"}\n"
          "]}\n");
    auto read = rv::world::readRoomSelection(*text, "selection.json");
    REQUIRE(read.has_value());
    RoomSelection expected = selection;
    std::sort(expected.unplaced.begin(), expected.unplaced.end());
    CHECK(*read == expected);
    CHECK(*rv::world::writeRoomSelection(*read) == *text);
}

TEST_CASE("world/room_selection: a selection out of priority order is refused", "[world][room_selection]") {
    RoomSelection swapped = sampleSelection();
    std::swap(swapped.titles[0], swapped.titles[1]);
    auto refused = rv::world::writeRoomSelection(swapped);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::InvalidArgument);
    CHECK_THAT(refused.error().message, ContainsSubstring("/titles/1: out of order"));

    RoomSelection twice = sampleSelection();
    twice.titles.push_back(twice.titles.back());
    CHECK_THAT(rv::world::writeRoomSelection(twice).error().message,
               ContainsSubstring("/titles/5: the game appears twice"));

    RoomSelection otherRoom = sampleSelection();
    otherRoom.unplaced.front().room = *rv::world::RoomId::forSystem(*rv::catalog::SystemName::parse("nes"));
    CHECK_THAT(rv::world::writeRoomSelection(otherRoom).error().message, ContainsSubstring("of another room"));

    RoomSelection badRating = sampleSelection();
    badRating.titles.front().ratingTenths = 11;
    CHECK_THAT(rv::world::writeRoomSelection(badRating).error().message, ContainsSubstring("/titles/0/rating"));

    // I4 step 4: the distinct titles over all the games are at least the titles.
    RoomSelection fewer = sampleSelection();
    fewer.distinctTitles = 4;
    CHECK_THAT(rv::world::writeRoomSelection(fewer).error().message,
               ContainsSubstring("/distinctTitles: fewer distinct titles (4) than titles (5)"));

    auto text = *rv::world::writeRoomSelection(sampleSelection());
    const std::size_t at = text.find("\"eligibility\":\"covered\"");
    text.replace(at, 23, "\"eligibility\":\"usable\"");
    auto unread = rv::world::readRoomSelection(text, "selection.json");
    REQUIRE_FALSE(unread.has_value());
    CHECK(unread.error().context == "selection.json:1");
    CHECK_THAT(unread.error().message, ContainsSubstring("/eligibility: expected one of"));
}

TEST_CASE("world/room_selection: the result types", "[world][room_selection]") {
    for (const Eligibility eligibility : {Eligibility::Covered, Eligibility::All, Eligibility::NoCoverInSystem}) {
        CHECK(rv::world::parseEligibility(rv::world::toString(eligibility)) == eligibility);
    }
    for (const auto level : {rv::world::RebuildLevel::NoRebuild, rv::world::RebuildLevel::LocalReflow,
                             rv::world::RebuildLevel::RoomExpansion, rv::world::RebuildLevel::Full}) {
        CHECK(rv::world::parseRebuildLevel(rv::world::toString(level)) == level);
    }
    CHECK(rv::world::parseGenerationOutcome("requires-explicit-regeneration") ==
          rv::world::GenerationOutcome::RequiresExplicitRegeneration);
    rv::world::ValidationReport report;
    CHECK(report.passed());
    report.findings.push_back({"main-passage", rv::world::Severity::Soft, "", "1.3 m wide"});
    CHECK(report.passed());
    report.findings.push_back({"footprints-overlap", rv::world::Severity::Hard, "room:snes/fx:w01", "overlaps"});
    CHECK_FALSE(report.passed());

    // A room input is the catalog's snapshot with its library.
    rv::world::RoomInput input;
    input.snapshot.system = *rv::catalog::SystemName::parse("snes");
    CHECK_FALSE(input.snapshot.mediaKnown);
    CHECK(input.snapshot.sortKeyVersion == rv::catalog::kSortKeyVersion);
    CHECK(input.library.str() == "esde");
}
