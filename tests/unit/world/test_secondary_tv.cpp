// The rooms' secondary TVs (I5 step 3, ADR-0016 §5): a theme pack's furniture, a look and never the plan's. The reader
// takes the pack's one secondary TV in a floor-standing decoration kind's list; the scene builder stands it in at most
// one socket of that kind a room (two in a room grown past 12 m, none in the arcade), never the one nearest the
// doorway's camera, the room's back half or the hero corner's reach first, then the room's seed (I5 step 3's review),
// within its fixture's own footprint wherever the fixture stands (the room editor may move it); the plan, its slots
// and lights stay as they are, and the other decorations draw as before.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/assets/prefab_resolver.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/world/room_look.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/validation.hpp>

#include <rv/core/file_io.hpp>

#include "plan_scene_support.hpp"
#include "world_generation_support.hpp"

using Catch::Matchers::ContainsSubstring;
using rv::world::Fixture;
using rv::world::FixtureId;
using rv::world::FixtureRole;

namespace {

const std::filesystem::path& assets() {
    return rv::test::world::sourceAssetRoot();
}

rv::world::SceneData withPack(rv::world::SceneData data, const std::string& pack) {
    auto theme = rv::world::loadThemeMaterials(assets(), pack);
    REQUIRE(theme.has_value());
    auto families = rv::assets::loadPrefabFamilies(assets());
    REQUIRE(families.has_value());
    data.theme = std::move(*theme);
    data.furniture = rv::world::resolveSceneFurniture(assets(), *families, data.theme.furniture);
    return data;
}

std::vector<FixtureId> tvsOf(const rv::world::WorldPlan& plan, const rv::world::RoomId& room,
                             const rv::world::SceneData& data) {
    auto tvs = rv::world::secondaryTvFixtures(plan, room, data);
    REQUIRE(tvs.has_value());
    return std::move(*tvs);
}

/// The scene file's objects of `room`, by id.
std::map<std::string, nlohmann::json> objectsOf(const rv::world::WorldPlan& plan, const rv::world::RoomId& room,
                                                const rv::world::SceneData& data) {
    auto scene = rv::world::toSceneFile(plan, room, data);
    REQUIRE(scene.has_value());
    std::map<std::string, nlohmann::json> objects;
    for (const nlohmann::json& object : scene->sceneFile.at("objects")) {
        objects.emplace(object.at("id").get<std::string>(), object);
    }
    return objects;
}

const Fixture& fixtureOf(const rv::world::WorldPlan& plan, const FixtureId& id) {
    const auto found =
        std::find_if(plan.fixtures.begin(), plan.fixtures.end(), [&](const Fixture& f) { return f.id == id; });
    REQUIRE(found != plan.fixtures.end());
    return *found;
}

std::size_t decorationsOfKind(const rv::world::WorldPlan& plan, const rv::world::RoomId& room,
                              const std::string& kind) {
    return static_cast<std::size_t>(std::count_if(plan.fixtures.begin(), plan.fixtures.end(), [&](const Fixture& f) {
        return f.id.room() == room && f.role == FixtureRole::Decoration && f.prefab.kind == kind;
    }));
}

/// Where a fixture's socket stands at home in `room` (its footprint's middle), and what the placement reads of it: its
/// squared distances to the doorway's camera and to the hero corner, its depth from the doorway and the room's extent
/// that way (millimetres; the console room's doorway is in its south wall).
struct Home {
    std::int64_t x = 0;
    std::int64_t z = 0;
    std::int64_t toDoorway = 0;
    std::int64_t depth = 0;
    std::int64_t extent = 0;
    std::int64_t toHero = 0;

