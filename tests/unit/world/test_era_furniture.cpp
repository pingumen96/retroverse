// The packs' era furniture (I6 step 3b, ADR-0014 §1 amended): every variant of every pack stands within the footprint
// of the hero corner's part or the decoration socket it fills, collides only inside it, leaves the plan's validation
// as it was (no hard finding, the same soft ones: the navigation's reach and passages, the TV's line of sight) and its
// lamps' bulbs where the plan's lights are; the plan itself, its slots and lights are what the scene draws them from.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <rv/assets/prefab_resolver.hpp>
#include <rv/core/file_io.hpp>
#include <rv/geometry/prefabs.hpp>
#include <rv/geometry/socket.hpp>
#include <rv/world/building.hpp>
#include <rv/world/plan_json.hpp>
#include <rv/world/gallery.hpp>
#include <rv/world/room_look.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/scene_builder.hpp>
#include <rv/world/validation.hpp>

#include "plan_scene_support.hpp"

using Catch::Matchers::ContainsSubstring;
using rv::world::Fixture;
using rv::world::FixtureRole;
using rv::world::Footprint;

namespace {

const std::filesystem::path& assets() {
    return rv::test::world::sourceAssetRoot();
}

const std::vector<std::string> kPacks = {"arcade_after_dark",     "classic_16bit_warm", "computer_studio",
                                         "console_90s_collector", "handheld_archive",   "optical_era_lounge",
                                         "retro_library_hub"};

/// A fixture's box on the floor (millimetres) and its heights, as validation.hpp's boxes (the bounds turned by the
/// yaw about the pivot; a poster hung from the middle of its back).
struct FloorBox {
    double minX = 0.0;
    double maxX = 0.0;
    double minZ = 0.0;
    double maxZ = 0.0;
    double minY = 0.0;
    double maxY = 0.0;
};

FloorBox boxOf(const Fixture& fixture) {
    const bool hung = fixture.prefab.kind == "poster_frame";
    const double halfWidth = 0.5 * static_cast<double>(fixture.widthMm);
    const double z0 = hung ? 0.0 : -0.5 * static_cast<double>(fixture.depthMm);
    const double z1 = hung ? static_cast<double>(fixture.depthMm) : 0.5 * static_cast<double>(fixture.depthMm);
    const double radians = static_cast<double>(fixture.yawCdeg) / 100.0 * 3.14159265358979323846 / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    FloorBox box{1e18, -1e18, 1e18, -1e18, 0.0, 0.0};
    for (const double x : {-halfWidth, halfWidth}) {
        for (const double z : {z0, z1}) {
            const double roomX = static_cast<double>(fixture.xMm) + c * x + s * z;
            const double roomZ = static_cast<double>(fixture.zMm) - s * x + c * z;
            box.minX = std::min(box.minX, roomX);
            box.maxX = std::max(box.maxX, roomX);
            box.minZ = std::min(box.minZ, roomZ);
            box.maxZ = std::max(box.maxZ, roomZ);
        }
    }
    box.minY = hung ? static_cast<double>(fixture.yMm) - 0.5 * static_cast<double>(fixture.heightMm)
                    : static_cast<double>(fixture.yMm);
    box.maxY = box.minY + static_cast<double>(fixture.heightMm);
    return box;
}

bool inside(const FloorBox& inner, const FloorBox& outer, double tolerance = 1.5) {
    return inner.minX >= outer.minX - tolerance && inner.maxX <= outer.maxX + tolerance &&
           inner.minZ >= outer.minZ - tolerance && inner.maxZ <= outer.maxZ + tolerance;
}

FloorBox boxOf(const Footprint& footprint) {
    return FloorBox{static_cast<double>(footprint.minXMm), static_cast<double>(footprint.maxXMm),
                    static_cast<double>(footprint.minZMm), static_cast<double>(footprint.maxZMm), 0.0, 0.0};
}

FloorBox merged(const FloorBox& a, const FloorBox& b) {
    return FloorBox{std::min(a.minX, b.minX), std::max(a.maxX, b.maxX), std::min(a.minZ, b.minZ),
                    std::max(a.maxZ, b.maxZ), std::min(a.minY, b.minY), std::max(a.maxY, b.maxY)};
}

std::set<std::string> findingsOf(const rv::world::ValidationReport& report, rv::world::Severity severity) {
    std::set<std::string> findings;
    for (const rv::world::ValidationFinding& finding : report.findings) {
        if (finding.severity == severity) {
            findings.insert(finding.rule + " " + finding.subject + ": " + finding.message);
        }
    }
    return findings;
}

/// What one room drawn with one pack must keep (see the file comment); returns how many fixtures the pack replaced.
/// `before` is validatePlan(plan, context), computed once per plan (it takes seconds in Debug on the building).
std::size_t checkRoom(const rv::world::WorldPlan& plan, const rv::world::RoomId& roomId,
                      const rv::world::SceneData& data, const rv::world::ValidationContext& context,
                      const rv::world::ValidationReport& before) {
    const rv::world::Room& room = *std::find_if(plan.rooms.begin(), plan.rooms.end(),
                                                [&](const rv::world::Room& candidate) { return candidate.id == roomId; });
    auto drawn = rv::world::drawnFixtures(plan, roomId, data);
    REQUIRE(drawn.has_value());
    std::map<rv::world::FixtureId, const Fixture*> planned;
    for (const Fixture& fixture : plan.fixtures) {
        if (fixture.id.room() == roomId) {
            planned.emplace(fixture.id, &fixture);
        }
    }
    // A hero part the pack leaves out (I6 step 6: the arcade's TV cabinet, the hero cabinet standing in its place) is
    // not drawn; every other fixture is.
    std::size_t leftOut = 0;
    for (const auto& [id, fixture] : planned) {
        if (fixture->role != FixtureRole::HeroCorner) {
            continue;
        }
        const auto listed = data.furniture.heroParts.find(id.part());
        if (room.kind == rv::world::RoomKind::Arcade && listed != data.furniture.heroParts.end() &&
            listed->second.empty()) {
            ++leftOut;
            CHECK(std::none_of(drawn->begin(), drawn->end(), [&](const Fixture& candidate) { return candidate.id == id; }));
        }
    }
    // I12 step 3: the hero corner's set adds a fixture of its own per piece that collides, "<socket>.dress-<name>",
    // within the hero corner and under the ceiling.
    std::size_t dressed = 0;
    for (const Fixture& fixture : *drawn) {
        if (!fixture.id.part().starts_with(rv::world::kHeroDressingPartPrefix)) {
            continue;
        }
        INFO(fixture.id.toString() << " drawn as " << fixture.prefab.manifest);
        ++dressed;
        CHECK(fixture.role == FixtureRole::HeroCorner);
        CHECK_FALSE(planned.contains(fixture.id));
        const rv::world::TemplateSocket* socket = data.roomTemplate.socket(fixture.id.socket());
        REQUIRE(socket != nullptr);
        CHECK(inside(boxOf(fixture), boxOf(rv::world::socketFootprint(data.roomTemplate, *socket, room.growthSteps))));
        CHECK(fixture.yMm >= 0);
        CHECK(fixture.yMm + fixture.heightMm <= room.heightMm);
    }
    REQUIRE(drawn->size() + leftOut == planned.size() + dressed);
    std::size_t replaced = 0;
    for (const Fixture& fixture : *drawn) {
        if (fixture.id.part().starts_with(rv::world::kHeroDressingPartPrefix)) {
            continue;
        }
        const Fixture& original = *planned.at(fixture.id);
        INFO(fixture.id.toString() << " drawn as " << fixture.prefab.manifest);
        CHECK(fixture.role == original.role);
        CHECK(fixture.levels == original.levels);
        if (fixture == original) {
            continue;
        }
        ++replaced;
        // Only the hero corner's parts and the decoration change.
        REQUIRE((fixture.role == FixtureRole::HeroCorner || fixture.role == FixtureRole::Decoration));
        const rv::world::TemplateSocket* socket = data.roomTemplate.socket(fixture.id.socket());
        REQUIRE(socket != nullptr);
        const FloorBox box = boxOf(fixture);
        const FloorBox socketBox = boxOf(rv::world::socketFootprint(data.roomTemplate, *socket, room.growthSteps));
        if (fixture.role == FixtureRole::Decoration) {
            // Within the socket it fills, on the floor or hung on its wall, under the ceiling.
            CHECK(inside(box, socketBox));
        } else {
            // Within the hero corner, or within the part's own footprint where the planned part reaches out of it (the
            // lamp beside the TV).
            CHECK(inside(box, merged(socketBox, boxOf(original))));
            if (fixture.id.part() == "lamp") {
                CHECK(inside(box, boxOf(original)));
            }
        }
        CHECK(box.minY >= -0.5);
        CHECK(box.maxY <= static_cast<double>(room.heightMm));
        // A floor covering stays one (walked over), and nothing else becomes one.
        const bool covering = original.yMm == 0 && original.heightMm <= rv::world::kFloorCoveringMaxHeightMm;
        CHECK((fixture.yMm == 0 && fixture.heightMm <= rv::world::kFloorCoveringMaxHeightMm) == covering);
    }

    // The validation of the plan holding the drawn fixtures: no hard finding, the same soft ones (the plan's own when
    // nothing is replaced or added).
    CHECK(findingsOf(before, rv::world::Severity::Hard).empty());
    if (replaced > 0 || dressed > 0) {
        rv::world::WorldPlan drawnPlan = plan;
        drawnPlan.fixtures.erase(std::remove_if(drawnPlan.fixtures.begin(), drawnPlan.fixtures.end(),
                                                [&](const Fixture& fixture) { return fixture.id.room() == roomId; }),
                                 drawnPlan.fixtures.end());
        drawnPlan.fixtures.insert(drawnPlan.fixtures.end(), drawn->begin(), drawn->end());
        const rv::world::ValidationReport after = rv::world::validatePlan(drawnPlan, context);
        CHECK(findingsOf(after, rv::world::Severity::Hard) == findingsOf(before, rv::world::Severity::Hard));
        CHECK(findingsOf(after, rv::world::Severity::Soft) == findingsOf(before, rv::world::Severity::Soft));
    }

    // The scene: every piece's colliders inside its bounds; a lamp's bulb near the plan's light it stands for.
    auto scene = rv::world::toSceneFile(plan, roomId, data);
    REQUIRE(scene.has_value());
    std::vector<glm::vec3> lights;
    for (const rv::world::PlanLight& light : plan.lights) {
        if (light.id.room() == roomId && light.role == rv::world::LightRole::Lamp) {
            lights.emplace_back(static_cast<float>(light.xMm) / 1000.0f, static_cast<float>(light.yMm) / 1000.0f,
                                static_cast<float>(light.zMm) / 1000.0f);
        }
    }
    for (const nlohmann::json& object : scene->sceneFile.at("objects")) {
        if (!object.contains("prefab")) {
            continue;
        }
        const std::string kind = object.at("prefab").get<std::string>();
        auto built = rv::geometry::buildPrefab(kind, object.at("params"));
        REQUIRE(built.has_value());
        for (const rv::geometry::PrefabCollider& collider : built->colliders) {
            CHECK(built->bounds.inflated(1e-5f).contains(collider.box));
        }
        // The hero corner's set holds a table lamp of its own (I12 step 3), lit by its shade alone.
        const bool dressing = object.at("id").get<std::string>().starts_with(rv::world::kHeroDressingObjectPrefix);
        if ((kind != "floor_lamp" && kind != "banker_lamp") || dressing) {
            continue;
        }
        const rv::geometry::Socket* source = rv::geometry::findSocket(built->sockets, "LightSource");
        REQUIRE(source != nullptr);
        const double yaw = object.at("rotationDeg")[1].get<double>() * 3.14159265358979323846 / 180.0;
        const glm::vec3 local(source->transform[3]);
        const glm::vec3 at(static_cast<float>(object.at("position")[0].get<double>() + std::cos(yaw) * local.x +
                                              std::sin(yaw) * local.z),
                           static_cast<float>(object.at("position")[1].get<double>()) + local.y,
                           static_cast<float>(object.at("position")[2].get<double>() - std::sin(yaw) * local.x +
                                              std::cos(yaw) * local.z));
        float nearest = 1e9f;
        for (const glm::vec3& light : lights) {
            nearest = std::min(nearest, glm::length(light - at));
        }
        INFO(object.at("id").get<std::string>());
        CHECK(nearest <= 0.15f);
    }
    return replaced;
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

} // namespace

TEST_CASE("world/era furniture: every pack's pieces resolve and build, the classic packs have none but the TV",
          "[world][era_furniture]") {
    auto families = rv::assets::loadPrefabFamilies(assets());
    REQUIRE(families.has_value());
    for (const std::string& pack : kPacks) {
        INFO(pack);
        auto theme = rv::world::loadThemeMaterials(assets(), pack);
        REQUIRE(theme.has_value());
        const rv::world::SceneFurniture furniture =
            rv::world::resolveSceneFurniture(assets(), *families, theme->furniture);
        CHECK(furniture.heroParts.size() == theme->furniture.heroParts.size());
        CHECK(furniture.decoration.size() == theme->furniture.decoration.size());
        for (const auto& [part, pieces] : theme->furniture.heroParts) {
            CHECK(furniture.heroParts.at(part).size() == pieces.size());
        }
        for (const auto& [kind, variants] : theme->furniture.decoration) {
            CHECK(furniture.decoration.at(kind).size() == variants.size());
        }
        const bool classic = pack == "classic_16bit_warm" || pack == "retro_library_hub";
        // I5 step 3: the classic pack's furniture is its secondary TV alone; the hub's and the arcade's have none.
        CHECK(theme->furniture.heroParts.empty() == classic);
        CHECK(theme->furniture.decoration.empty() == classic);
        // I12 step 3: every console pack's hero corner is a lived-in set; the arcade's cabinets and the hub draw none.
        const bool set = pack != "arcade_after_dark" && pack != "retro_library_hub";
        CHECK(theme->furniture.heroDressing.empty() == !set);
        CHECK(furniture.heroDressing.size() == theme->furniture.heroDressing.size());
        CHECK(theme->furniture.secondaryTv.has_value() == (pack != "retro_library_hub" && pack != "arcade_after_dark"));
        CHECK(furniture.secondaryTv.has_value() == theme->furniture.secondaryTv.has_value());
        if (pack == "arcade_after_dark") {
            // The runner is the room's dressing (I6 step 6's review: 3.5 m, longer than the hero corner), the hero's
            // rug left out; the rug kind stays the ornate carpet, which a plan's rug drawn as planned keeps.
            CHECK(theme->materials("rug").at("fabric") == "MAT_Carpet_Ornate");
            CHECK(theme->furniture.heroParts.at("rug").empty());
            REQUIRE(theme->dressing.runner.has_value());
            CHECK(theme->dressing.runner->material == "MAT_Carpet_Runner");
        }
        if (!classic) {
            // Each era's seat differs from the armchair, and its rug from the classic one (the rug piece's own
            // material, or the rug kind's).
            CHECK(theme->furniture.heroParts.contains("armchair"));
            std::string rug = theme->materials("rug").at("fabric");
            const auto piece = theme->furniture.heroParts.find("rug");
            if (piece != theme->furniture.heroParts.end() && !piece->second.empty() &&
                piece->second.front().materials.contains("fabric")) {
                rug = piece->second.front().materials.at("fabric");
            } else if (theme->dressing.runner) {
                rug = theme->dressing.runner->material;
            }
            CHECK(rug != "MAT_Carpet_Ornate");
            CHECK(theme->furniture.decoration.contains("poster_frame"));
        }
    }
}

TEST_CASE("world/era furniture: the reader refuses a broken piece or a list without weight", "[world][era_furniture]") {
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
    auto good = parse(R"({"heroParts": {"armchair": [{"prefab": {"manifest": "furniture.sofa.generic", "kind": "sofa"},
                                                      "params": {"seats": 3}, "xMm": -100, "zMm": 2450,
                                                      "yawCdeg": 18000}]},
                          "decoration": {"poster_frame": [
                            {"weight": 0.25, "prefab": {"manifest": "decor.poster_frame.generic", "kind": "poster_frame"},
                             "textures": {"picture": "cork_board"}, "materials": {"frame": "MAT_Wood_Oak"}},
                            {"weight": 0.0, "prefab": {"manifest": "decor.poster_frame.generic", "kind": "poster_frame"}}]}})");
    REQUIRE(good.has_value());
    const rv::world::FurniturePiece& sofa = good->furniture.heroParts.at("armchair").front();
    CHECK(sofa.prefab.kind == "sofa");
    CHECK(sofa.params.at("seats") == 3);
    CHECK(sofa.xMm == -100);
    CHECK(sofa.yawCdeg == 18000);
    const auto& posters = good->furniture.decoration.at("poster_frame");
    REQUIRE(posters.size() == 2);
    CHECK(posters[0].weightPermille == 250);
    CHECK(posters[1].weightPermille == 0);
    CHECK(posters[0].piece.textures.at("picture") == "cork_board");
    CHECK(posters[0].piece.materials.at("frame") == "MAT_Wood_Oak");
    CHECK_FALSE(posters[0].piece.xMm.has_value());

    const std::string poster = R"({"manifest": "decor.poster_frame.generic", "kind": "poster_frame"})";
    CHECK_THAT(refusal(R"({"decoration": {"poster_frame": [{"weight": 0.0, "prefab": )" + poster + "}]}}"),
               ContainsSubstring("weight above 0"));
    CHECK_THAT(refusal(R"({"decoration": {"poster_frame": [{"weight": 2, "prefab": )" + poster + "}]}}"),
               ContainsSubstring("/furniture/decoration/poster_frame/0/weight"));
    CHECK_THAT(refusal(R"({"decoration": {"poster_frame": [{"weight": 1, "xMm": 5, "prefab": )" + poster + "}]}}"),
               ContainsSubstring("xMm"));
    // An empty hero part's list leaves the part undrawn (I6 step 6) where the CRT stands as a cabinet in its place,
    // never the CRT's own; without the cabinet the pack is refused (the CRT would float); a decoration's needs pieces.
    const std::string heroCabinet = R"({"prefab": {"manifest": "furniture.arcade_cabinet.generic",
                                                   "kind": "arcade_cabinet"}, "yMm": 0, "zMm": 440})";
    auto leftOut = parse(R"({"heroParts": {"crt": [)" + heroCabinet + R"(], "tv-cabinet": []}})");
    REQUIRE(leftOut.has_value());
    CHECK(leftOut->furniture.heroParts.at("tv-cabinet").empty());
    CHECK_THAT(refusal(R"({"heroParts": {"tv-cabinet": []}})"),
               ContainsSubstring("/furniture/heroParts/tv-cabinet"));
    CHECK_THAT(refusal(R"({"heroParts": {"tv-cabinet": []}})"), ContainsSubstring("heroParts/crt"));
    CHECK_THAT(refusal(R"({"heroParts": {"crt": []}})"), ContainsSubstring("non-empty"));
    CHECK_THAT(refusal(R"({"decoration": {"poster_frame": []}})"), ContainsSubstring("non-empty"));
    // The CRT only as arcade cabinets (I6 step 6's hero cabinet), a piece standing at a height of its own.
    CHECK_THAT(refusal(R"({"heroParts": {"crt": [{"prefab": {"manifest": "furniture.sofa.generic", "kind": "sofa"}}]}})"),
               ContainsSubstring("arcade cabinet"));
    auto cabinet = parse(R"({"heroParts": {"crt": [{"prefab": {"manifest": "furniture.arcade_cabinet.generic",
                                                               "kind": "arcade_cabinet"}, "yMm": 0, "zMm": 440}]}})");
    REQUIRE(cabinet.has_value());
    CHECK(cabinet->furniture.heroParts.at("crt").front().yMm == 0);
    CHECK_THAT(refusal(R"({"heroParts": {"armchair": [{"prefab": )" + poster + R"(, "textures": {"picture": "mona"}}]}})"),
               ContainsSubstring("/furniture/heroParts/armchair/0/textures/picture"));
    CHECK_THAT(refusal(R"({"heroParts": {"armchair": [{"prefab": )" + poster + R"(, "materials": {"frame": "oak"}}]}})"),
               ContainsSubstring("MAT_"));
    CHECK_THAT(refusal(R"({"heroParts": {"armchair": [{"prefab": )" + poster + R"(, "yawCdeg": 36000}]}})"),
               ContainsSubstring("yawCdeg"));
    CHECK_THAT(refusal(R"({"heroParts": {"armchair": [{"prefab": {"manifest": "x", "kind": "Sofa"}}]}})"),
               ContainsSubstring("prefab"));
    CHECK_THAT(refusal(R"({"sofas": {}})"), ContainsSubstring("sofas"));
    // The CRT, the sign and the console are what the room is about, never furniture.
    for (const std::string role : {"crt", "sign", "console"}) {
        INFO(role);
        CHECK_THAT(refusal(R"({"heroParts": {")" + role + R"(": [{"prefab": )" + poster + "}]}}"),
                   ContainsSubstring("/furniture/heroParts/" + role));
    }
}

