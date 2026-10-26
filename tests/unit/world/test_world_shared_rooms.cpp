// The shared rooms and the arcade room (rv/world/generation.hpp's generateBuilding(), incremental.hpp's
// updateBuilding(); ADR-0013 §3, §6, §7, §11; I4 step 4), with the shipped data: every arcade system a section of the
// arcade room on the ground floor, a system under sharedRoomMinTitles distinct titles (over every game, with a cover or
// not) a section of its floor's shared room, both of the generic case and without a console; a shared room's collection
// generated and updated as a console room's whose sort keys lead with the section (the face-out titles by priority
// across the sections, each section's titles after the previous section's, a new title after its predecessor in its
// section); a system crossing the threshold moving nothing (sticky); a shared room updated only with every section's
// selection (one given only some of them held and reported); the count rule; a full floor's console room joining the
// floor's shared room; their scenes' signs; a stored shared or arcade room's data read whatever its first section's
// classification is now.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/catalog/entry.hpp>
#include <rv/catalog/lookup_key.hpp>
#include <rv/catalog/metadata.hpp>
#include <rv/core/expected.hpp>
#include <rv/core/file_io.hpp>
#include <rv/core/random.hpp>
#include <rv/world/building.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/incremental.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/validation.hpp>

#include "support/temp_dir.hpp"
#include "world_generation_support.hpp"
#include "world_update_support.hpp"

using Catch::Matchers::ContainsSubstring;
using rv::catalog::GameId;
using rv::catalog::SystemName;
using rv::world::GenerationResult;
using rv::world::Room;
using rv::world::RoomId;
using rv::world::RoomKind;
using rv::world::RoomSelection;
using rv::world::WorldData;
using rv::world::WorldPlan;