    [[nodiscard]] bool backHalf() const { return 2 * depth >= extent; }
    [[nodiscard]] std::int64_t band() const { return std::clamp<std::int64_t>(3 * depth / extent, 0, 2); }
};

Home homeOf(const rv::world::Room& room, const rv::world::RoomTemplate& roomTemplate, const FixtureId& id) {
    REQUIRE(roomTemplate.doorway.wall == rv::world::Wall::South);
    const rv::world::TemplateSocket* socket = roomTemplate.socket(id.socket());
    REQUIRE(socket != nullptr);
    const rv::world::Footprint home = rv::world::socketFootprint(roomTemplate, *socket, room.growthSteps);
    Home result;
    result.x = (home.minXMm + home.maxXMm) / 2;
    result.z = (home.minZMm + home.maxZMm) / 2;
    const rv::world::DoorwayCamera camera = rv::world::doorwayCamera(room, roomTemplate);
    const std::int64_t eyeX = std::llround(camera.x * 1000.0);
    const std::int64_t eyeZ = std::llround(camera.z * 1000.0);
    result.toDoorway = (result.x - eyeX) * (result.x - eyeX) + (result.z - eyeZ) * (result.z - eyeZ);
    result.depth = room.maxZMm - result.z;
    result.extent = room.maxZMm - room.minZMm;
    for (const rv::world::TemplateSocket& candidate : roomTemplate.sockets) {
        if (candidate.kind == rv::world::SocketKind::HeroCorner) {
            const rv::world::Footprint hero = rv::world::socketFootprint(roomTemplate, candidate, room.growthSteps);
            const std::int64_t dx = result.x - (hero.minXMm + hero.maxXMm) / 2;
            const std::int64_t dz = result.z - (hero.minZMm + hero.maxZMm) / 2;
            result.toHero = dx * dx + dz * dz;
        }
    }
    return result;
}

/// The room's sideboards (its tv_cabinet decorations standing on the floor), by id.
std::vector<FixtureId> sideboardsOf(const rv::world::WorldPlan& plan, const rv::world::RoomId& room) {
    std::vector<FixtureId> ids;
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() == room && fixture.role == FixtureRole::Decoration && fixture.yMm == 0 &&
            fixture.prefab.kind == "tv_cabinet") {
            ids.push_back(fixture.id);
        }
    }
    return ids;
}

/// The placement's rule on `tvs` (I5 step 3's review): never the sideboard nearest the doorway's camera (one alone only
/// where it is preferred); a preferred one (the back half, or within kSecondaryTvNearHeroMm of the hero corner) first
/// whenever another than that one is, in the deepest third of the room's depth among them.
void checkPlacement(const rv::world::WorldPlan& plan, const rv::world::Room& room,
                    const rv::world::RoomTemplate& roomTemplate, const std::vector<FixtureId>& tvs) {
    const std::vector<FixtureId> sideboards = sideboardsOf(plan, room.id);
    if (sideboards.empty()) {
        CHECK(tvs.empty());
        return;
    }
    FixtureId nearest = sideboards.front();
    for (const FixtureId& id : sideboards) {
        const Home home = homeOf(room, roomTemplate, id);
        const Home best = homeOf(room, roomTemplate, nearest);
        if (home.toDoorway < best.toDoorway ||
            (home.toDoorway == best.toDoorway && id.localText() < nearest.localText())) {
            nearest = id;
        }
    }
    const auto preferred = [&](const FixtureId& id) {
        const Home home = homeOf(room, roomTemplate, id);
        return home.backHalf() ||
               home.toHero <= rv::world::kSecondaryTvNearHeroMm * rv::world::kSecondaryTvNearHeroMm;
    };
    bool anyPreferred = false;
    std::int64_t deepest = -1;
    for (const FixtureId& id : sideboards) {
        if ((id != nearest || sideboards.size() == 1) && preferred(id)) {
            anyPreferred = true;
            deepest = std::max(deepest, homeOf(room, roomTemplate, id).band());
        }
    }
    INFO("the room's sideboards " << sideboards.size() << ", the nearest the doorway " << nearest.localText());
    if (sideboards.size() > 1) {
        CHECK(std::find(tvs.begin(), tvs.end(), nearest) == tvs.end());
    }
    if (!tvs.empty() && anyPreferred) {
        INFO("the first TV " << tvs.front().localText());
        CHECK(preferred(tvs.front()));
        // The deepest third of the room's depth from the doorway first.
        CHECK(homeOf(room, roomTemplate, tvs.front()).band() == deepest);
    }
    CHECK(tvs.empty() == (sideboards.size() == 1 && !anyPreferred));
}

} // namespace