TEST_CASE("world/era furniture: every pack's variants stand within their footprints in the golden plans' rooms, the "
          "validation and the lamps' lights as they were",
          "[world][era_furniture]") {
    for (const std::string name : {"plan-0-seed-102938", "plan-15-seed-7", "plan-57-seed-102938",
                                   "plan-890-seed-18446744073709551615", "plan-2000-seed-102938"}) {
        const rv::test::world::PlanCase& entry = rv::test::world::planCase(name);
        auto roomTemplate = rv::world::loadRoomTemplate(assets(), entry.plan.rooms.front().templateId);
        REQUIRE(roomTemplate.has_value());
        rv::world::ValidationContext context;
        context.templates = {*roomTemplate};
        context.prefabSlots = rv::world::manifestPrefabSlots(assets());
        const rv::world::ValidationReport before = rv::world::validatePlan(entry.plan, context);
        for (const std::string& pack : kPacks) {
            INFO(name << " drawn as " << pack);
            const rv::world::SceneData data = withPack(entry.data, pack);
            const std::size_t replaced = checkRoom(entry.plan, entry.room, data, context, before);
            const bool classic = pack == "classic_16bit_warm" || pack == "retro_library_hub";
            if (classic) {
                // The classic pack replaces its secondary TV's sideboard alone (I5 step 3), the hub's nothing.
                auto tvs = rv::world::secondaryTvFixtures(entry.plan, entry.room, data);
                REQUIRE(tvs.has_value());
                CHECK(replaced == tvs->size());
                CHECK(tvs->size() <= 1);
            } else {
                CHECK(replaced > 0);
            }
        }
    }
}

