// Title keys, version tags and the version choice of the room selection (rv/world/title_key.hpp, room_selection.hpp;
// ADR-0010 §3, I3 step 2), against an independent oracle: the tables below are the output of
// `python -I scripts/title_key_oracle.py` (Python, nothing shared with rv_world), pasted as printed, and
// `python -I scripts/title_key_oracle.py --check tests/unit/world/test_title_key.cpp` must pass. The selection table
// holds the roadmap's 40 invented file names (No-Intro and GoodTools tags, revisions, betas, demos, " : ", ": ", " - "
// and " – " subtitles, a portrait cover, unscraped games titled from their files) and four entries of other states (a
// missing file, a hidden game, a file that is no game, a folder); its whole selection, the priority order included, is
// the golden tests/fixtures/world/selection/table.json.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/world/room_selection.hpp>
#include <rv/world/title_key.hpp>
#include <rv/world/world_config.hpp>

#include "selection_golden.hpp"

using rv::catalog::GameId;
using rv::world::RegionRank;

namespace {

/// A row of the selection table: the entry's relative path, its gamelist title ("" for an unscraped game, titled from
/// its file), its cover's extension ("" for none), its state ("game", "missing", "not-a-game", "folder"), ES-DE's
/// hidden flag, favourite and play count, Retroverse's sessions, the rating in tenths (-1 for none). GameIds: the index
/// + 1.
struct Row {
    const char* path;
    const char* title;
    const char* cover;
    const char* state;
    bool hidden;
    bool favorite;
    std::uint64_t plays;
    std::uint64_t sessions;
    int rating;
};

/// What the oracle says of a row: its title key, its file name's tags, its outcome ("title", "duplicate-title" with the
/// representative's row, "no-cover", "unavailable", "none" for an entry that is no game the room considers).
struct Outcome {
    int row;
    const char* titleKey;
    RegionRank region;
    bool status;
    std::uint32_t revision;
    const char* outcome;
    int representative;
};

struct KeyCase {
    const char* title;
    const char* key;
};

struct TagCase {
    const char* name;
    RegionRank region;
    bool status;
    std::uint32_t revision;
};

// clang-format off
// Selection rows: path, title, cover, state, hidden, favourite, ES-DE plays, sessions, rating.
constexpr Row kRows[] = {
    {"Pixel Quest (USA).sfc", "Pixel Quest", ".png", "game", false, false, 0, 0, -1},
    {"Pixel Quest (Europe).sfc", "Pixel Quest", ".png", "game", false, true, 0, 0, -1},
    {"Pixel Quest (Japan).sfc", "Pixel Quest", "", "game", false, false, 0, 0, -1},
    {"Star Harbor (Europe).sfc", "Star Harbor", ".png", "game", false, false, 0, 0, -1},
    {"Star Harbor (Europe) (Rev 1).sfc", "Star Harbor", ".png", "game", false, false, 0, 0, -1},
    {"Thunder Lanes (USA, Europe).sfc", "Thunder Lanes", ".png", "game", false, false, 0, 0, -1},
    {"Thunder Lanes (Japan).sfc", "Thunder Lanes", ".png", "game", false, false, 7, 0, -1},
    {"Moon Garden (Japan, USA).sfc", "Moon Garden", ".png", "game", false, false, 0, 0, -1},
    {"Moon Garden (Europe).sfc", "Moon Garden", ".png", "game", false, false, 0, 0, -1},
    {"Crystal Depths (World).sfc", "Crystal Depths", ".png", "game", false, false, 0, 0, -1},
    {"Crystal Depths (USA).sfc", "Crystal Depths", ".png", "game", false, false, 0, 0, -1},
    {"River Run (USA) (Beta).sfc", "River Run", ".png", "game", false, false, 0, 0, -1},
    {"River Run (Japan).sfc", "River Run", ".png", "game", false, false, 0, 0, -1},
    {"Glass Tower (Europe) (Proto).sfc", "Glass Tower", ".png", "game", false, false, 0, 0, 10},
    {"Glass Tower (Europe).sfc", "Glass Tower", ".png", "game", false, false, 0, 0, 9},
    {"Bolt Brigade (U) [!].smc", "Bolt Brigade", ".png", "game", false, false, 0, 0, -1},
    {"Bolt Brigade (U) [h1C].smc", "Bolt Brigade", ".png", "game", false, false, 0, 0, -1},
    {"Bolt Brigade (E).smc", "Bolt Brigade", ".png", "game", false, false, 0, 0, -1},
    {"Iron Sky (JU).smc", "Iron Sky", ".png", "game", false, false, 0, 0, -1},
    {"Iron Sky (UE) [a1].smc", "Iron Sky", ".png", "game", false, false, 0, 0, -1},
    {"Lucky Lagoon (UK).smc", "Lucky Lagoon", ".png", "game", false, false, 0, 0, 8},
    {"Dungeon Story - The Lost Key (USA).sfc", "Dungeon Story: The Lost Key", ".png", "game", false, false, 0, 0, -1},
    {"Dungeon Story - The Lost Key (Europe).sfc", "Dungeon Story - The Lost Key", ".png", "game", false, false, 0, 0, -1},
    {"Neon Drift - Turbo (Europe).sfc", "Neon Drift : Turbo", ".png", "game", false, false, 0, 0, -1},
    {"Neon Drift - Turbo (Europe) (Rev 1).sfc", "Neon Drift: Turbo", ".png", "game", false, false, 0, 0, -1},
    {"Sky Pirates - Gold (Japan).sfc", "Sky Pirates \xE2\x80\x93 Gold", ".png", "game", false, false, 0, 0, -1},
    {"Sky Pirates - Gold (Asia).sfc", "Sky Pirates - Gold", ".jpg", "game", false, false, 0, 0, -1},
    {"SUPER BLOCKS (USA).sfc", "SUPER BLOCKS", ".png", "game", false, false, 0, 0, -1},
    {"Super Blocks (Europe).sfc", "Super Blocks", ".png", "game", false, false, 0, 0, -1},
    {"Ace Racer (USA).sfc", "Ace Racer", ".png", "game", false, false, 0, 0, -1},
    {"Ace Racer (USA) (Rev 1).sfc", "Ace Racer (Rev 1)", ".png", "game", false, false, 0, 0, -1},
    {"Ace Racer (USA) (Rev 2).sfc", "", "", "game", false, false, 0, 0, -1},
    {"Brass Engine (Japan) (Rev 2).sfc", "Brass Engine", ".png", "game", false, false, 0, 0, -1},
    {"Brass Engine (Japan) (Rev 10).sfc", "Brass Engine", ".png", "game", false, false, 0, 3, -1},
    {"Quiet Forest (Europe).sfc", "", ".png", "game", false, false, 0, 0, -1},
    {"Quiet Forest (USA) (Demo).sfc", "Quiet Forest", ".png", "game", false, false, 0, 0, -1},
    {"Hidden Fort (Pirate).sfc", "Hidden Fort", ".png", "game", false, false, 0, 0, -1},
    {"Hidden Fort (Asia).sfc", "Hidden Fort", ".png", "game", false, false, 0, 0, -1},
    {"Echo Valley (Europe) (Sample) (Rev A).sfc", "Echo Valley", ".png", "game", false, false, 0, 0, -1},
    {"Fable Hill (Rev 1) (USA) (En,Fr).sfc", "Fable Hill", ".png", "game", false, false, 0, 0, -1},
    {"Mirror Maze (Europe) (En,Fr,De).sfc", "Mirror Maze", ".png", "missing", false, false, 0, 0, -1},
    {"Gone Hiking (USA).sfc", "Gone Hiking", ".png", "game", true, false, 0, 0, -1},
    {"Pixel Quest (USA).txt", "", ".png", "not-a-game", false, false, 0, 0, -1},
    {"Pixel Collection", "Pixel Collection", ".png", "folder", false, false, 0, 0, -1},
};

// Selection outcomes: row, title key, region rank, status, revision, outcome, representative.
constexpr Outcome kOutcomes[] = {
    {0, "pixel quest", RegionRank::UsaOrWorld, false, 0, "title", -1},
    {1, "pixel quest", RegionRank::Europe, false, 0, "duplicate-title", 0},
    {2, "pixel quest", RegionRank::Other, false, 0, "no-cover", -1},
    {3, "star harbor", RegionRank::Europe, false, 0, "duplicate-title", 4},
    {4, "star harbor", RegionRank::Europe, false, 1, "title", -1},
    {5, "thunder lanes", RegionRank::UsaOrWorld, false, 0, "title", -1},
    {6, "thunder lanes", RegionRank::Other, false, 0, "duplicate-title", 5},
    {7, "moon garden", RegionRank::UsaOrWorld, false, 0, "title", -1},
    {8, "moon garden", RegionRank::Europe, false, 0, "duplicate-title", 7},
    {9, "crystal depths", RegionRank::UsaOrWorld, false, 0, "duplicate-title", 10},
    {10, "crystal depths", RegionRank::UsaOrWorld, false, 0, "title", -1},
    {11, "river run", RegionRank::UsaOrWorld, true, 0, "title", -1},
    {12, "river run", RegionRank::Other, false, 0, "duplicate-title", 11},
    {13, "glass tower", RegionRank::Europe, true, 0, "duplicate-title", 14},
    {14, "glass tower", RegionRank::Europe, false, 0, "title", -1},
    {15, "bolt brigade", RegionRank::UsaOrWorld, false, 0, "title", -1},
    {16, "bolt brigade", RegionRank::UsaOrWorld, true, 0, "duplicate-title", 15},
    {17, "bolt brigade", RegionRank::Europe, false, 0, "duplicate-title", 15},
    {18, "iron sky", RegionRank::UsaOrWorld, false, 0, "title", -1},
    {19, "iron sky", RegionRank::UsaOrWorld, true, 0, "duplicate-title", 18},
    {20, "lucky lagoon", RegionRank::Other, false, 0, "title", -1},
    {21, "dungeon story: the lost key", RegionRank::UsaOrWorld, false, 0, "title", -1},
    {22, "dungeon story: the lost key", RegionRank::Europe, false, 0, "duplicate-title", 21},
    {23, "neon drift: turbo", RegionRank::Europe, false, 0, "duplicate-title", 24},
    {24, "neon drift: turbo", RegionRank::Europe, false, 1, "title", -1},
    {25, "sky pirates: gold", RegionRank::Other, false, 0, "duplicate-title", 26},
    {26, "sky pirates: gold", RegionRank::Other, false, 0, "title", -1},
    {27, "super blocks", RegionRank::UsaOrWorld, false, 0, "title", -1},
    {28, "super blocks", RegionRank::Europe, false, 0, "duplicate-title", 27},
    {29, "ace racer", RegionRank::UsaOrWorld, false, 0, "duplicate-title", 30},
    {30, "ace racer", RegionRank::UsaOrWorld, false, 1, "title", -1},
    {31, "ace racer", RegionRank::UsaOrWorld, false, 2, "no-cover", -1},
    {32, "brass engine", RegionRank::Other, false, 2, "duplicate-title", 33},
    {33, "brass engine", RegionRank::Other, false, 10, "title", -1},
    {34, "quiet forest", RegionRank::Europe, false, 0, "duplicate-title", 35},
    {35, "quiet forest", RegionRank::UsaOrWorld, true, 0, "title", -1},
    {36, "hidden fort", RegionRank::Other, true, 0, "duplicate-title", 37},
    {37, "hidden fort", RegionRank::Other, false, 0, "title", -1},
    {38, "echo valley", RegionRank::Europe, true, 0, "title", -1},
    {39, "fable hill", RegionRank::UsaOrWorld, false, 1, "title", -1},
    {40, "mirror maze", RegionRank::Europe, false, 0, "unavailable", -1},
    {41, "gone hiking", RegionRank::UsaOrWorld, false, 0, "none", -1},
    {42, "pixel quest", RegionRank::UsaOrWorld, false, 0, "none", -1},
    {43, "pixel collection", RegionRank::Other, false, 0, "none", -1},
};

// Title keys: title, key.
constexpr KeyCase kKeys[] = {
    {"Pixel Quest", "pixel quest"},
    {"  Pixel   Quest  ", "pixel quest"},
    {"Pixel\x09Quest\x0A", "pixel quest"},
    {"Pixel Quest (USA)", "pixel quest"},
    {"Pixel Quest [!]", "pixel quest"},
    {"Pixel Quest (Hack (v2)) Extra", "pixel quest extra"},
    {"Pixel Quest (a [b) c]", "pixel quest c]"},
    {"Pixel Quest [a (b] c)", "pixel quest c)"},
    {"Pixel Quest (unbalanced", "pixel quest (unbalanced"},
    {"Pixel Quest ) stray", "pixel quest ) stray"},
    {"Pixel Quest ]] stray [", "pixel quest ]] stray ["},
    {"(Untitled)", "(untitled)"},
    {"[Untitled] (Beta)", "[untitled] (beta)"},
    {"Dungeon Story: The Lost Key", "dungeon story: the lost key"},
    {"Dungeon Story - The Lost Key", "dungeon story: the lost key"},
    {"Dungeon Story : The Lost Key", "dungeon story: the lost key"},
    {"Dungeon Story \xE2\x80\x93 The Lost Key", "dungeon story: the lost key"},
    {"Dungeon Story:The Lost Key", "dungeon story:the lost key"},
    {"Dungeon Story-The Lost Key", "dungeon story-the lost key"},
    {"Quest - Part : Two", "quest: part: two"},
    {"Quest  -  Part", "quest: part"},
    {"Quest -", "quest -"},
    {"Quest (Disc 1) - Part Two", "quest: part two"},
    {"Spider-Man 2", "spider-man 2"},
    {"Quest II", "quest ii"},
    {"Quest 2", "quest 2"},
    {"Caf\xC3\xA9 Racer", "caf\xC3\xA9 racer"},
    {"CAF\xC3\x89 RACER", "caf\xC3\xA9 racer"},
};

// Version tags: file name, region rank, status, revision.
constexpr TagCase kTags[] = {
    {"Pixel Quest.sfc", RegionRank::Other, false, 0},
    {"Pixel Quest (USA).sfc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (World).sfc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (Europe).sfc", RegionRank::Europe, false, 0},
    {"Pixel Quest (Japan).sfc", RegionRank::Other, false, 0},
    {"Pixel Quest (USA, Europe).sfc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (Europe, USA).sfc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (Japan, Europe).sfc", RegionRank::Europe, false, 0},
    {"Pixel Quest (Japan, Korea).sfc", RegionRank::Other, false, 0},
    {"Pixel Quest (USA,Europe).sfc", RegionRank::Other, false, 0},
    {"Pixel Quest (En,Fr,De) (Europe).sfc", RegionRank::Europe, false, 0},
    {"Pixel Quest (Europe) (USA).sfc", RegionRank::Europe, false, 0},
    {"Pixel Quest (U).smc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (E).smc", RegionRank::Europe, false, 0},
    {"Pixel Quest (J).smc", RegionRank::Other, false, 0},
    {"Pixel Quest (W).smc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (4).smc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (JU).smc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (UE).smc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (JE).smc", RegionRank::Europe, false, 0},
    {"Pixel Quest (JUE).smc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (UU).smc", RegionRank::Other, false, 0},
    {"Pixel Quest (UK).smc", RegionRank::Other, false, 0},
    {"Pixel Quest (Unl).smc", RegionRank::Other, false, 0},
    {"Pixel Quest (PD).smc", RegionRank::Other, false, 0},
    {"Pixel Quest (u).smc", RegionRank::Other, false, 0},
    {"Pixel Quest (USA) (Beta).sfc", RegionRank::UsaOrWorld, true, 0},
    {"Pixel Quest (USA) (Beta 2).sfc", RegionRank::UsaOrWorld, true, 0},
    {"Pixel Quest (USA) (Proto 1).sfc", RegionRank::UsaOrWorld, true, 0},
    {"Pixel Quest (USA) (Demo, Kiosk).sfc", RegionRank::UsaOrWorld, true, 0},
    {"Pixel Quest (USA) (Kiosk Demo).sfc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (Sample).sfc", RegionRank::Other, true, 0},
    {"Pixel Quest (Pirate).sfc", RegionRank::Other, true, 0},
    {"Pixel Quest (Betamax).sfc", RegionRank::Other, false, 0},
    {"Pixel Quest (U) [h].smc", RegionRank::UsaOrWorld, true, 0},
    {"Pixel Quest (U) [h1C].smc", RegionRank::UsaOrWorld, true, 0},
    {"Pixel Quest (U) [hI].smc", RegionRank::UsaOrWorld, true, 0},
    {"Pixel Quest (U) [b1].smc", RegionRank::UsaOrWorld, true, 0},
    {"Pixel Quest (U) [a2].smc", RegionRank::UsaOrWorld, true, 0},
    {"Pixel Quest (U) [!].smc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (U) [T+Eng].smc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (U) [hack].smc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (USA) (Rev 1).sfc", RegionRank::UsaOrWorld, false, 1},
    {"Pixel Quest (USA) (Rev 2) (Rev 10).sfc", RegionRank::UsaOrWorld, false, 10},
    {"Pixel Quest (USA) (Rev A).sfc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (USA) (Rev 1, Beta).sfc", RegionRank::UsaOrWorld, true, 1},
    {"Pixel Quest (USA) (Rev1).sfc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (USA) (V1.1).sfc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest (USA) (Rev 1234567890).sfc", RegionRank::UsaOrWorld, false, 0},
    {"Pixel Quest ((USA)).sfc", RegionRank::Other, false, 0},
    {"Pixel Quest (USA", RegionRank::Other, false, 0},
    {"Pixel Quest [USA] (Europe).sfc", RegionRank::Europe, false, 0},
};
// clang-format on

GameId idOfRow(std::size_t row) {
    return *GameId::fromValue(row + 1);
}

rv::catalog::EntryState stateOf(std::string_view state) {
    return state == "missing"      ? rv::catalog::EntryState::MissingFile
           : state == "not-a-game" ? rv::catalog::EntryState::NotAGame
           : state == "folder"     ? rv::catalog::EntryState::Folder
                                   : rv::catalog::EntryState::Game;
}

/// The title the catalog gives a row: its gamelist title, else the title of its file name.
std::string titleOf(const Row& row) {
    const bool directory = std::string_view(row.state) == "folder";
    return *row.title != '\0' ? std::string(row.title) : rv::catalog::titleFromFileStem(row.path, directory);
}

/// The table as the room's snapshot of a system "snes" whose media are known, entries by GameId.
rv::world::RoomInput tableInput() {
    rv::world::RoomInput input;
    input.library = *rv::catalog::LibraryId::parse("esde");
    rv::catalog::RoomSnapshot& snapshot = input.snapshot;
    snapshot.system = *rv::catalog::SystemName::parse("snes");
    snapshot.mediaKnown = true;
    for (std::size_t i = 0; i < std::size(kRows); ++i) {
        const Row& row = kRows[i];
        rv::catalog::RoomEntry entry;
        entry.id = idOfRow(i);
        entry.key = *rv::catalog::makeLookupKey(row.path);
        entry.state = stateOf(row.state);
        entry.directory = entry.state == rv::catalog::EntryState::Folder;
        entry.availability = entry.state == rv::catalog::EntryState::MissingFile ? rv::catalog::Availability::Missing
                                                                                 : rv::catalog::Availability::Available;
        entry.title = titleOf(row);
        entry.sortKey = *rv::catalog::sortKeyOf(entry.title);
        entry.hidden = row.hidden;
        entry.favorite = row.favorite;
        entry.esdePlayCount = row.plays;
        entry.sessionCount = row.sessions;
        if (row.rating >= 0) {
            entry.rating = rv::catalog::Rating{row.rating};
        }
        if (*row.cover != '\0') {
            entry.media[rv::catalog::MediaKind::Cover] = rv::catalog::MediaPresence{true, row.cover};
        }
        snapshot.entries.push_back(std::move(entry));
    }
    return input;
}

/// Each game's outcome in a selection, as the oracle names it, with its representative ("none" when not listed).
std::map<GameId, std::pair<std::string, GameId>> outcomesOf(const rv::world::RoomSelection& selection) {
    std::map<GameId, std::pair<std::string, GameId>> outcomes;
    for (const rv::world::RoomTitle& title : selection.titles) {
        outcomes[title.game] = {"title", GameId()};
    }
    for (const rv::world::UnplacedGame& game : selection.unplaced) {
        outcomes[game.game] = {std::string(rv::world::toString(game.reason)), game.representative};
    }
    return outcomes;
}

} // namespace