namespace {

constexpr std::uint64_t kWorldSeed = 102938;

SystemName name(std::string_view text) {
    return *SystemName::parse(text);
}

/// The shipped data of `system`'s room (with the room its section would stand in), its ES-DE platform tags `tags`.
WorldData dataOf(std::string_view system, std::vector<std::string> tags = {}) {
    rv::world::RoomDataRequest request;
    request.system = name(system);
    request.platformTags = std::move(tags);
    auto data = rv::world::loadWorldData(rv::test::world::assetRoot(), request);
    INFO(system << ": " << (data ? std::string("loaded") : data.error().toString()));
    REQUIRE(data.has_value());
    return std::move(*data);
}

/// makeSelection()'s selection of `titles` titles for `system`'s room, `distinct` distinct titles over all its games.
RoomSelection selectionOf(std::string_view system, std::size_t titles, std::uint64_t seed, std::size_t distinct = 0) {
    RoomSelection selection = rv::test::world::makeSelection(titles, seed);
    selection.room = *RoomId::forSystem(name(system));
    for (rv::world::UnplacedGame& game : selection.unplaced) {
        game.room = selection.room;
    }
    selection.distinctTitles = std::max(distinct, titles);
    return selection;
}

/// selectionOf() with every title's priority its own play count (`first`, then every `step`), none a favourite or
/// rated, so that no sort key decides the priority.
RoomSelection rankedSelectionOf(std::string_view system, std::size_t titles, std::uint64_t seed, std::uint64_t first,
                                std::uint64_t step) {
    RoomSelection selection = selectionOf(system, titles, seed);
    std::uint64_t plays = first;
    for (rv::world::RoomTitle& title : selection.titles) {
        title.favorite = false;
        title.ratingTenths.reset();
        title.playCount = plays;
        plays += step;
    }
    std::sort(selection.titles.begin(), selection.titles.end(), rv::world::priorityLess);
    return selection;
}

struct Building {
    rv::world::BuildingInput input;
    std::vector<WorldData> rooms;
};

GenerationResult generate(const Building& building, const rv::world::BuildingData* layout = nullptr) {
    const WorldData& any = building.rooms.front();
    auto result = rv::world::generateBuilding(building.input, layout != nullptr ? *layout : any.building, any.systems,
                                              building.rooms);
    INFO((result ? std::string("generated") : result.error().toString()));
    REQUIRE(result.has_value());
    for (const rv::world::ValidationFinding& finding : result->validation.findings) {
        if (finding.severity == rv::world::Severity::Hard) {
            UNSCOPED_INFO(finding.rule << " " << finding.subject << ": " << finding.message);
        }
    }
    return std::move(*result);
}

const Room& roomOf(const WorldPlan& plan, std::string_view id) {
    const auto found = std::find_if(plan.rooms.begin(), plan.rooms.end(),
                                    [&](const Room& room) { return room.id.toString() == id; });
    INFO(id);
    REQUIRE(found != plan.rooms.end());
    return *found;
}

bool hasRoom(const WorldPlan& plan, std::string_view id) {
    return std::any_of(plan.rooms.begin(), plan.rooms.end(),
                       [&](const Room& room) { return room.id.toString() == id; });
}

std::vector<std::string> sectionsOf(const Room& room) {
    std::vector<std::string> sections;
    for (const SystemName& system : room.sections) {
        sections.push_back(system.str());
    }
    return sections;
}

/// Each placed game's slot within its room ("fx:hw-w/s:3.12": its id less the room's).
std::map<GameId, std::string> localSlots(const WorldPlan& plan) {
    std::map<GameId, std::string> slots;
    for (const rv::world::Placement& placement : plan.placements) {
        const std::string text = placement.slot.toString();
        slots.emplace(placement.game, text.substr(text.find('/') + 1));
    }
    return slots;
}

/// The plan's placements that stand in a face-out section's slots.
std::set<GameId> faceOutGames(const WorldPlan& plan) {
    const auto sections = rv::test::world::levelSections(plan);
    std::set<GameId> games;
    for (const rv::world::Placement& placement : plan.placements) {
        const auto found = sections.find({placement.slot.fixture(), placement.slot.level()});
        if (found != sections.end() && rv::world::isFaceOut(found->second)) {
            games.insert(placement.game);
        }
    }
    return games;
}

/// The selection of a console room equivalent to a shared room of `sections`: their titles with sort keys that lead
/// with the section's index, as one system's.
RoomSelection sectionsAsOneSystem(std::string_view system, const std::vector<const RoomSelection*>& sections) {
    RoomSelection merged;
    merged.room = *RoomId::forSystem(name(system));
    merged.titleKeyRule = sections.front()->titleKeyRule;
    merged.catalogSortKeyVersion = sections.front()->catalogSortKeyVersion;
    for (std::size_t index = 0; index < sections.size(); ++index) {
        for (rv::world::RoomTitle title : sections[index]->titles) {
            title.sortKey = std::to_string(index) + "|" + title.sortKey;
            merged.titles.push_back(std::move(title));
        }
        for (rv::world::UnplacedGame game : sections[index]->unplaced) {
            game.room = merged.room;
            merged.unplaced.push_back(game);
        }
        merged.distinctTitles += sections[index]->distinctTitles;
    }
    std::sort(merged.titles.begin(), merged.titles.end(), rv::world::priorityLess);
    std::sort(merged.unplaced.begin(), merged.unplaced.end());
    return merged;
}

/// The update of `previous` to `selections` with the stored rooms' data (loadRoomData()).
GenerationResult update(const WorldPlan& previous, std::vector<RoomSelection> selections,
                        const rv::world::WorldConfig& config,
                        std::map<SystemName, std::vector<std::string>> platformTags = {}) {
    std::vector<WorldData> rooms;
    for (const Room& room : previous.rooms) {
        const auto tags = platformTags.find(room.sections.front());
        auto data = rv::world::loadRoomData(rv::test::world::assetRoot(), room,
                                            tags != platformTags.end() ? std::span<const std::string>(tags->second)
                                                                       : std::span<const std::string>());
        INFO(room.id.toString() << ": " << (data ? std::string("loaded") : data.error().toString()));
        REQUIRE(data.has_value());
        rooms.push_back(std::move(*data));
    }
    rv::world::BuildingUpdateInput input;
    input.library = rv::test::world::library();
    input.selections = std::move(selections);
    input.platformTags = std::move(platformTags);
    input.config = config;
    input.previous = previous;
    auto result =
        rv::world::updateBuilding(input, rooms.front().building, rooms.front().systems, std::span(rooms));
    INFO((result ? std::string("updated") : result.error().toString()));
    REQUIRE(result.has_value());
    return std::move(*result);
}

rv::world::WorldConfig defaultConfig() {
    rv::world::WorldConfig config;
    config.seed = kWorldSeed;
    return config;
}

/// The handheld floor's small systems (pokemini, and pocketlab, a custom system with gb's platform tag) and gb.
Building handhelds() {
    Building building;
    building.input.library = rv::test::world::library();
    building.input.config = defaultConfig();
    building.input.platformTags[name("pocketlab")] = {"gb"};
    building.rooms = {dataOf("gb"), dataOf("pokemini"), dataOf("pocketlab", {"gb"})};
    building.input.selections = {selectionOf("gb", 30, 3400), selectionOf("pokemini", 8, 3500),
                                 selectionOf("pocketlab", 6, 3700)};
    return building;
}

} // namespace