TEST_CASE("world/era furniture: the golden building's rooms with their floors' packs keep their footprints and "
          "validation",
          "[world][era_furniture]") {
    auto text = rv::core::readTextFile(rv::test::world::goldenPlanDir() / "building-esde-seed-102938.json");
    REQUIRE(text.has_value());
    auto plan = rv::world::readPlan(*text, "building-esde-seed-102938");
    REQUIRE(plan.has_value());
    auto building = rv::world::loadBuildingData(assets());
    REQUIRE(building.has_value());
    rv::world::ValidationContext context;
    context.building = *building;
    context.prefabSlots = rv::world::manifestPrefabSlots(assets());
    std::set<std::string> templateIds;
    for (const rv::world::Room& room : plan->rooms) {
        if (templateIds.insert(room.templateId).second) {
            auto roomTemplate = rv::world::loadRoomTemplate(assets(), room.templateId);
            REQUIRE(roomTemplate.has_value());
            context.templates.push_back(*roomTemplate);
        }
    }
    const rv::world::ValidationReport before = rv::world::validatePlan(*plan, context);
    std::set<std::string> packs;
    for (const rv::world::Room& room : plan->rooms) {
        INFO(room.id.toString());
        auto data = rv::world::loadSceneData(assets(), *plan, room.id);
        REQUIRE(data.has_value());
        packs.insert(data->theme.id);
        const std::size_t replaced = checkRoom(*plan, room.id, *data, context, before);
        CHECK((replaced > 0) == !data->theme.furniture.empty());
    }
    // The building's rooms draw five packs' furniture besides the classic SNES room's.
    CHECK(packs.size() >= 5);
}