TEST_CASE("world/secondary TV: the reader takes one secondary TV in a decoration kind's list, refusing the others",
          "[world][secondary_tv]") {
    const auto parse = [](const std::string& furniture) {
        return rv::world::parseThemeMaterials(
            R"({"schemaVersion": 1, "id": "t", "name": "T", "heroParts": {}, "kinds": {}, "furniture": )" + furniture +
                "}",
            "t.json");
    };
    const auto refusal = [&](const std::string& furniture) {
        auto parsed = parse(furniture);
        REQUIRE_FALSE(parsed.has_value());
        return parsed.error().message;
    };
    const std::string tv = R"({"prefab": {"manifest": "furniture.secondary_tv.generic", "kind": "secondary_tv"},
                               "params": {"drawers": 0}, "materials": {"plastic_body": "MAT_Plastic_Black"}})";
    const std::string showcase = R"({"weight": 1, "prefab": {"manifest": "furniture.showcase.generic",
                                                              "kind": "showcase"}})";
    // Beside a weighted variant: the TV is no variant.
    auto beside = parse(R"({"decoration": {"tv_cabinet": [)" + showcase + ", " + tv + "]}}");
    REQUIRE(beside.has_value());
    REQUIRE(beside->furniture.secondaryTv.has_value());
    CHECK(beside->furniture.secondaryTv->kind == "tv_cabinet");
    CHECK(beside->furniture.secondaryTv->piece.prefab.kind == "secondary_tv");
    CHECK(beside->furniture.secondaryTv->piece.params.at("drawers") == 0);
    CHECK(beside->furniture.secondaryTv->piece.materials.at("plastic_body") == "MAT_Plastic_Black");
    REQUIRE(beside->furniture.decoration.at("tv_cabinet").size() == 1);
    CHECK(beside->furniture.decoration.at("tv_cabinet").front().piece.prefab.kind == "showcase");
    CHECK_FALSE(beside->furniture.empty());
    // Alone in its list: the kind keeps no variant (its other sockets draw the plan's prefab).
    auto alone = parse(R"({"decoration": {"tv_cabinet": [)" + tv + "]}}");
    REQUIRE(alone.has_value());
    CHECK(alone->furniture.secondaryTv.has_value());
    CHECK(alone->furniture.decoration.empty());
    CHECK(alone->furniture.heroParts.empty());
    CHECK_FALSE(alone->furniture.empty());

    // Refusals: a weight (the room's rule places it), a second one, one in the hero corner, a CRT as decoration.
    CHECK_THAT(refusal(R"({"decoration": {"tv_cabinet": [{"weight": 1, "prefab": {"manifest":
                          "furniture.secondary_tv.generic", "kind": "secondary_tv"}}]}})"),
               ContainsSubstring("/furniture/decoration/tv_cabinet/0/weight"));
    CHECK_THAT(refusal(R"({"decoration": {"tv_cabinet": [)" + tv + R"(], "plant": [)" + tv + "]}}"),
               ContainsSubstring("one secondary TV"));
    CHECK_THAT(refusal(R"({"heroParts": {"armchair": [)" + tv + "]}}"),
               ContainsSubstring("/furniture/heroParts/armchair/0/prefab"));
    CHECK_THAT(refusal(R"({"decoration": {"tv_cabinet": [{"weight": 1, "prefab": {"manifest":
                          "electronics.crt.consumer.generic", "kind": "crt"}}]}})"),
               ContainsSubstring("secondary_tv piece"));
    // I5 step 3's review: under a hung kind (it would float on a poster's socket), and a prefab kind of another type
    // refused rather than thrown.
    CHECK_THAT(refusal(R"({"decoration": {"poster_frame": [)" + tv + "]}}"),
               ContainsSubstring("/furniture/decoration/poster_frame/0/prefab"));
    CHECK_THAT(refusal(R"({"decoration": {"poster_frame": [)" + tv + "]}}"), ContainsSubstring("floor-standing"));
    const auto numeric = parse(R"({"decoration": {"tv_cabinet": [{"weight": 1, "prefab": {"manifest":
                                   "furniture.secondary_tv.generic", "kind": 5}}]}})");
    REQUIRE_FALSE(numeric.has_value());
    CHECK_THAT(numeric.error().message, ContainsSubstring("/furniture/decoration/tv_cabinet/0/prefab"));
    for (const std::string_view kind : rv::world::kFloorStandingDecorationKinds) {
        INFO(kind);
        auto floor = parse(R"({"decoration": {")" + std::string(kind) + R"(": [)" + tv + "]}}");
        REQUIRE(floor.has_value());
        CHECK(floor->furniture.secondaryTv->kind == kind);
    }
}

