// The packs' era furniture (I6 step 3b, ADR-0014 §1 amended): every variant of every pack stands within the footprint
// of the hero corner's part or the decoration socket it fills, collides only inside it, leaves the plan's validation
// as it was (no hard finding, the same soft ones: the navigation's reach and passages, the TV's line of sight) and its
// lamps' bulbs where the plan's lights are; the plan itself, its slots and lights are what the scene draws them from.

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
    REQUIRE(drawn->size() == planned.size());
    std::size_t replaced = 0;
    for (const Fixture& fixture : *drawn) {
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
    // nothing is replaced).
    CHECK(findingsOf(before, rv::world::Severity::Hard).empty());
    if (replaced > 0) {
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
        if (kind != "floor_lamp" && kind != "banker_lamp") {
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

TEST_CASE("world/era furniture: every pack's pieces resolve and build, the classic packs have none",
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
        CHECK(theme->furniture.empty() == classic);
        if (pack == "arcade_after_dark") {
            // The runner's material is its piece's: the rug kind stays the ornate carpet, which a plan's rug drawn as
            // planned (the piece left out) keeps instead of a runner stretched over 2.4 x 1.6 m.
            CHECK(theme->materials("rug").at("fabric") == "MAT_Carpet_Ornate");
            CHECK(theme->furniture.heroParts.at("rug").front().materials.at("fabric") == "MAT_Carpet_Runner");
        }
        if (!classic) {
            // Each era's seat differs from the armchair, and its rug from the classic one (the rug piece's own
            // material, or the rug kind's).
            CHECK(theme->furniture.heroParts.contains("armchair"));
            std::string rug = theme->materials("rug").at("fabric");
            if (const auto piece = theme->furniture.heroParts.find("rug");
                piece != theme->furniture.heroParts.end() && piece->second.front().materials.contains("fabric")) {
                rug = piece->second.front().materials.at("fabric");
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
    CHECK_THAT(refusal(R"({"heroParts": {"armchair": []}})"), ContainsSubstring("non-empty"));
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
                CHECK(replaced == 0);
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