TEST_CASE("world/era furniture: a piece leaving its footprint is not drawn, the plan's prefab is",
          "[world][era_furniture]") {
    const rv::test::world::PlanCase& entry = rv::test::world::planCase("plan-0-seed-102938");
    const rv::world::SceneData data = withPack(entry.data, "optical_era_lounge");
    const auto objectOf = [&](const rv::world::SceneData& sceneData, const std::string& id) {
        auto scene = rv::world::toSceneFile(entry.plan, entry.room, sceneData);
        REQUIRE(scene.has_value());
        nlohmann::json found;
        for (const nlohmann::json& object : scene->sceneFile.at("objects")) {
            if (object.at("id") == id) {
                found = object;
            }
        }
        REQUIRE(found.is_object());
        return found;
    };
    const auto drawnOf = [&](const rv::world::SceneData& sceneData, const rv::world::FixtureId& id) {
        auto drawn = rv::world::drawnFixtures(entry.plan, entry.room, sceneData);
        REQUIRE(drawn.has_value());
        const auto found = std::find_if(drawn->begin(), drawn->end(), [&](const Fixture& f) { return f.id == id; });
        REQUIRE(found != drawn->end());
        return *found;
    };

    // A decoration the pack replaces, in a socket narrowed under its variant: drawn as the plan has it.
    const Fixture* decoration = nullptr;
    for (const Fixture& fixture : entry.plan.fixtures) {
        if (fixture.id.room() == entry.room && fixture.role == FixtureRole::Decoration &&
            data.furniture.decoration.contains(fixture.prefab.kind)) {
            decoration = &fixture;
            break;
        }
    }
    REQUIRE(decoration != nullptr);
    INFO(decoration->id.toString());
    CHECK_FALSE(drawnOf(data, decoration->id) == *decoration);
    rv::world::SceneData narrow = data;
    for (rv::world::TemplateSocket& socket : narrow.roomTemplate.sockets) {
        if (socket.id == decoration->id.socket()) {
            socket.widthMm = 300; // narrower than every variant of the pack
        }
    }
    CHECK(drawnOf(narrow, decoration->id) == *decoration);
    const nlohmann::json planned = objectOf(narrow, decoration->id.localText());
    CHECK(planned.at("prefab") == decoration->prefab.kind);

    // The sofa moved out of the hero corner: the plan's armchair is drawn, with the part's id.
    rv::world::SceneData outside = data;
    REQUIRE(outside.furniture.heroParts.contains("armchair"));
    outside.furniture.heroParts.at("armchair").front().piece.xMm = 2500;
    const Fixture& armchair =
        *std::find_if(entry.plan.fixtures.begin(), entry.plan.fixtures.end(), [&](const Fixture& f) {
            return f.id.room() == entry.room && f.role == FixtureRole::HeroCorner && f.id.part() == "armchair";
        });
    CHECK(objectOf(data, armchair.id.localText()).at("prefab") == "sofa");
    CHECK(objectOf(outside, armchair.id.localText()).at("prefab") == "armchair");
    CHECK(drawnOf(outside, armchair.id) == armchair);
}

