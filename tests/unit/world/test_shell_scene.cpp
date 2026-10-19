// The empty room's shell (I3 step 10b, world::shellSceneDescription): what the app shows while the room is made or when
// no plan can be shown. The room of the template at its base size with its own surfaces, environment and doorway
// camera, as a plan's room has them, the pendant every plan has, and nothing else.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include <rv/scene/scene_description.hpp>
#include <rv/world/generation.hpp>
#include <rv/world/room_template.hpp>
#include <rv/world/scene_builder.hpp>

#include "plan_scene_support.hpp"

TEST_CASE("world/shell_scene: the template's empty room, as a plan's room is drawn", "[world][plan_scene]") {
    const auto roomTemplate =
        rv::world::loadRoomTemplate(rv::test::world::sourceAssetRoot(), rv::world::kDefaultRoomTemplate);
    REQUIRE(roomTemplate.has_value());
    const rv::world::RoomId room = rv::world::RoomId::parse("room:snes").value();
    const auto shell = rv::world::shellSceneDescription(room, *roomTemplate);
    REQUIRE(shell.has_value());
    CHECK(shell->name == "room:snes");
    REQUIRE(shell->objects.size() == 1);
    CHECK(shell->objects.front().id == "room");
    CHECK(shell->objects.front().kind == "room_shell");
    REQUIRE(shell->lights.size() == 1);
    CHECK(shell->lights.front().name == "fx:room.pendant");
    CHECK(shell->lights.front().type == rv::scene::LightType::Point);
    CHECK_FALSE(shell->lights.front().castsShadows);

    // The room of a plan of the base size (no title: no growth) has the same shell, the same pendant, the same
    // environment and the same camera.
    const rv::test::world::PlanCase& empty = rv::test::world::planCase("plan-0-seed-102938");
    const auto planned = rv::world::toSceneDescription(empty.plan, empty.room, empty.data);
    REQUIRE(planned.has_value());
    const auto plannedShell = std::find_if(planned->objects.begin(), planned->objects.end(),
                                           [](const rv::scene::SceneObject& object) { return object.id == "room"; });
    REQUIRE(plannedShell != planned->objects.end());
    CHECK(plannedShell->params == shell->objects.front().params);
    CHECK(plannedShell->transform == shell->objects.front().transform);
    CHECK(plannedShell->materialsByRole == shell->objects.front().materialsByRole);
    const auto plannedPendant =
        std::find_if(planned->lights.begin(), planned->lights.end(),
                     [](const rv::scene::Light& light) { return light.name == "fx:room.pendant"; });
    REQUIRE(plannedPendant != planned->lights.end());
    CHECK(plannedPendant->position == shell->lights.front().position);
    CHECK(plannedPendant->intensity == shell->lights.front().intensity);
    CHECK(planned->camera.position == shell->camera.position);
    CHECK(planned->camera.yawDeg == shell->camera.yawDeg);
    CHECK(planned->camera.pitchDeg == shell->camera.pitchDeg);
    CHECK(planned->environment.clearColor == shell->environment.clearColor);
    CHECK(planned->environment.exposure == shell->environment.exposure);
}