TEST_CASE("world/secondary TV: every pack but the arcade's and the hub's stands one in a sideboard's socket",
          "[world][secondary_tv]") {
    auto families = rv::assets::loadPrefabFamilies(assets());
    REQUIRE(families.has_value());
    for (const std::string& pack : rv::world::themePackIds(assets())) {
        INFO(pack);
        auto theme = rv::world::loadThemeMaterials(assets(), pack);
        REQUIRE(theme.has_value());
        const rv::world::SceneFurniture furniture =
            rv::world::resolveSceneFurniture(assets(), *families, theme->furniture);
        if (pack == "arcade_after_dark" || pack == "retro_library_hub") {
            CHECK_FALSE(theme->furniture.secondaryTv.has_value());
            continue;
        }
        REQUIRE(theme->furniture.secondaryTv.has_value());
        CHECK(theme->furniture.secondaryTv->kind == "tv_cabinet");
        // Resolved and built, its screen a ScreenSurface (the room's clips play on it).
        REQUIRE(furniture.secondaryTv.has_value());
        const rv::world::ScenePiece& piece = furniture.secondaryTv->piece;
        CHECK(piece.prefab.generator == "secondary_tv");
        REQUIRE(piece.prefab.prefab.has_value());
        CHECK(rv::geometry::findSocket(piece.prefab.prefab->sockets, "ScreenSurface") != nullptr);
        // Its footprint the narrow sideboard's (0.8 x 0.36 m: a side-wall socket's decoration), under the ceiling.
        CHECK(piece.prefab.prefab->bounds.size().x <= 0.8f + 1e-4f);
        CHECK(piece.prefab.prefab->bounds.size().z <= 0.36f + 1e-4f);
        CHECK(piece.prefab.prefab->bounds.size().y < 1.2f);
    }
    // The built-in pack is the classic pack's copy, its TV included.
    CHECK(rv::world::builtInThemeMaterials().furniture.secondaryTv ==
          rv::world::loadThemeMaterials(assets(), "classic_16bit_warm")->furniture.secondaryTv);
}