TEST_CASE("world/era furniture: the arcade's TV cabinet is left out only under the hero cabinet (I6 step 6's review)",
          "[world][era_furniture]") {
    auto text = rv::core::readTextFile(rv::test::world::goldenPlanDir() / "building-esde-seed-102938.json");
    REQUIRE(text.has_value());
    auto plan = rv::world::readPlan(*text, "building-esde-seed-102938");
    REQUIRE(plan.has_value());
    const auto arcade = std::find_if(plan->rooms.begin(), plan->rooms.end(), [](const rv::world::Room& room) {
        return room.kind == rv::world::RoomKind::Arcade;
    });
    REQUIRE(arcade != plan->rooms.end());
    auto data = rv::world::loadSceneData(assets(), *plan, arcade->id);
    REQUIRE(data.has_value());
    REQUIRE(data->theme.id == "arcade_after_dark");
    auto families = rv::assets::loadPrefabFamilies(assets());
    REQUIRE(families.has_value());
    // The hero corner's CRT and TV cabinet as drawn: their objects' prefabs ("" when not drawn), and whether the TV
    // cabinet covers its floor among the drawn fixtures.
    struct Drawn {
        std::string crt;
        std::string tvCabinet;
        bool tvCabinetCovers = false;
    };
    const auto drawnWith = [&](const rv::world::SceneData& sceneData) {
        auto scene = rv::world::toSceneFile(*plan, arcade->id, sceneData);
        REQUIRE(scene.has_value());
        Drawn result;
        for (const nlohmann::json& object : scene->sceneFile.at("objects")) {
            const std::string id = object.at("id");
            const std::string prefab = object.value("prefab", std::string());
            if (id == "fx:hero.crt") {
                result.crt = prefab;
            } else if (id == "fx:hero.tv-cabinet") {
                result.tvCabinet = prefab;
            }
        }
        auto fixtures = rv::world::drawnFixtures(*plan, arcade->id, sceneData);
        REQUIRE(fixtures.has_value());
        result.tvCabinetCovers = std::any_of(fixtures->begin(), fixtures->end(), [](const Fixture& fixture) {
            return fixture.role == FixtureRole::HeroCorner && fixture.id.part() == "tv-cabinet";
        });
        return result;
    };
    const auto withCrt = [&](const nlohmann::json& patch, const std::string& manifest) {
        rv::world::ThemeFurniture furniture = data->theme.furniture;
        rv::world::FurniturePiece& piece = furniture.heroParts.at("crt").front();
        piece.params.merge_patch(patch);
        if (!manifest.empty()) {
            piece.prefab.manifest = manifest;
        }
        rv::world::SceneData changed = *data;
        changed.furniture = rv::world::resolveSceneFurniture(assets(), *families, furniture);
        return changed;
    };

    // As the pack has it: the hero cabinet in the CRT's place, the TV cabinet left out.
    const Drawn packed = drawnWith(*data);
    CHECK(packed.crt == "arcade_cabinet");
    CHECK(packed.tvCabinet.empty());
    CHECK_FALSE(packed.tvCabinetCovers);
    // A cabinet without a screen surface (its screen a slot), or a manifest that resolves to no cabinet: the CRT is
    // drawn as planned, on its TV cabinet.
    for (const auto& changed : {withCrt(nlohmann::json{{"slots", true}}, {}),
                                withCrt(nlohmann::json::object(), "furniture.sofa.generic")}) {
        const Drawn planned = drawnWith(changed);
        CHECK(planned.crt == "crt");
        CHECK(planned.tvCabinet == "tv_cabinet");
        CHECK(planned.tvCabinetCovers);
    }
    // A cabinet that leaves the hero corner (the guard): the same.
    rv::world::SceneData outside = *data;
    outside.furniture.heroParts.at("crt").front().piece.xMm = 2500;
    const Drawn guarded = drawnWith(outside);
    CHECK(guarded.crt == "crt");
    CHECK(guarded.tvCabinet == "tv_cabinet");
    CHECK(guarded.tvCabinetCovers);
}

TEST_CASE("world/era furniture: the reader takes a pack's room dressing and accents, refusing a broken one (I6 step "
          "6's review)",
          "[world][era_furniture]") {
    const auto parse = [](const std::string& members) {
        return rv::world::parseThemeMaterials(
            R"({"schemaVersion": 1, "id": "t", "name": "T", "heroParts": {}, "kinds": {})" + members + "}", "t.json");
    };
    const auto refusal = [&](const std::string& members) {
        auto parsed = parse(members);
        REQUIRE_FALSE(parsed.has_value());
        return parsed.error().message;
    };
    auto good = parse(R"(, "roomDressing": {
        "cove": {"yMm": 2600, "west": "MAT_Cove_Magenta", "east": "MAT_Cove_Cyan", "westWash": "MAT_Cove_Wash_Magenta",
                 "eastWash": "MAT_Cove_Wash_Cyan"},
        "pictures": {"centreYMm": 2200, "widthMm": 460, "heightMm": 600, "roles": ["hero-module", "side-module"],
                     "kinds": ["tv_cabinet"], "frame": "MAT_Metal_Graphite", "picture": "MAT_Lightbox",
                     "pattern": "poster_flyer"},
        "cans": {"roles": ["key", "wall-washer"], "body": "MAT_Metal_Graphite", "lens": "MAT_Bulb_Warm"},
        "runner": {"widthMm": 900, "lengthMm": 3500, "material": "MAT_Carpet_Runner"},
        "signIcon": {"widthMm": 400, "heightMm": 300, "pattern": "handheld_icon", "material": "MAT_Neon_Cyan"}},
      "accents": [{"name": "aisle", "on": "display", "anchor": "arcade_cabinet", "type": "point",
                   "position": [0, 2.5, 1], "color": "#30C8FF", "colorAlt": "#FF3048", "intensity": 3, "range": 3}])");
    REQUIRE(good.has_value());
    const rv::world::RoomDressing& dressing = good->dressing;
    REQUIRE(dressing.cove.has_value());
    CHECK(dressing.cove->westWash == "MAT_Cove_Wash_Magenta");
    REQUIRE(dressing.pictures.has_value());
    CHECK(dressing.pictures->roles == std::vector<FixtureRole>{FixtureRole::HeroModule, FixtureRole::SideModule});
    CHECK(dressing.pictures->kinds == std::vector<std::string>{"tv_cabinet"});
    REQUIRE(dressing.cans.has_value());
    CHECK(dressing.cans->roles ==
          std::vector<rv::world::LightRole>{rv::world::LightRole::Key, rv::world::LightRole::WallWasher});
    REQUIRE(dressing.runner.has_value());
    CHECK(dressing.runner->lengthMm == 3500);
    REQUIRE(dressing.signIcon.has_value());
    CHECK(dressing.signIcon->pattern == "handheld_icon");
    REQUIRE(good->accents.size() == 1);
    CHECK(good->accents.front().colorAlt == "#FF3048");
    CHECK(parse("")->dressing.empty());
    CHECK(parse(R"(, "roomDressing": {"cardPerSlot": true})")->dressing.cardPerSlot);

    CHECK_THAT(
        refusal(R"(, "roomDressing": {"cove": {"yMm": 2600, "west": "MAT_A", "east": "MAT_B", "westWash": "MAT_C"}})"),
        ContainsSubstring("both or neither"));
    CHECK_THAT(refusal(R"(, "roomDressing": {"cans": {"roles": ["pendant"], "body": "MAT_A", "lens": "MAT_B"}})"),
               ContainsSubstring("spot light"));
    CHECK_THAT(refusal(R"(, "roomDressing": {"pictures": {"centreYMm": 2200, "widthMm": 400, "heightMm": 400,
                                                          "roles": ["attic"], "frame": "MAT_A", "picture": "MAT_B",
                                                          "pattern": "cover"}})"),
               ContainsSubstring("fixture role"));
    CHECK_THAT(refusal(R"(, "roomDressing": {"signIcon": {"widthMm": 400, "heightMm": 300, "pattern": "mona",
                                                          "material": "MAT_A"}})"),
               ContainsSubstring("texture pattern"));
    CHECK_THAT(refusal(R"(, "roomDressing": {"runner": {"widthMm": 900, "lengthMm": 3500, "material": "red"}})"),
               ContainsSubstring("MAT_"));
    CHECK_THAT(refusal(R"(, "roomDressing": {"skylight": {}})"), ContainsSubstring("skylight"));
    CHECK_THAT(refusal(R"(, "accents": [{"name": "glow", "anchor": "sign", "type": "point", "position": [0, 0, 0.3],
                                         "color": "#FFFFFF", "colorAlt": "#000000", "intensity": 1, "range": 1}])"),
               ContainsSubstring("alternates"));
}