TEST_CASE("world/shared rooms: arcade systems in the arcade room, small systems in their floor's shared room",
          "[world][shared_rooms]") {
    Building building = handhelds();
    building.rooms.push_back(dataOf("mame"));
    building.rooms.push_back(dataOf("fbneo"));
    building.rooms.push_back(dataOf("snes"));
    building.rooms.push_back(dataOf("amiga"));
    building.input.selections.push_back(selectionOf("mame", 15, 3100, 25));
    building.input.selections.push_back(selectionOf("fbneo", 40, 3150)); // arcade, whatever its size
    building.input.selections.push_back(selectionOf("snes", 40, 3200));
    building.input.selections.push_back(selectionOf("amiga", 20, 3600)); // under 30 distinct titles
    const GenerationResult result = generate(building);
    const WorldPlan& plan = result.plan;
    CHECK(result.validation.passed());

    std::map<std::string, std::vector<std::string>> rooms;
    for (const Room& room : plan.rooms) {
        rooms[room.id.toString()] = sectionsOf(room);
    }
    // The sections in the order the systems entered: mame (101) before fbneo (102), the custom pocketlab (gb's row,
    // 700) before pokemini (740).
    CHECK(rooms == std::map<std::string, std::vector<std::string>>{{"area:arcade", {"mame", "fbneo"}},
                                                                   {"area:shared:computer", {"amiga"}},
                                                                   {"area:shared:handheld", {"pocketlab", "pokemini"}},
                                                                   {"room:gb", {"gb"}},
                                                                   {"room:snes", {"snes"}}});
    const Room& arcade = roomOf(plan, "area:arcade");
    CHECK(arcade.kind == RoomKind::Arcade);
    CHECK(arcade.floor == "ground");
    const Room& shared = roomOf(plan, "area:shared:handheld");
    CHECK(shared.kind == RoomKind::Shared);
    CHECK(shared.floor == "handheld");
    CHECK(roomOf(plan, "area:shared:computer").floor == "computer");
    const WorldData& gb = building.rooms.front();
    for (const Room* room : {&arcade, &shared, &roomOf(plan, "area:shared:computer")}) {
        INFO(room->id.toString());
        // The arcade room keeps I4's generic case until I6 step 6 (ADR-0014 §2, amended on 2026-10-24).
        CHECK(room->mediaFamily ==
              (room->kind == RoomKind::Arcade ? rv::world::kArcadeRoomFamily : rv::world::kSharedRoomFamily));
        // The pack its floor gives it, the arcade room's its kind's (I6 step 3, ADR-0014 §5).
        CHECK(room->themePack == rv::world::themePackOf(room->kind, room->floor,
                                                        room->kind == RoomKind::Arcade
                                                            ? rv::world::SystemKind::Arcade
                                                            : gb.building.floor(room->floor)->kinds.front(),
                                                        gb.systems, gb.building));
        // The hero corner without its console part (a code rule by kind), with its CRT.
        bool crt = false;
        for (const rv::world::Fixture& fixture : plan.fixtures) {
            if (fixture.id.room() == room->id) {
                CHECK(fixture.id.part() != "console");
                crt = crt || fixture.id.part() == rv::world::kHeroTvPart;
            }
        }
        CHECK(crt);
    }
    // The console rooms keep theirs.
    for (const std::string_view id : {"room:gb", "room:snes"}) {
        const RoomId room = *RoomId::parse(id);
        CHECK(std::any_of(plan.fixtures.begin(), plan.fixtures.end(), [&](const rv::world::Fixture& fixture) {
            return fixture.id.room() == room && fixture.id.part() == "console";
        }));
    }
    // gb entered the handheld floor first, the north row; the shared room the south row's first place.
    CHECK(roomOf(plan, "room:gb").placement == rv::world::RoomPlacement{7650, -6650, 0});
    CHECK(shared.placement == rv::world::RoomPlacement{7650, 6650, 1});
    CHECK(arcade.placement.xMm == 9000); // the hub's first place (I6 step 5)
    CHECK(arcade.placement.yawHalfTurns == 0);
    // Every game of a section is placed in its room or unplaced there.
    for (const RoomSelection& selection : building.input.selections) {
        const Room* home = nullptr;
        for (const Room& room : plan.rooms) {
            if (std::find(room.sections.begin(), room.sections.end(), selection.room.system()) != room.sections.end()) {
                home = &room;
            }
        }
        REQUIRE(home != nullptr);
        const std::map<GameId, std::string> slots = localSlots(plan);
        for (const rv::world::RoomTitle& title : selection.titles) {
            const bool placed = std::any_of(plan.placements.begin(), plan.placements.end(), [&](const auto& p) {
                return p.game == title.game && p.slot.fixture().room() == home->id;
            });
            CHECK(placed);
        }
        for (const rv::world::UnplacedGame& game : selection.unplaced) {
            const bool listed = std::any_of(plan.unplaced.begin(), plan.unplaced.end(), [&](const auto& entry) {
                return entry.game == game.game && entry.room == home->id && entry.reason == game.reason;
            });
            CHECK(listed);
        }
    }
    // The entry order is the data's: the same building whatever the selections' order, two runs alike.
    Building reversed = building;
    std::reverse(reversed.input.selections.begin(), reversed.input.selections.end());
    std::reverse(reversed.rooms.begin(), reversed.rooms.end());
    CHECK(*rv::world::writePlan(generate(reversed).plan) == *rv::world::writePlan(plan));
    // A plan this program reads back as written.
    const std::string text = *rv::world::writePlan(plan);
    CHECK(*rv::world::writePlan(*rv::world::readPlan(text, "building")) == text);

    // The scenes: no console, the template's CRT, the signs of the kind's defaults (else the fallback's).
    for (const auto& [id, sign] : std::map<std::string, std::string>{{"area:arcade", "ARCADE"},
                                                                     {"area:shared:handheld", "MORE GAMES"}}) {
        INFO(id);
        // (Their platform is the floor's kind's: the first section's platform tags change nothing.)
        auto data = rv::world::loadSceneData(rv::test::world::assetRoot(), plan, *RoomId::parse(id));
        REQUIRE(data.has_value());
        CHECK_FALSE(data->platform.console.has_value());
        CHECK(data->platform.signText == sign);
        auto scene = rv::world::toSceneFile(plan, *RoomId::parse(id), *data);
        REQUIRE(scene.has_value());
        std::vector<std::string> signs;
        bool crt = false;
        for (const nlohmann::json& object : scene->sceneFile["objects"]) {
            const std::string objectId = object["id"].get<std::string>();
            CHECK(objectId != "fx:hero.console");
            crt = crt || objectId == "fx:hero.crt";
            const auto textures = object.find("textures");
            if (textures != object.end() && textures->contains("picture") &&
                (*textures)["picture"].value("pattern", "") == "neon") { // the neon sign (I6 step 3)
                signs.push_back((*textures)["picture"].value("title", ""));
            }
        }
        CHECK(crt);
        CHECK(signs == std::vector<std::string>{sign});
    }
}