TEST_CASE("world/title_key: title keys against the oracle", "[world][title_key]") {
    CHECK(rv::world::kTitleKeyRule == "title-key/1");
    CHECK(rv::world::isValidRuleName(rv::world::kTitleKeyRule));
    for (const KeyCase& entry : kKeys) {
        INFO(entry.title);
        const auto key = rv::world::titleKey(entry.title);
        REQUIRE(key.has_value());
        CHECK(*key == entry.key);
    }
    // A title that is not UTF-8 has no key.
    CHECK_FALSE(rv::world::titleKey("Pixel \xFF Quest").has_value());
}

TEST_CASE("world/title_key: version tags against the oracle", "[world][title_key]") {
    for (const TagCase& entry : kTags) {
        INFO(entry.name);
        const rv::world::VersionTags tags = rv::world::readVersionTags(entry.name);
        CHECK(tags.region == entry.region);
        CHECK(tags.status == entry.status);
        CHECK(tags.revision == entry.revision);
    }
    // The region tag's text as written, and the order of the tags.
    CHECK(rv::world::readVersionTags("Pixel Quest (Japan, USA) (Rev 1).sfc").regionTag == "Japan, USA");
    CHECK(rv::world::readVersionTags("Pixel Quest (JU) [!].smc").regionTag == "JU");
    CHECK_FALSE(rv::world::readVersionTags("Pixel Quest.sfc").regionTag.has_value());
    CHECK(rv::world::fileNameOf("Collection A/Pixel Quest (USA).sfc") == "Pixel Quest (USA).sfc");
    CHECK(rv::world::fileNameOf("Pixel Quest (USA).sfc") == "Pixel Quest (USA).sfc");
    using rv::world::VersionTags;
    const VersionTags usa{RegionRank::UsaOrWorld, "USA", false, 0};
    const VersionTags usaBeta{RegionRank::UsaOrWorld, "USA", true, 3};
    const VersionTags usaRev{RegionRank::UsaOrWorld, "USA", false, 2};
    const VersionTags europe{RegionRank::Europe, "Europe", false, 9};
    CHECK(rv::world::versionTagsLess(usaRev, usa));     // the highest revision first
    CHECK(rv::world::versionTagsLess(usa, usaBeta));    // without a status tag first
    CHECK(rv::world::versionTagsLess(usaBeta, europe)); // the region first of all
    CHECK_FALSE(rv::world::versionTagsLess(usa, usa));
}