TEST_CASE("world/era furniture: the arcade's and the handhelds' room dressing in their scenes (I6 step 6's review)",
          "[world][era_furniture]") {
    auto text = rv::core::readTextFile(rv::test::world::goldenPlanDir() / "building-esde-seed-102938.json");
    REQUIRE(text.has_value());
    auto plan = rv::world::readPlan(*text, "building-esde-seed-102938");
    REQUIRE(plan.has_value());
    const auto sceneOf = [&](const std::string& id) {
        const rv::world::RoomId room = *rv::world::RoomId::parse(id);
        auto data = rv::world::loadSceneData(assets(), *plan, room);
        REQUIRE(data.has_value());
        auto scene = rv::world::toSceneFile(*plan, room, *data);
        REQUIRE(scene.has_value());
        return scene->sceneFile;
    };
    const auto withPrefix = [](const nlohmann::json& scene, const std::string& prefix) {
        std::vector<nlohmann::json> found;
        for (const nlohmann::json& object : scene.at("objects")) {
            if (object.at("id").get<std::string>().rfind(prefix, 0) == 0) {
                found.push_back(object);
            }
        }
        return found;
    };

    // The arcade: two coves (magenta west, cyan east) with their washes, a flyer over each module and showcase against
    // a wall, a can over each key and washer spot, the runner; none collides or casts a shadow; the pairs' accents
    // alternate their colours; every prefab builds.
    const nlohmann::json arcade = sceneOf("area:arcade");
    for (const nlohmann::json& object : withPrefix(arcade, "dressing:")) {
        INFO(object.at("id").get<std::string>());
        CHECK(object.at("castsShadows") == false);
        CHECK(object.at("collides") == false);
        if (object.contains("prefab")) {
            CHECK(rv::geometry::buildPrefab(object.at("prefab").get<std::string>(), object.at("params")).has_value());
        }
    }
    CHECK(withPrefix(arcade, "dressing:cove-w").size() == 3);
    CHECK(withPrefix(arcade, "dressing:cove-e").size() == 3);
    CHECK(withPrefix(arcade, "dressing:cove-w").front().at("material") == "MAT_Cove_Magenta");
    std::size_t modules = 0;
    std::size_t showcases = 0;
    std::size_t spots = 0;
    for (const Fixture& fixture : plan->fixtures) {
        if (fixture.id.room().toString() != "area:arcade") {
            continue;
        }
        modules += fixture.role == FixtureRole::HeroModule || fixture.role == FixtureRole::SideModule ? 1 : 0;
        showcases += fixture.role == FixtureRole::Decoration && fixture.prefab.kind == "tv_cabinet" ? 1 : 0;
    }
    for (const rv::world::PlanLight& light : plan->lights) {
        const bool spot = light.role == rv::world::LightRole::Key || light.role == rv::world::LightRole::WallWasher;
        spots += light.id.room().toString() == "area:arcade" && spot ? 1 : 0;
    }
    // Since I12 step 2 the modules take the gallery's lightboxes (the games' marquees), the showcases keep the flyers.
    CHECK(withPrefix(arcade, "dressing:picture:").size() == showcases);
    for (const nlohmann::json& flyer : withPrefix(arcade, "dressing:picture:")) {
        CHECK(flyer.at("textures").at("picture").at("pattern") == "poster_flyer");
        CHECK(flyer.at("position")[1].get<double>() - 0.3 > 1.7); // above the cabinets
    }
    const auto lightboxes = withPrefix(arcade, "dressing:gallery:");
    CHECK(lightboxes.size() == modules);
    for (const nlohmann::json& lightbox : lightboxes) {
        CHECK(lightbox.at("materials").at("picture") == "MAT_Lightbox");
        CHECK(lightbox.at("textures").at("picture").at("pattern") == "marquee");
        const double aspect = (lightbox.at("params").at("width").get<double>() - 0.05) /
                              (lightbox.at("params").at("height").get<double>() - 0.05);
        CHECK(rv::world::galleryKindOfAspect(aspect) == rv::world::GalleryKind::Lightbox);
        // Under the coves (2.6 m): the cove's light washes the wall over them.
        CHECK(lightbox.at("position")[1].get<double>() + lightbox.at("params").at("height").get<double>() / 2 < 2.6);
    }
    CHECK(withPrefix(arcade, "dressing:can:").size() == 3 * spots); // a can, its lens, its stem
    const auto runner = withPrefix(arcade, "dressing:runner");
    REQUIRE(runner.size() == 1);
    CHECK(runner.front().at("params").at("depth") == 3.5);
    CHECK(withPrefix(arcade, "fx:hero.rug").empty()); // the hero's rug left out for the runner
    std::vector<std::string> aisle;
    for (const nlohmann::json& light : arcade.at("lights")) {
        if (light.at("name").get<std::string>().rfind("accent:aisle_glow:", 0) == 0) {
            aisle.push_back(light.at("color").get<std::string>());
        }
    }
    REQUIRE(aisle.size() >= 2);
    CHECK(aisle[0] == "#30C8FF");
    CHECK(aisle[1] == "#FF3048");
    CHECK(arcade.at("lights").size() <= rv::world::kMaxSceneLights);

    // The gb room: posters over its shelves and showcases, the pictogram beside its sign; its counters' empty levels
    // a card per slot at the item's shape (never a cover stretched across a level).
    const nlohmann::json gb = sceneOf("room:gb");
    CHECK_FALSE(withPrefix(gb, "dressing:picture:").empty());
    const auto icon = withPrefix(gb, "dressing:sign-icon");
    REQUIRE(icon.size() == 1);
    CHECK(icon.front().at("textures").at("picture").at("pattern") == "handheld_icon");
    // The handheld floor's shared room's display cases (not counters: the pack's "cardPerSlot") too.
    for (const nlohmann::json& room : {gb, sceneOf("area:shared:handheld")}) {
        std::size_t cards = 0;
        for (const nlohmann::json& object : room.at("objects")) {
            const std::string id = object.at("id");
            if (id.rfind("dress:fx:dc", 0) == 0) {
                INFO(room.at("name").get<std::string>() << " " << id);
                CHECK(object.at("prefab") == "poster_frame");
                const double width = object.at("params").at("width");
                const double height = object.at("params").at("height");
                CHECK(width / height < 1.5);
                CHECK(width < 0.14);
                CHECK(rv::geometry::buildPrefab("poster_frame", object.at("params")).has_value());
                ++cards;
            }
        }
        CHECK(cards > 0);
    }
    // The counters lit inside, their handhelds' displays lit.
    std::size_t counters = 0;
    for (const nlohmann::json& object : gb.at("objects")) {
        if (object.value("prefab", "") == "display_case") {
            CHECK(object.at("params").value("counter", false));
            CHECK(object.at("params").value("litScreens", false));
            CHECK(object.at("params").value("boardLight", false));
            ++counters;
        }
    }
    CHECK(counters > 0);
}