TEST_CASE("world/shared rooms: the count rule: one cover, 40 distinct titles, a room of its own",
          "[world][shared_rooms]") {
    // ADR-0013 §3: the distinct titles over every game, with a cover or not (RoomSelection::distinctTitles,
    // selectRoom() under include "all"), decide; the room then places the one covered title.
    const auto snapshotOf = [](std::size_t games) {
        rv::world::RoomInput input;
        input.library = rv::test::world::library();
        input.snapshot.system = name("snes");
        input.snapshot.mediaKnown = true;
        for (std::uint64_t i = 0; i < games; ++i) {
            rv::catalog::RoomEntry entry;
            entry.id = *GameId::fromValue(1000 + i);
            const std::string title = "Comet Tale " + std::to_string(100 + i);
            entry.key = *rv::catalog::makeLookupKey(title + " (USA).sfc");
            entry.title = title;
            entry.sortKey = *rv::catalog::sortKeyOf(title);
            if (i == 0) {
                entry.media[rv::catalog::MediaKind::Cover] = rv::catalog::MediaPresence{true, ".png"};
            }
            input.snapshot.entries.push_back(std::move(entry));
        }
        return input;
    };
    const WorldData snes = dataOf("snes");
    for (const auto& [games, room] : std::vector<std::pair<std::size_t, std::string>>{{40, "room:snes"},
                                                                                      {30, "room:snes"},
                                                                                      {29, "area:shared:gen4"}}) {
        INFO(games);
        auto selection = rv::world::selectRoom(snapshotOf(games), defaultConfig());
        REQUIRE(selection.has_value());
        CHECK(selection->titles.size() == 1);
        CHECK(selection->distinctTitles == games);
        Building building;
        building.input.library = rv::test::world::library();
        building.input.config = defaultConfig();
        building.input.selections = {*selection};
        building.rooms = {snes};
        const GenerationResult result = generate(building);
        CHECK(result.validation.passed());
        REQUIRE(result.plan.rooms.size() == 1);
        CHECK(result.plan.rooms.front().id.toString() == room);
        CHECK(result.plan.placements.size() == 1);
        CHECK(result.plan.unplaced.size() == games - 1); // the other games, without a cover
    }
    // sharedRoomMinTitles 0: every system a room of its own (golden G06's single ROM).
    auto lone = rv::world::selectRoom(snapshotOf(1), defaultConfig());
    REQUIRE(lone.has_value());
    Building building;
    building.input.library = rv::test::world::library();
    building.input.config = rv::test::world::roomConfig(kWorldSeed);
    building.input.selections = {*lone};
    building.rooms = {snes};
    CHECK(generate(building).plan.rooms.front().id.toString() == "room:snes");
}