TEST_CASE("world/title_key: the table of invented file names against the oracle", "[world][title_key]") {
    const rv::world::RoomInput input = tableInput();
    const auto selection = rv::world::selectRoom(input, rv::world::WorldConfig{});
    REQUIRE(selection.has_value());
    CHECK(selection->eligibility == rv::world::Eligibility::Covered);
    const auto outcomes = outcomesOf(*selection);
    std::map<GameId, std::string> keys;
    for (const rv::world::RoomTitle& title : selection->titles) {
        keys[title.game] = title.titleKey;
    }
    REQUIRE(std::size(kOutcomes) == std::size(kRows));
    for (const Outcome& expected : kOutcomes) {
        const Row& row = kRows[expected.row];
        INFO(row.path);
        CHECK(*rv::world::titleKey(titleOf(row)) == expected.titleKey);
        const rv::world::VersionTags tags = rv::world::readVersionTags(rv::world::fileNameOf(row.path));
        CHECK(tags.region == expected.region);
        CHECK(tags.status == expected.status);
        CHECK(tags.revision == expected.revision);

        const GameId id = idOfRow(static_cast<std::size_t>(expected.row));
        const auto found = outcomes.find(id);
        const std::string outcome = found == outcomes.end() ? "none" : found->second.first;
        CHECK(outcome == expected.outcome);
        const GameId representative =
            expected.representative < 0 ? GameId() : idOfRow(static_cast<std::size_t>(expected.representative));
        if (found != outcomes.end()) {
            CHECK(found->second.second == representative);
        }
        if (outcome == "title") {
            CHECK(keys[id] == expected.titleKey);
        }
    }
}