namespace {

/// The hero corner's set of a scene (I12 step 3): each "dressing:hero:" object's box in the room's frame (millimetres).
std::map<std::string, FloorBox> heroDressingBoxes(const nlohmann::json& sceneFile) {
    std::map<std::string, FloorBox> boxes;
    for (const nlohmann::json& object : sceneFile.at("objects")) {
        const std::string id = object.at("id").get<std::string>();
        if (!id.starts_with(rv::world::kHeroDressingObjectPrefix)) {
            continue;
        }
        auto built = rv::geometry::buildPrefab(object.at("prefab").get<std::string>(), object.at("params"));
        REQUIRE(built.has_value());
        const double yaw = object.at("rotationDeg")[1].get<double>() * 3.14159265358979323846 / 180.0;
        const double c = std::cos(yaw);
        const double s = std::sin(yaw);
        const nlohmann::json& at = object.at("position");
        FloorBox box{1e18, -1e18, 1e18, -1e18, 1e18, -1e18};
        for (const double x : {built->bounds.min.x, built->bounds.max.x}) {
            for (const double z : {built->bounds.min.z, built->bounds.max.z}) {
                const double roomX = 1000.0 * (at[0].get<double>() + c * x + s * z);
                const double roomZ = 1000.0 * (at[2].get<double>() - s * x + c * z);
                box.minX = std::min(box.minX, roomX);
                box.maxX = std::max(box.maxX, roomX);
                box.minZ = std::min(box.minZ, roomZ);
                box.maxZ = std::max(box.maxZ, roomZ);
            }
        }
        box.minY = 1000.0 * (at[1].get<double>() + built->bounds.min.y);
        box.maxY = 1000.0 * (at[1].get<double>() + built->bounds.max.y);
        boxes.emplace(id.substr(rv::world::kHeroDressingObjectPrefix.size()), box);
    }
    return boxes;
}

/// Whether two boxes overlap by more than a millimetre of rounding (a piece standing on another touches its top).
bool overlaps(const FloorBox& a, const FloorBox& b) {
    constexpr double kTouch = 1.0;
    return a.minX < b.maxX - kTouch && b.minX < a.maxX - kTouch && a.minZ < b.maxZ - kTouch &&
           b.minZ < a.maxZ - kTouch && a.minY < b.maxY - kTouch && b.minY < a.maxY - kTouch;
}

} // namespace

TEST_CASE("world/era furniture: the hero corner's set stands clear of the TV, the console and the plaque (I12 step 3)",
          "[world][era_furniture][hero_dressing]") {
    const rv::test::world::PlanCase& entry = rv::test::world::planCase("plan-57-seed-102938");
    const rv::world::Room& room = *std::find_if(entry.plan.rooms.begin(), entry.plan.rooms.end(),
                                                [&](const rv::world::Room& r) { return r.id == entry.room; });
    const rv::world::TemplateSocket* hero = entry.data.roomTemplate.socket("hero");
    REQUIRE(hero != nullptr);
    // The hero corner's frame in the room: its origin at the socket's pivot (yaw 0 in the console room).
    REQUIRE(hero->yawCdeg == 0);
    const double ox = static_cast<double>(hero->xMm);
    const double oz = static_cast<double>(hero->zMm);
    // The cabinet's top and what stands on it (the CRT, the console model's reserve) up to 0.6 m above it; the plaque.
    const FloorBox cabinetTop{ox - 600.0, ox + 600.0, oz, oz + 500.0, 550.0, 1150.0};
    const FloorBox plaque{ox + 745.0, ox + 1165.0, oz, oz + 60.0, 1200.0, 1500.0};
    for (const std::string& pack : kPacks) {
        INFO(pack);
        const rv::world::SceneData data = withPack(entry.data, pack);
        auto scene = rv::world::toSceneFile(entry.plan, entry.room, data);
        REQUIRE(scene.has_value());
        const std::map<std::string, FloorBox> boxes = heroDressingBoxes(scene->sceneFile);
        CHECK(boxes.size() == data.furniture.heroDressing.size());
        if (pack == "classic_16bit_warm") {
            CHECK(boxes.size() >= 10);
        }
        for (const auto& [name, box] : boxes) {
            INFO(name);
            CHECK_FALSE(overlaps(box, cabinetTop));
            CHECK_FALSE(overlaps(box, plaque));
            CHECK(box.maxY <= static_cast<double>(room.heightMm));
        }
        // Pieces never stand in one another, a prop on its support aside (the support's top is its bottom).
        for (auto a = boxes.begin(); a != boxes.end(); ++a) {
            for (auto b = std::next(a); b != boxes.end(); ++b) {
                INFO(a->first << " and " << b->first);
                CHECK_FALSE(overlaps(a->second, b->second));
            }
        }
    }
}