TEST_CASE("world/shared rooms: a shared room's collection is a console room's whose sort keys lead with the section",
          "[world][shared_rooms]") {
    // Two gen4 systems sharing a room (a threshold above their counts): sfc (order 401) before neogeo (406). The room's
    // face-out titles are the first D + B of both by priority, and each section's titles stand after the previous
    // section's: the room is the console room of one system whose titles' sort keys lead with their section.
    rv::world::WorldConfig config = defaultConfig();
    config.sharedRoomMinTitles = 1000;
    const RoomSelection sfc = rankedSelectionOf("sfc", 60, 4100, 1, 2);       // odd play counts
    const RoomSelection neogeo = rankedSelectionOf("neogeo", 60, 4200, 2, 2); // even ones, interleaved
    Building building;
    building.input.library = rv::test::world::library();
    building.input.config = config;
    building.input.selections = {neogeo, sfc};
    const WorldData sfcData = dataOf("sfc");
    building.rooms = {dataOf("neogeo"), sfcData};
    const GenerationResult shared = generate(building);
    CHECK(shared.validation.passed());
    REQUIRE(shared.plan.rooms.size() == 1);
    const Room& room = shared.plan.rooms.front();
    CHECK(room.id.toString() == "area:shared:gen4");
    CHECK(sectionsOf(room) == std::vector<std::string>{"sfc", "neogeo"});

    // The equivalent console room: sfc alone with both sections' titles, in the shared room's data.
    REQUIRE(sfcData.sharedRoom != nullptr);
    rv::world::GenerationInput alone;
    alone.library = rv::test::world::library();
    alone.selection = sectionsAsOneSystem("sfc", {&sfc, &neogeo});
    alone.config = rv::test::world::roomConfig(kWorldSeed);
    auto console = rv::world::generate(alone, *sfcData.sharedRoom);
    REQUIRE(console.has_value());
    CHECK(console->plan.rooms.front().id.toString() == "room:sfc");
    const std::map<GameId, std::string> slots = localSlots(shared.plan);
    CHECK(slots.size() == 120);
    CHECK(slots == localSlots(console->plan));
    // The face-out titles: the 48 most played of both systems (D + B of the generic case's room).
    std::vector<std::pair<std::uint64_t, GameId>> byPlays;
    for (const RoomSelection* selection : {&sfc, &neogeo}) {
        for (const rv::world::RoomTitle& title : selection->titles) {
            byPlays.emplace_back(title.playCount, title.game);
        }
    }
    std::sort(byPlays.rbegin(), byPlays.rend());
    std::set<GameId> mostPlayed;
    for (std::size_t i = 0; i < 48; ++i) {
        mostPlayed.insert(byPlays[i].second);
    }
    CHECK(faceOutGames(shared.plan) == mostPlayed);
    // Every unplaced game of the sections names the shared room.
    CHECK(shared.plan.unplaced.size() == sfc.unplaced.size() + neogeo.unplaced.size());
    for (const rv::world::UnplacedGame& game : shared.plan.unplaced) {
        CHECK(game.room == room.id);
    }

    // The update: each section's new titles after their predecessor in their section, as in the console room.
    RoomSelection sfcMore = sfc;
    RoomSelection neogeoMore = neogeo;
    std::set<std::uint64_t> taken = rv::test::world::gameValuesOf(sfc);
    const std::set<std::uint64_t> neogeoTaken = rv::test::world::gameValuesOf(neogeo);
    taken.insert(neogeoTaken.begin(), neogeoTaken.end());
    rv::core::Random random(4300);
    std::vector<GameId> added;
    for (int i = 0; i < 6; ++i) {
        added.push_back(rv::test::world::addTitle(i % 2 == 0 ? sfcMore : neogeoMore, random, taken).game);
    }
    const GenerationResult updated = update(shared.plan, {neogeoMore, sfcMore}, config);
    CHECK(updated.validation.passed());
    CHECK(updated.changes.outcome == rv::world::GenerationOutcome::Updated);
    CHECK(updated.changes.added.size() == 6);
    CHECK(updated.changes.kept == shared.plan.placements.size());
    auto consoleUpdated = rv::world::update(
        rv::world::UpdateInput{alone.library, sectionsAsOneSystem("sfc", {&sfcMore, &neogeoMore}), alone.config, {},
                               console->plan, {}, {}},
        *sfcData.sharedRoom);
    REQUIRE(consoleUpdated.has_value());
    const std::map<GameId, std::string> after = localSlots(updated.plan);
    const std::map<GameId, std::string> consoleAfter = localSlots(consoleUpdated->plan);
    for (const GameId& game : added) {
        INFO(game.toString());
        REQUIRE(after.contains(game));
        CHECK(after.at(game) == consoleAfter.at(game));
    }
    CHECK(after == consoleAfter);
}