TEST_CASE("world/secondary TV: at most one a room, away from the doorway, in a sideboard's socket, the others as "
          "before",
          "[world][secondary_tv]") {
    std::size_t rooms = 0;
    std::size_t withTv = 0;
    for (const std::string& name : rv::test::world::goldenPlanNames()) {
        const rv::test::world::PlanCase& entry = rv::test::world::planCase(name);
        const rv::world::Room& room = entry.plan.rooms.front();
        for (const std::string pack : {"classic_16bit_warm", "console_90s_collector", "optical_era_lounge"}) {
            INFO(name << " drawn as " << pack);
            const rv::world::SceneData data = withPack(entry.data, pack);
            const std::vector<FixtureId> tvs = tvsOf(entry.plan, entry.room, data);
            ++rooms;
            // At most one in a room of at most 12 m (the golden rooms are 10 and 12 m deep), by the placement's rule.
            REQUIRE(room.maxZMm - room.minZMm <= rv::world::kSecondaryTvTwoPastDepthMm);
            CHECK(tvs.size() <= 1);
            checkPlacement(entry.plan, room, data.roomTemplate, tvs);
            withTv += tvs.empty() ? 0 : 1;
            // The same choice every time.
            CHECK(tvsOf(entry.plan, entry.room, data) == tvs);

            // Drawn as the TV: its fixture's id, the standby picture on its screen, the pack's sideboard and CRT.
            const auto objects = objectsOf(entry.plan, entry.room, data);
            std::size_t drawn = 0;
            for (const auto& [id, object] : objects) {
                drawn += object.value("prefab", "") == "secondary_tv" ? 1 : 0;
            }
            CHECK(drawn == tvs.size());
            for (const FixtureId& id : tvs) {
                const Fixture& fixture = fixtureOf(entry.plan, id);
                CHECK(fixture.role == FixtureRole::Decoration);
                CHECK(fixture.prefab.kind == "tv_cabinet");
                const nlohmann::json& object = objects.at(id.localText());
                CHECK(object.at("prefab") == "secondary_tv");
                CHECK(object.at("textures").at("screen").at("pattern") == "standby");
                // The materials, a later source winning: the kinds' tv_cabinet, crt and secondary_tv, then the
                // piece's own (the 90s collector's charcoal set).
                const nlohmann::json& materials = object.at("materials");
                const std::map<std::string, std::string>& own = data.theme.furniture.secondaryTv->piece.materials;
                const auto expected = [&](const std::string& role, const std::string& kind) {
                    const auto found = own.find(role);
                    return found != own.end() ? found->second : data.theme.materials(kind).at(role);
                };
                CHECK(materials.at("screen") == expected("screen", "crt"));
                CHECK(materials.at("plastic_body") == expected("plastic_body", "crt"));
                CHECK(materials.at("wood_primary") == expected("wood_primary", "tv_cabinet"));
                if (std::string_view(pack) == "console_90s_collector") {
                    CHECK(materials.at("plastic_body") == "MAT_Plastic_Charcoal");
                    CHECK(data.theme.materials("crt").at("plastic_body") == "MAT_Plastic_Gray");
                }
                // It stands on the floor where the sideboard stood, turned as it.
                CHECK(object.at("position")[1] == 0.0);
                CHECK(object.at("rotationDeg")[1].get<double>() == static_cast<double>(fixture.yawCdeg) / 100.0);
            }
            // Every other object is drawn as it was without the TV.
            rv::world::SceneData without = data;
            without.furniture.secondaryTv.reset();
            const auto before = objectsOf(entry.plan, entry.room, without);
            REQUIRE(before.size() == objects.size());
            for (const auto& [id, object] : objects) {
                const bool tv =
                    std::any_of(tvs.begin(), tvs.end(), [&](const FixtureId& f) { return f.localText() == id; });
                if (!tv) {
                    INFO(id);
                    CHECK(before.at(id) == object);
                }
            }
        }
    }
    CHECK(withTv > rooms / 2);
    // A room whose side walls are all modules and whose decoration holds no sideboard has none (plan-2000-seed-7).
    const rv::test::world::PlanCase& full = rv::test::world::planCase("plan-2000-seed-7");
    CHECK(tvsOf(full.plan, full.room, full.data).empty());
}

TEST_CASE("world/secondary TV: two in a room grown past 12 m, in two thirds of its depth; none in the arcade",
          "[world][secondary_tv]") {
    const rv::test::world::PlanCase& entry = rv::test::world::planCase("plan-0-seed-102938");
    const rv::world::SceneData& data = entry.data;
    REQUIRE(decorationsOfKind(entry.plan, entry.room, "tv_cabinet") >= 2);
    const std::vector<FixtureId> one = tvsOf(entry.plan, entry.room, data);
    REQUIRE(one.size() == 1);
    checkPlacement(entry.plan, entry.plan.rooms.front(), data.roomTemplate, one);

    // The rule reads the room's depth (the generator grows a room south in 2 m steps): 12 m holds one, 14 m two.
    for (const std::int64_t depth : {12000, 14000, 16000}) {
        INFO(depth);
        rv::world::WorldPlan grown = entry.plan;
        grown.rooms.front().maxZMm = grown.rooms.front().minZMm + depth;
        const std::vector<FixtureId> tvs = tvsOf(grown, entry.room, data);
        if (depth <= rv::world::kSecondaryTvTwoPastDepthMm) {
            CHECK(tvs == one);
            continue;
        }
        REQUIRE(tvs.size() == 2);
        // The second in another third of the room's depth from the doorway: never a pair mirrored across the room.
        const rv::world::Room& deep = grown.rooms.front();
        CHECK(homeOf(deep, data.roomTemplate, tvs[0]).band() != homeOf(deep, data.roomTemplate, tvs[1]).band());
        checkPlacement(grown, deep, data.roomTemplate, tvs);
        std::size_t drawn = 0;
        for (const auto& [id, object] : objectsOf(grown, entry.room, data)) {
            drawn += object.value("prefab", "") == "secondary_tv" ? 1 : 0;
        }
        CHECK(drawn == 2);
    }

    // The arcade (its cabinets are its screens) never draws one, whatever its pack lists.
    rv::world::WorldPlan arcade = entry.plan;
    arcade.rooms.front().kind = rv::world::RoomKind::Arcade;
    CHECK(tvsOf(arcade, entry.room, data).empty());
    // A pack without a TV draws none.
    CHECK(tvsOf(entry.plan, entry.room, withPack(data, "retro_library_hub")).empty());
}

