// Expansion rooms' signs (I4b step 8, ADR-0015 §7: "its portal and neon sign show the head's sign with its number"), on
// the overflow fixture's counts generated fresh (room:amiga and room:amiga:2 on the computer floor, area:arcade and
// area:arcade:2 on the ground floor's hub): the hall's portal and blade signs, the hub's fascia signs and each room's
// own neon sign of an expansion are its head's with its number; a head's stay as they were.

#include "world_expansion_support.hpp"

#include <rv/world/floor_scene.hpp>
#include <rv/world/scene_builder.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace rv::test::world;
using rv::world::RoomId;

constexpr std::uint64_t kWorldSeed = 102938;

const nlohmann::json* findObject(const nlohmann::json& scene, std::string_view id) {
    for (const nlohmann::json& object : scene["objects"]) {
        if (object["id"].get<std::string>() == id) {
            return &object;
        }
    }
    return nullptr;
}

std::string titleOf(const nlohmann::json* object) {
    REQUIRE(object != nullptr);
    return (*object)["textures"]["picture"]["title"].get<std::string>();
}

/// The titles of the neon signs among `room`'s objects of the floor's scene.
std::vector<std::string> neonTitles(const nlohmann::json& scene, std::string_view room) {
    std::vector<std::string> titles;
    for (const nlohmann::json& object : scene["objects"]) {
        if (!object["id"].get<std::string>().starts_with(std::string(room) + "/") || !object.contains("textures")) {
            continue;
        }
        const nlohmann::json& textures = object["textures"];
        if (textures.contains("picture") && textures["picture"].value("pattern", "") == "neon") {
            titles.push_back(textures["picture"]["title"].get<std::string>());
        }
    }
    return titles;
}

} // namespace

TEST_CASE("world/expansion_signs: a chain's sign with the room's number", "[world][expansion]") {
    const RoomId head = roomId("room:amiga");
    CHECK(rv::world::chainSignText("HOME COMPUTERS", head) == "HOME COMPUTERS");
    CHECK(rv::world::chainSignText("HOME COMPUTERS", roomId("room:amiga:2")) == "HOME COMPUTERS 2");
    CHECK(rv::world::chainSignText("ARCADE", roomId("area:arcade:12")) == "ARCADE 12");
    CHECK(rv::world::chainSignText("", roomId("area:shared:gen4:3")) == "3");
    CHECK(rv::world::chainSignText("", head).empty());
}

TEST_CASE("world/expansion_signs: an expansion's portal, fascia and neon signs show its head's with its number",
          "[world][expansion]") {
    BuildingWorld world = shippedWorld(kWorldSeed);
    const rv::world::GenerationResult result = generateBuildingOf(world, overflowSelections());
    REQUIRE(result.validation.passed());
    const rv::world::WorldPlan& plan = result.plan;

    // The computer floor's hall: the portals' plaques and blade signs.
    auto computer = rv::world::loadFloorSceneData(world.root, plan, "computer", std::nullopt, world.tags);
    REQUIRE(computer.has_value());
    auto computerScene = rv::world::toFloorSceneFile(plan, "computer", *computer);
    REQUIRE(computerScene.has_value());
    const nlohmann::json& hall = computerScene->sceneFile;
    const std::string sign = computer->rooms.at(roomId("room:amiga")).platform.signText;
    REQUIRE_FALSE(sign.empty());
    CHECK(computer->rooms.at(roomId("room:amiga:2")).platform.signText == sign + " 2");
    CHECK(titleOf(findObject(hall, "hall:computer/portal:room:amiga/sign")) == sign);
    CHECK(titleOf(findObject(hall, "hall:computer/portal:room:amiga:2/sign")) == sign + " 2");
    // Its blade signs (one per side the hall's portal shows one on): at least one, each numbered.
    std::size_t blades = 0;
    for (const std::string_view side : {"east", "west"}) {
        const std::string blade = "hall:computer/portal:room:amiga:2/blade-" + std::string(side);
        if (const nlohmann::json* object = findObject(hall, blade)) {
            ++blades;
            CHECK(titleOf(object) == sign + " 2");
        }
    }
    REQUIRE(blades > 0);
    // Each room's own neon sign (its hero corner's): the head's as it was, the expansion's numbered.
    const std::vector<std::string> headNeon = neonTitles(hall, "room:amiga");
    const std::vector<std::string> expansionNeon = neonTitles(hall, "room:amiga:2");
    REQUIRE_FALSE(headNeon.empty());
    REQUIRE_FALSE(expansionNeon.empty());
    for (const std::string& title : headNeon) {
        CHECK(title == sign);
    }
    for (const std::string& title : expansionNeon) {
        CHECK(title == sign + " 2");
    }

    // The ground floor's hub: the arcade's expansion signed on its portal's plaque and on the balcony's fascia.
    auto ground = rv::world::loadFloorSceneData(world.root, plan, rv::world::kGroundFloor, std::nullopt, world.tags);
    REQUIRE(ground.has_value());
    auto groundScene = rv::world::toFloorSceneFile(plan, rv::world::kGroundFloor, *ground);
    REQUIRE(groundScene.has_value());
    const nlohmann::json& hub = groundScene->sceneFile;
    CHECK(titleOf(findObject(hub, "hall:ground/hub/fascia-sign:area:arcade")) == "ARCADE");
    CHECK(titleOf(findObject(hub, "hall:ground/hub/fascia-sign:area:arcade:2")) == "ARCADE 2");
    CHECK(titleOf(findObject(hub, "hall:ground/portal:area:arcade:2/sign")) == "ARCADE 2");
}