TEST_CASE("world/shared rooms: a system crossing the threshold moves nothing; an update needs every section",
          "[world][shared_rooms]") {
    const Building building = handhelds();
    const GenerationResult generated = generate(building);
    CHECK(generated.validation.passed());
    const WorldPlan& plan = generated.plan;
    const RoomId shared = *RoomId::parse("area:shared:handheld");
    CHECK(sectionsOf(roomOf(plan, "area:shared:handheld")) == std::vector<std::string>{"pocketlab", "pokemini"});
    const std::map<SystemName, std::vector<std::string>> tags = building.input.platformTags;

    // The same selections (any order) with the stored rooms' data: nothing new, nothing written.
    std::vector<RoomSelection> same = building.input.selections;
    std::reverse(same.begin(), same.end());
    const GenerationResult unchanged = update(plan, same, building.input.config, tags);
    CHECK(unchanged.changes.digestAfter == unchanged.changes.digestBefore);
    CHECK(unchanged.plan == plan);

    // pokemini grows past 30 distinct titles: it stays in its section (sticky), nothing placed before moves, its new
    // titles stand in the shared room.
    RoomSelection pokemini = building.input.selections[1];
    REQUIRE(pokemini.room.system() == name("pokemini"));
    std::set<std::uint64_t> taken;
    for (const RoomSelection& selection : building.input.selections) {
        const std::set<std::uint64_t> values = rv::test::world::gameValuesOf(selection);
        taken.insert(values.begin(), values.end());
    }
    rv::core::Random random(3501);
    for (int i = 0; i < 24; ++i) {
        rv::test::world::addTitle(pokemini, random, taken);
    }
    CHECK(pokemini.distinctTitles == 32);
    std::vector<RoomSelection> grown = building.input.selections;
    grown[1] = pokemini;
    const GenerationResult updated = update(plan, grown, building.input.config, tags);
    CHECK(updated.validation.passed());
    CHECK(updated.changes.added.size() == 24);
    CHECK(updated.changes.kept == plan.placements.size());
    CHECK(std::includes(updated.plan.placements.begin(), updated.plan.placements.end(), plan.placements.begin(),
                        plan.placements.end()));
    CHECK_FALSE(hasRoom(updated.plan, "room:pokemini"));
    CHECK(sectionsOf(roomOf(updated.plan, "area:shared:handheld")) ==
          std::vector<std::string>{"pocketlab", "pokemini"});
    CHECK(roomOf(updated.plan, "area:shared:handheld").placement == roomOf(plan, "area:shared:handheld").placement);
    for (const rv::world::Placement& placement : updated.plan.placements) {
        if (std::find(updated.changes.added.begin(), updated.changes.added.end(), placement.game) !=
            updated.changes.added.end()) {
            CHECK(placement.slot.fixture().room() == shared);
        }
    }
    CHECK(*rv::world::roomContentDigest(updated.plan, *RoomId::parse("room:gb")) ==
          *rv::world::roomContentDigest(plan, *RoomId::parse("room:gb")));
    CHECK(unchanged.changes.heldRooms.empty());
    CHECK(updated.changes.heldRooms.empty());

    // pokemini's selection without pocketlab's: the shared room is held as it is (the plan does not say which section
    // an unplaced game was of), its selection not read, and the report names it with the section it lacks: nothing
    // changes, nothing is written.
    const std::string heldReason = "no selection of its section pocketlab: the selections of its other sections are "
                                   "not read";
    const GenerationResult partial = update(plan, {pokemini}, building.input.config, tags);
    CHECK(partial.validation.passed());
    CHECK(partial.changes.outcome == rv::world::GenerationOutcome::Updated);
    CHECK(partial.changes.added.empty());
    CHECK(partial.plan == plan);
    CHECK(partial.changes.digestAfter == partial.changes.digestBefore);
    CHECK(partial.changes.keptRooms.empty());
    REQUIRE(partial.changes.heldRooms.size() == 1);
    CHECK(partial.changes.heldRooms.front().room == shared);
    CHECK(partial.changes.heldRooms.front().reason == heldReason);

    // With gb's grown selection too: gb's room takes its new titles, the shared room is held and reported alike.
    RoomSelection gb = building.input.selections[0];
    REQUIRE(gb.room.system() == name("gb"));
    rv::core::Random gbRandom(3401);
    for (int i = 0; i < 3; ++i) {
        rv::test::world::addTitle(gb, gbRandom, taken);
    }
    const GenerationResult mixed = update(plan, {gb, pokemini}, building.input.config, tags);
    CHECK(mixed.validation.passed());
    CHECK(mixed.changes.added.size() == 3);
    for (const rv::world::Placement& placement : mixed.plan.placements) {
        if (std::find(mixed.changes.added.begin(), mixed.changes.added.end(), placement.game) !=
            mixed.changes.added.end()) {
            CHECK(placement.slot.fixture().room() == *RoomId::parse("room:gb"));
        }
    }
    CHECK(*rv::world::roomContentDigest(mixed.plan, shared) == *rv::world::roomContentDigest(plan, shared));
    REQUIRE(mixed.changes.heldRooms.size() == 1);
    CHECK(mixed.changes.heldRooms.front().room == shared);
    CHECK(mixed.changes.heldRooms.front().reason == heldReason);
}