TEST_CASE("world/secondary TV: it follows its sideboard moved by the room editor, within its footprint, the "
          "validation as it was",
          "[world][secondary_tv]") {
    const rv::test::world::PlanCase& entry = rv::test::world::planCase("plan-0-seed-102938");
    const rv::world::SceneData& data = entry.data;
    const FixtureId chosen = tvsOf(entry.plan, entry.room, data).front();
    const Fixture& home = fixtureOf(entry.plan, chosen);
    const auto objectAt = [&](const rv::world::WorldPlan& plan) {
        return objectsOf(plan, entry.room, data).at(chosen.localText());
    };
    const nlohmann::json atHome = objectAt(entry.plan);

    // Moved 0.3 m along its wall and turned 15 degrees, as the editor may (edit_rule::kTurnStep): still the TV, at the
    // fixture's new place, turned with it.
    rv::world::WorldPlan moved = entry.plan;
    Fixture& fixture = *std::find_if(moved.fixtures.begin(), moved.fixtures.end(),
                                     [&](const Fixture& f) { return f.id == chosen; });
    const bool alongX = home.yawCdeg == 0 || home.yawCdeg == 18000;
    (alongX ? fixture.xMm : fixture.zMm) += 300;
    CHECK(tvsOf(moved, entry.room, data) == std::vector<FixtureId>{chosen});
    const nlohmann::json slid = objectAt(moved);
    CHECK(slid.at("prefab") == "secondary_tv");
    CHECK(slid.at("position")[alongX ? 0 : 2].get<double>() ==
          Catch::Approx(atHome.at("position")[alongX ? 0 : 2].get<double>() + 0.3).margin(1e-6));
    fixture.yawCdeg = (fixture.yawCdeg + 1500) % 36000;
    const nlohmann::json turned = objectAt(moved);
    CHECK(turned.at("prefab") == "secondary_tv");
    CHECK(turned.at("rotationDeg")[1].get<double>() == static_cast<double>(fixture.yawCdeg) / 100.0);

    // Its drawn box within the sideboard's own box wherever it stands: the editor's collisions and the navigation
    // judge the plan's fixture, and the drawn room keeps them.
    for (const rv::world::WorldPlan* plan : std::vector<const rv::world::WorldPlan*>{&entry.plan, &moved}) {
        auto drawn = rv::world::drawnFixtures(*plan, entry.room, data);
        REQUIRE(drawn.has_value());
        const Fixture& tv =
            *std::find_if(drawn->begin(), drawn->end(), [&](const Fixture& f) { return f.id == chosen; });
        const Fixture& planned = fixtureOf(*plan, chosen);
        CHECK(tv.prefab.kind == "secondary_tv");
        CHECK(tv.role == FixtureRole::Decoration);
        const rv::world::FixtureBox tvBox = rv::world::fixtureBox(tv);
        const rv::world::FixtureBox plannedBox = rv::world::fixtureBox(planned);
        CHECK(tvBox.minXMm >= plannedBox.minXMm - 2);
        CHECK(tvBox.maxXMm <= plannedBox.maxXMm + 2);
        CHECK(tvBox.minZMm >= plannedBox.minZMm - 2);
        CHECK(tvBox.maxZMm <= plannedBox.maxZMm + 2);
    }

    // The validation of the plan holding the drawn fixtures: no hard finding, the same soft ones.
    auto roomTemplate = rv::world::loadRoomTemplate(assets(), entry.plan.rooms.front().templateId);
    REQUIRE(roomTemplate.has_value());
    rv::world::ValidationContext context;
    context.templates = {*roomTemplate};
    context.prefabSlots = rv::world::manifestPrefabSlots(assets());
    const rv::world::ValidationReport before = rv::world::validatePlan(entry.plan, context);
    auto drawn = rv::world::drawnFixtures(entry.plan, entry.room, data);
    REQUIRE(drawn.has_value());
    rv::world::WorldPlan drawnPlan = entry.plan;
    drawnPlan.fixtures = *drawn;
    const rv::world::ValidationReport after = rv::world::validatePlan(drawnPlan, context);
    const auto findings = [](const rv::world::ValidationReport& report, rv::world::Severity severity) {
        std::set<std::string> result;
        for (const rv::world::ValidationFinding& finding : report.findings) {
            if (finding.severity == severity) {
                result.insert(finding.rule + " " + finding.subject + ": " + finding.message);
            }
        }
        return result;
    };
    CHECK(findings(after, rv::world::Severity::Hard).empty());
    CHECK(findings(after, rv::world::Severity::Soft) == findings(before, rv::world::Severity::Soft));
}