TEST_CASE("world/title_key: the table's selection in priority order, golden", "[world][title_key][golden]") {
    const auto selection = rv::world::selectRoom(tableInput(), rv::world::WorldConfig{});
    REQUIRE(selection.has_value());
    // The favourite (its Europe version is), the most played (its Japanese version's 7 plays), the one Retroverse
    // started 3 times, the best rated (its prototype's 10), the next rated, then the others by their representatives'
    // sort keys.
    const std::vector<std::size_t> rows{0, 5, 33, 14, 20, 30, 15, 10, 21, 38, 39, 37, 18, 7, 24, 35, 11, 26, 4, 27};
    std::vector<GameId> expected;
    for (const std::size_t row : rows) {
        expected.push_back(idOfRow(row));
    }
    std::vector<GameId> order;
    for (const rv::world::RoomTitle& title : selection->titles) {
        order.push_back(title.game);
    }
    CHECK(order == expected);
    const rv::world::RoomTitle& first = selection->titles.front();
    CHECK(first.favorite);
    CHECK(selection->titles[1].playCount == 7);
    CHECK(selection->titles[2].playCount == 3);
    CHECK(selection->titles[3].ratingTenths == 10);
    CHECK(selection->titles[4].ratingTenths == 8);
    CHECK(selection->titles[5].sortKey == "ace racer (rev 1)");
    // 44 rows: 20 titles, 21 games unplaced, and two entries that are no games and a hidden game left out.
    CHECK(selection->unplaced.size() == 21);

    auto text = rv::world::writeRoomSelection(*selection);
    REQUIRE(text.has_value());
    rv::test::checkSelectionGolden("table", *text);
    const auto read = rv::world::readRoomSelection(*text, "table.json");
    REQUIRE(read.has_value());
    CHECK(*read == *selection);
}