TEST_CASE("world/era furniture: the hero corner's set goes with its anchor part, a piece leaving the corner is not "
          "drawn (I12 step 3)",
          "[world][era_furniture][hero_dressing]") {
    const rv::test::world::PlanCase& entry = rv::test::world::planCase("plan-57-seed-102938");
    rv::world::SceneData data = withPack(entry.data, "classic_16bit_warm");
    const auto names = [&](const rv::world::WorldPlan& plan) {
        auto scene = rv::world::toSceneFile(plan, entry.room, data);
        REQUIRE(scene.has_value());
        std::set<std::string> out;
        for (const auto& [name, box] : heroDressingBoxes(scene->sceneFile)) {
            out.insert(name);
        }
        return out;
    };
    const std::set<std::string> all = names(entry.plan);
    CHECK(all.contains("pad-rug"));
    CHECK(all.contains("speaker-left"));
    // A room without its console (a shared room's hero corner) draws no pads.
    rv::world::WorldPlan withoutConsole = entry.plan;
    std::erase_if(withoutConsole.fixtures, [&](const Fixture& fixture) {
        return fixture.id.room() == entry.room && fixture.role == FixtureRole::HeroCorner &&
               fixture.id.part() == "console";
    });
    const std::set<std::string> consoleless = names(withoutConsole);
    CHECK_FALSE(consoleless.contains("pad-rug"));
    CHECK_FALSE(consoleless.contains("pad-rack"));
    CHECK(consoleless.contains("speaker-left"));
    CHECK(consoleless.size() + 2 == all.size());
    // A piece pushed out of the hero corner is not drawn; the others are.
    for (rv::world::SceneHeroDressing& piece : data.furniture.heroDressing) {
        if (piece.name == "speaker-left") {
            piece.piece.piece.xMm = -1500;
        }
    }
    const std::set<std::string> pushed = names(entry.plan);
    CHECK_FALSE(pushed.contains("speaker-left"));
    CHECK(pushed.size() + 1 == all.size());
    // The props never collide; the rest does, and a moved anchor takes its pieces with it.
    auto scene = rv::world::toSceneFile(entry.plan, entry.room, withPack(entry.data, "classic_16bit_warm"));
    REQUIRE(scene.has_value());
    for (const nlohmann::json& object : scene->sceneFile.at("objects")) {
        const std::string id = object.at("id").get<std::string>();
        if (id == "dressing:hero:cushion" || id == "dressing:hero:pad-rug") {
            CHECK(object.at("collides") == false);
        }
        if (id == "dressing:hero:coffee-table") {
            CHECK_FALSE(object.contains("collides"));
        }
    }
    rv::world::WorldPlan moved = entry.plan;
    for (Fixture& fixture : moved.fixtures) {
        if (fixture.id.room() == entry.room && fixture.id.part() == "tv-cabinet") {
            fixture.xMm -= 40;
        }
    }
    const auto boxesOf = [&](const rv::world::WorldPlan& plan) {
        auto built = rv::world::toSceneFile(plan, entry.room, withPack(entry.data, "classic_16bit_warm"));
        REQUIRE(built.has_value());
        return heroDressingBoxes(built->sceneFile);
    };
    const std::map<std::string, FloorBox> before = boxesOf(entry.plan);
    const std::map<std::string, FloorBox> after = boxesOf(moved);
    CHECK(after.at("rack").minX == Catch::Approx(before.at("rack").minX - 40.0).margin(0.01));
    CHECK(after.at("coffee-table").minX == Catch::Approx(before.at("coffee-table").minX).margin(0.01));
}

TEST_CASE("world/era furniture: the reader takes the hero corner's set, refusing a broken one (I12 step 3)",
          "[world][era_furniture][hero_dressing]") {
    const auto parse = [](const std::string& list) {
        return rv::world::parseThemeMaterials(
            R"({"schemaVersion": 1, "id": "t", "name": "T", "heroParts": {}, "kinds": {},
                "furniture": {"heroDressing": )" +
                list + "}}",
            "t.json");
    };
    const auto refusal = [&](const std::string& list) {
        auto parsed = parse(list);
        REQUIRE_FALSE(parsed.has_value());
        return parsed.error().message;
    };
    const std::string pad = R"("prefab": {"manifest": "decor.pad.generic", "kind": "pad"})";
    auto good = parse(R"([{"name": "pad-rug", "anchor": "console", "prop": true, )" + pad +
                      R"(, "params": {"style": "grip"}, "xMm": 150, "yMm": 12, "zMm": 900, "yawCdeg": 2000},
                         {"name": "rack", "anchor": "tv-cabinet",
                          "prefab": {"manifest": "furniture.media_rack.generic", "kind": "media_rack"},
                          "xMm": 1000, "zMm": 250}])");
    REQUIRE(good.has_value());
    REQUIRE(good->furniture.heroDressing.size() == 2);
    const rv::world::HeroDressingPiece& first = good->furniture.heroDressing.front();
    CHECK(first.name == "pad-rug");
    CHECK(first.anchor == "console");
    CHECK(first.prop);
    CHECK(first.piece.params.at("style") == "grip");
    CHECK(first.piece.yMm == 12);
    CHECK(first.piece.yawCdeg == 2000);
    CHECK_FALSE(good->furniture.heroDressing.back().prop);
    CHECK_FALSE(good->furniture.heroDressing.back().piece.yMm.has_value());
    CHECK_FALSE(good->furniture.empty());

    CHECK_THAT(refusal(R"({})"), ContainsSubstring("array"));
    CHECK_THAT(refusal(R"([{"anchor": "console", )" + pad + R"(, "xMm": 0, "zMm": 0}])"),
               ContainsSubstring("/furniture/heroDressing/0/name"));
    CHECK_THAT(refusal(R"([{"name": "Pad", "anchor": "console", )" + pad + R"(, "xMm": 0, "zMm": 0}])"),
               ContainsSubstring("socket name"));
    CHECK_THAT(refusal(R"([{"name": "a-name-far-longer-than-a-part", "anchor": "console", )" + pad +
                       R"(, "xMm": 0, "zMm": 0}])"),
               ContainsSubstring("26"));
    CHECK_THAT(refusal(R"([{"name": "pad", "anchor": "console", )" + pad + R"(, "xMm": 0, "zMm": 0},
                           {"name": "pad", "anchor": "console", )" +
                       pad + R"(, "xMm": 0, "zMm": 0}])"),
               ContainsSubstring("repeats"));
    CHECK_THAT(refusal(R"([{"name": "pad", "anchor": "sign", )" + pad + R"(, "xMm": 0, "zMm": 0}])"),
               ContainsSubstring("/furniture/heroDressing/0/anchor"));
    CHECK_THAT(refusal(R"([{"name": "pad", "anchor": "console", "prop": 1, )" + pad + R"(, "xMm": 0, "zMm": 0}])"),
               ContainsSubstring("/furniture/heroDressing/0/prop"));
    CHECK_THAT(refusal(R"([{"name": "pad", "anchor": "console", )" + pad + R"(, "xMm": 0}])"),
               ContainsSubstring("zMm"));
    CHECK_THAT(refusal(R"([{"name": "pad", "anchor": "console", )" + pad + R"(, "xMm": 0, "zMm": 0, "weight": 1}])"),
               ContainsSubstring("weight"));
    for (const std::string kind : {"crt", "secondary_tv", "arcade_cabinet"}) {
        INFO(kind);
        CHECK_THAT(refusal(R"([{"name": "tv", "anchor": "console", "prefab": {"manifest": "x.generic", "kind": ")" +
                           kind + R"("}, "xMm": 0, "zMm": 0}])"),
                   ContainsSubstring("no screen"));
    }
}