TEST_CASE("world/shared rooms: on a full floor a console room joins the floor's shared room", "[world][shared_rooms]") {
    // One room per row: snes takes gen4's north row, the shared room (sfc, under the threshold) its south row; then
    // megadrive finds no place and joins the shared room as a section (ADR-0013 §7).
    rv::world::BuildingData small = rv::test::world::snesData().building;
    small.layout.maxRoomsPerRow = 1;
    Building building;
    building.input.library = rv::test::world::library();
    building.input.config = defaultConfig();
    building.rooms = {dataOf("snes"), dataOf("sfc"), dataOf("megadrive")};
    building.input.selections = {selectionOf("snes", 40, 5000), selectionOf("sfc", 5, 5001),
                                 selectionOf("megadrive", 40, 5002)};
    const GenerationResult joined = generate(building, &small);
    CHECK(joined.validation.passed());
    REQUIRE(joined.plan.rooms.size() == 2);
    CHECK(sectionsOf(roomOf(joined.plan, "area:shared:gen4")) == std::vector<std::string>{"sfc", "megadrive"});
    CHECK(joined.plan.placements.size() == 85);

    // Without a shared room on the floor, a small system that finds no place has its games unplaced for capacity,
    // naming the shared room.
    building.input.selections = {selectionOf("snes", 40, 5000), selectionOf("megadrive", 40, 5002),
                                 selectionOf("neogeo", 5, 5003)};
    building.rooms = {dataOf("snes"), dataOf("megadrive"), dataOf("neogeo")};
    const GenerationResult full = generate(building, &small);
    CHECK(full.validation.passed());
    CHECK(full.plan.rooms.size() == 2);
    std::size_t capacity = 0;
    for (const rv::world::UnplacedGame& game : full.plan.unplaced) {
        if (game.room.toString() == "area:shared:gen4") {
            CHECK(game.reason == rv::world::UnplacedReason::Capacity);
            ++capacity;
        }
    }
    CHECK(capacity == 5);
}

TEST_CASE("world/shared rooms: a shared room's data comes with its system's", "[world][shared_rooms]") {
    // loadWorldData() of a system reads the room its section would stand in; loadRoomData() of a stored shared room
    // reads the same data (its family, theme pack, platform and prefabs), so that a building and its update hash alike.
    const WorldData pokemini = dataOf("pokemini");
    CHECK(pokemini.kind == RoomKind::Console);
    CHECK(pokemini.mediaFamily.id == "cartridge_handheld");
    CHECK(pokemini.platform.console.has_value());
    REQUIRE(pokemini.sharedRoom != nullptr);
    const WorldData& shared = *pokemini.sharedRoom;
    CHECK(shared.kind == RoomKind::Shared);
    CHECK(shared.sharedRoom == nullptr);
    CHECK(shared.mediaFamily.id == "generic_case_v2");
    CHECK(shared.platform.mediaFamily == "generic_case_v2");
    CHECK_FALSE(shared.platform.console.has_value());
    CHECK(shared.platform.system == name("pokemini"));
    CHECK(shared.platform.signText == "MORE GAMES");
    CHECK(shared.roomTemplate.heroModule == *shared.mediaFamily.fixtures.heroModule);
    const WorldData mame = dataOf("mame");
    REQUIRE(mame.sharedRoom != nullptr);
    CHECK(mame.sharedRoom->kind == RoomKind::Arcade);
    CHECK(mame.sharedRoom->platform.signText == "ARCADE");

    Room room;
    room.id = *RoomId::sharedArea("handheld");
    room.kind = RoomKind::Shared;
    room.floor = "handheld";
    room.sections = {name("pokemini"), name("gb")};
    room.templateId = std::string(rv::world::kDefaultRoomTemplate);
    room.mediaFamily = shared.mediaFamily.id;
    room.themePack = shared.platform.themePack;
    auto stored = rv::world::loadRoomData(rv::test::world::assetRoot(), room);
    REQUIRE(stored.has_value());
    CHECK(stored->kind == RoomKind::Shared);
    CHECK(stored->sharedRoom == nullptr);
    CHECK(stored->platform == shared.platform);
    CHECK(stored->mediaFamily == shared.mediaFamily);
    CHECK(rv::world::prefabCatalogToJson(stored->prefabs) == rv::world::prefabCatalogToJson(shared.prefabs));
    CHECK(stored->roomTemplate == shared.roomTemplate);
}