namespace {

/// A room of 4,300 titles generated with the world seed 1 (world_generation_support.hpp's selection 11): grown to 14 m,
/// its side-wall modules filling it from the hero wall, its only sideboards in its last growth step's side-wall sockets
/// w11 and e11, flanking the doorway.
const rv::world::GenerationResult& grownRoom() {
    static const rv::world::GenerationResult result = rv::test::world::generateRoom(4300, 11, 1);
    return result;
}

} // namespace

TEST_CASE("world/secondary TV: a room the generator grew past 12 m, its sideboards flanking the doorway, holds one, "
          "never the mirrored pair",
          "[world][secondary_tv]") {
    const rv::world::WorldPlan& plan = grownRoom().plan;
    REQUIRE(plan.rooms.size() == 1);
    const rv::world::Room& room = plan.rooms.front();
    CHECK(room.maxZMm - room.minZMm == 14000);
    auto data = rv::world::loadSceneData(assets(), plan, room.id);
    REQUIRE(data.has_value());
    std::set<std::string> sideboards;
    for (const FixtureId& id : sideboardsOf(plan, room.id)) {
        sideboards.insert(id.localText());
    }
    REQUIRE(sideboards == std::set<std::string>{"fx:e11", "fx:w11"});
    // I5 step 3's review: the two stand as far from the doorway's camera, e11 counted the nearest (the smaller id);
    // w11 holds the room's TV, beside the doorway rather than facing it (out of the doorway's view), and no second one
    // (none in another third of the room's depth).
    const std::vector<FixtureId> tvs = tvsOf(plan, room.id, *data);
    REQUIRE(tvs.size() == 1);
    CHECK(tvs.front().localText() == "fx:w11");
    checkPlacement(plan, room, data->roomTemplate, tvs);
    std::size_t drawn = 0;
    for (const auto& [id, object] : objectsOf(plan, room.id, *data)) {
        drawn += object.value("prefab", "") == "secondary_tv" ? 1 : 0;
    }
    CHECK(drawn == 1);
    // The drawn room validates as the plan does.
    rv::world::ValidationContext context;
    context.templates = {data->roomTemplate};
    context.prefabSlots = rv::world::manifestPrefabSlots(assets());
    auto drawnFixtures = rv::world::drawnFixtures(plan, room.id, *data);
    REQUIRE(drawnFixtures.has_value());
    rv::world::WorldPlan drawnPlan = plan;
    drawnPlan.fixtures = *drawnFixtures;
    const rv::world::ValidationReport before = rv::world::validatePlan(plan, context);
    const rv::world::ValidationReport after = rv::world::validatePlan(drawnPlan, context);
    CHECK(after.findings == before.findings);
}

TEST_CASE("world/secondary TV: the grown room's scene file, for a look by hand", "[.secondary_tv_look]") {
    // Writes build/i5/step3/grown-room.scene.json under the source tree: retroverse.exe --scene <it> shows the room of
    // 14 m with its secondary TV beside the doorway (standby: a scene file binds no games).
    const rv::world::WorldPlan& plan = grownRoom().plan;
    const rv::world::Room& room = plan.rooms.front();
    auto data = rv::world::loadSceneData(assets(), plan, room.id);
    REQUIRE(data.has_value());
    auto scene = rv::world::toSceneFile(plan, room.id, *data);
    REQUIRE(scene.has_value());
    const std::filesystem::path file =
        rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR) / "build" / "i5" / "step3" / "grown-room.scene.json";
    std::filesystem::create_directories(file.parent_path());
    REQUIRE(rv::core::writeTextFileAtomic(file, rv::world::sceneFileText(scene->sceneFile)).has_value());
}