TEST_CASE("world/shared rooms: a stored shared or arcade room's data whatever its first section's classification now",
          "[world][shared_rooms]") {
    // gb and pokemini: pokemini's 8 titles take the first section of the handheld floor's shared room.
    Building building;
    building.input.library = rv::test::world::library();
    building.input.config = defaultConfig();
    building.rooms = {dataOf("gb"), dataOf("pokemini")};
    building.input.selections = {selectionOf("gb", 30, 3400), selectionOf("pokemini", 8, 3500)};
    const GenerationResult generated = generate(building);
    REQUIRE(generated.validation.passed());
    const WorldPlan& plan = generated.plan;
    const RoomId sharedId = *RoomId::sharedArea("handheld");
    const Room& stored = roomOf(plan, sharedId.toString());
    REQUIRE(sectionsOf(stored) == std::vector<std::string>{"pokemini"});

    // A later systems.json excludes pokemini (a copy of the asset root's world data and manifests).
    const rv::test::TempDir dir("rv-world-shared-excluded");
    const std::filesystem::path root = dir.path() / "assets";
    std::filesystem::create_directories(root);
    for (const std::string_view folder : {"world", "manifests"}) {
        std::filesystem::copy(rv::test::world::assetRoot() / folder, root / folder,
                              std::filesystem::copy_options::recursive);
    }
    const std::filesystem::path systemsFile = root / "world" / "systems.json";
    auto text = rv::core::readTextFile(systemsFile);
    REQUIRE(text.has_value());
    const std::size_t from = text->find(R"({"system": "pokemini", "kind": "handheld")");
    REQUIRE(from != std::string::npos);
    const std::size_t to = text->find("]},", from);
    REQUIRE(to != std::string::npos);
    text->replace(from, to + 3 - from, R"({"system": "pokemini", "kind": "excluded", "platforms": ["pokemini"]},)");
    REQUIRE(rv::core::writeTextFileAtomic(systemsFile, *text).has_value());
    auto systems = rv::world::loadSystemsData(root);
    REQUIRE(systems.has_value());
    CHECK(rv::world::classify(name("pokemini"), {}, *systems, building.rooms.front().building).kind() ==
          rv::world::SystemKind::Excluded);

    // Entering the building, pokemini gets no room now.
    rv::world::RoomDataRequest entering;
    entering.system = name("pokemini");
    entering.kind = RoomKind::Shared;
    auto refused = rv::world::loadWorldData(root, entering);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == rv::ErrorCode::InvalidArgument);
    CHECK_THAT(refused.error().message, ContainsSubstring("the system pokemini gets no room (excluded)"));

    // The stored shared room's data is read as before: its kind and recorded floor decide its platform (ADR-0013 §3: a
    // data change never moves or freezes a room).
    auto now = rv::world::loadRoomData(root, stored);
    INFO((now ? std::string("loaded") : now.error().toString()));
    REQUIRE(now.has_value());
    auto before = rv::world::loadRoomData(rv::test::world::assetRoot(), stored);
    REQUIRE(before.has_value());
    CHECK(now->kind == RoomKind::Shared);
    CHECK(now->platform == before->platform);
    CHECK(now->mediaFamily == before->mediaFamily);
    CHECK(rv::world::prefabCatalogToJson(now->prefabs) == rv::world::prefabCatalogToJson(before->prefabs));
    CHECK(now->roomTemplate == before->roomTemplate);

    // So the building updates with that data: gb's new titles placed, the shared room as it stood.
    std::vector<WorldData> rooms;
    for (const Room& room : plan.rooms) {
        auto data = rv::world::loadRoomData(root, room);
        INFO(room.id.toString() << ": " << (data ? std::string("loaded") : data.error().toString()));
        REQUIRE(data.has_value());
        rooms.push_back(std::move(*data));
    }
    RoomSelection gb = building.input.selections[0];
    std::set<std::uint64_t> taken;
    for (const RoomSelection& selection : building.input.selections) {
        const std::set<std::uint64_t> values = rv::test::world::gameValuesOf(selection);
        taken.insert(values.begin(), values.end());
    }
    rv::core::Random random(3402);
    for (int i = 0; i < 2; ++i) {
        rv::test::world::addTitle(gb, random, taken);
    }
    rv::world::BuildingUpdateInput input;
    input.library = rv::test::world::library();
    input.selections = {gb, building.input.selections[1]};
    input.config = building.input.config;
    input.previous = plan;
    auto updated = rv::world::updateBuilding(input, rooms.front().building, *systems, std::span(rooms));
    INFO((updated ? std::string("updated") : updated.error().toString()));
    REQUIRE(updated.has_value());
    CHECK(updated->validation.passed());
    CHECK(updated->changes.added.size() == 2);
    CHECK(updated->changes.keptRooms.empty());
    CHECK(updated->changes.heldRooms.empty());
    CHECK(*rv::world::roomContentDigest(updated->plan, sharedId) == *rv::world::roomContentDigest(plan, sharedId));

    // The arcade room alike: a stored one whose first section gets no room now (kodi, excluded in the shipped data).
    const WorldData mame = dataOf("mame");
    REQUIRE(mame.sharedRoom != nullptr);
    Room arcade;
    arcade.id = *RoomId::arcadeArea();
    arcade.kind = RoomKind::Arcade;
    arcade.floor = "ground";
    arcade.sections = {name("kodi"), name("mame")};
    arcade.templateId = std::string(rv::world::kDefaultRoomTemplate);
    arcade.mediaFamily = mame.sharedRoom->mediaFamily.id;
    arcade.themePack = mame.sharedRoom->platform.themePack;
    auto arcadeData = rv::world::loadRoomData(rv::test::world::assetRoot(), arcade);
    INFO((arcadeData ? std::string("loaded") : arcadeData.error().toString()));
    REQUIRE(arcadeData.has_value());
    CHECK(arcadeData->kind == RoomKind::Arcade);
    CHECK(arcadeData->platform.system == name("kodi"));
    CHECK(arcadeData->platform.signText == "ARCADE");
    CHECK_FALSE(arcadeData->platform.console.has_value());
    CHECK(arcadeData->mediaFamily == mame.sharedRoom->mediaFamily);
}
