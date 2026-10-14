// Free-roam navigation through the M0 test room (GPU-free): the scene's collision world (scene::buildCollisionWorld of
// the real prefab geometry), the first-person controller and the verification input scripts of tests/input_scripts,
// played frame by frame the way retroverse.exe plays them with --fixed-dt 0.016667. The app runs of those scripts must
// log the same final poses.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <rv/assets/material_library.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/input/action_state.hpp>
#include <rv/input/input_script.hpp>
#include <rv/input/input_system.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/first_person_controller.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

using Catch::Approx;
using rv::input::Action;

namespace {

constexpr float kFixedDt = 0.016667f;

std::filesystem::path sourceDir() {
    return rv::core::pathFromUtf8(RV_TEST_SOURCE_DIR);
}

rv::input::InputScript loadScript(std::string_view name) {
    auto script = rv::input::InputScript::loadFromFile(sourceDir() / "tests" / "input_scripts" /
                                                       rv::core::pathFromUtf8(std::string(name) + ".json"));
    if (!script) {
        FAIL(script.error().toString());
    }
    return std::move(*script);
}

/// The M0 test room with a walker at the scene camera, as FreeRoam sets it up.
struct TestRoomWalker {
    rv::scene::SceneDescription scene;
    rv::scene::CollisionWorld world;
    rv::scene::FirstPersonController controller;
    rv::scene::Camera camera;

    TestRoomWalker() {
        auto description = rv::scene::loadSceneDescription(sourceDir() / "assets" / "style_tests" / "reference_scenes" /
                                                           "m0_testroom.json");
        REQUIRE(description.has_value());
        scene = std::move(*description);
        auto materials =
            rv::assets::MaterialLibrary::loadFromFile(sourceDir() / "assets" / "materials" / "base" / "materials.json");
        REQUIRE(materials.has_value());
        world = rv::scene::buildCollisionWorld(rv::scene::buildSceneGeometry(scene, *materials));
        controller.teleport(scene.camera.position - glm::vec3(0.0f, controller.settings().eyeHeight, 0.0f),
                            rv::core::degreesToRadians(scene.camera.yawDeg),
                            rv::core::degreesToRadians(scene.camera.pitchDeg));
    }

    /// Plays frames [0, frames) of `script` over idle devices.
    void play(const rv::input::InputScript& script, std::uint64_t frames, float dt = kFixedDt) {
        rv::input::InputSystem system;
        rv::input::InputScriptPlayer player(script);
        for (std::uint64_t frame = 0; frame < frames; ++frame) {
            rv::input::ActionState actions = system.resolve(rv::input::RawInputFrame{});
            player.apply(frame, actions, system.settings().pressThreshold);
            controller.update(actions, dt, camera, &world);
            REQUIRE_FALSE(world.overlaps(controller.feetPosition(), controller.settings().capsule));
        }
    }

    /// Holds MoveForward (and Sprint) for `frames` frames of `dt`, without a script.
    void walk(int frames, float dt, bool sprint = false) {
        rv::input::ActionState actions;
        actions.setAxis(Action::MoveForward, 1.0f);
        actions.setButton(Action::Sprint, sprint, false, false);
        for (int frame = 0; frame < frames; ++frame) {
            controller.update(actions, dt, camera, &world);
            REQUIRE_FALSE(world.overlaps(controller.feetPosition(), controller.settings().capsule));
        }
    }

    [[nodiscard]] std::string nearestTag() const {
        const auto nearest = world.nearestBlocking(controller.feetPosition(), controller.settings().capsule);
        return nearest ? world.boxes()[nearest->index].tag : std::string();
    }

    [[nodiscard]] float nearestDistance() const {
        const auto nearest = world.nearestBlocking(controller.feetPosition(), controller.settings().capsule);
        REQUIRE(nearest.has_value());
        return nearest->distance;
    }
};

} // namespace

TEST_CASE("integration/free_roam: the M0 room spawn is free", "[integration][free_roam]") {
    const TestRoomWalker walker;
    CHECK(rv::core::nearlyEqual(walker.controller.feetPosition(), glm::vec3(0.0f, 0.0f, 3.2f)));
    CHECK_FALSE(walker.world.overlaps(walker.controller.feetPosition(), walker.controller.settings().capsule));
    CHECK(walker.world.boxes().size() == 35);
}

TEST_CASE("integration/free_roam: walk_forward_2s.json walks 5 m north", "[integration][free_roam]") {
    TestRoomWalker walker;
    walker.play(loadScript("walk_forward_2s"), 120);
    const glm::vec3 feet = walker.controller.feetPosition();
    CHECK(feet.x == Approx(0.0f).margin(1e-5));
    CHECK(feet.y == 0.0f);
    CHECK(feet.z == Approx(3.2f - 120.0f * 2.5f * kFixedDt).margin(1e-4)); // -1.8 m: nothing in the way
    CHECK(walker.camera.position().y == Approx(1.65f));
}

TEST_CASE("integration/free_roam: sprint_forward_1s.json sprints 4.5 m", "[integration][free_roam]") {
    TestRoomWalker walker;
    walker.play(loadScript("sprint_forward_1s"), 60);
    CHECK(walker.controller.feetPosition().z == Approx(3.2f - 60.0f * 4.5f * kFixedDt).margin(1e-4));
}

TEST_CASE("integration/free_roam: walk_into_shelf.json stops one capsule radius before the shelf",
          "[integration][free_roam]") {
    TestRoomWalker walker;
    walker.play(loadScript("walk_into_shelf"), 336);
    const glm::vec3 feet = walker.controller.feetPosition();
    CHECK(feet.x == Approx(-1.5f).margin(1e-3));
    CHECK(feet.z == Approx(-4.625f + 0.3f).margin(1e-4)); // shelf front face + radius
    CHECK(walker.nearestTag() == "shelf_main");
    CHECK(walker.nearestDistance() >= 0.3f - rv::scene::CollisionWorld::kContactTolerance);
}

TEST_CASE("integration/free_roam: walk_into_north_wall.json stops one capsule radius before the wall",
          "[integration][free_roam]") {
    TestRoomWalker walker;
    walker.play(loadScript("walk_into_north_wall"), 300);
    CHECK(walker.controller.feetPosition().z == Approx(-5.0f + 0.3f).margin(1e-4));
    CHECK(walker.nearestTag() == "room/north");
    CHECK(walker.nearestDistance() >= 0.3f - rv::scene::CollisionWorld::kContactTolerance);
}

TEST_CASE("integration/free_roam: look_turn_pitch.json turns and clamps the pitch at 85 degrees",
          "[integration][free_roam]") {
    const rv::input::InputScript script = loadScript("look_turn_pitch");
    TestRoomWalker turned;
    turned.play(script, 10);
    CHECK(rv::core::radiansToDegrees(turned.controller.yaw()) == Approx(25.2101f).margin(1e-3));
    CHECK(rv::core::radiansToDegrees(turned.controller.pitch()) == Approx(-6.0f).margin(1e-4));

    TestRoomWalker up;
    up.play(script, 110);
    CHECK(rv::core::radiansToDegrees(up.controller.pitch()) == Approx(85.0f).margin(1e-4));
    TestRoomWalker down;
    down.play(script, 230);
    CHECK(rv::core::radiansToDegrees(down.controller.yaw()) == Approx(25.2101f).margin(1e-3));
    CHECK(rv::core::radiansToDegrees(down.controller.pitch()) == Approx(-85.0f).margin(1e-4));
    CHECK(rv::core::nearlyEqual(down.controller.feetPosition(), glm::vec3(0.0f, 0.0f, 3.2f)));
}

TEST_CASE("integration/free_roam: the doorway is passable, the walls are not", "[integration][free_roam]") {
    // Out through the doorway (south, 1.6 m wide) into the void beyond the room.
    TestRoomWalker outward;
    outward.controller.teleport(glm::vec3(0.0f, 0.0f, 3.2f), rv::core::kPi, 0.0f);
    outward.walk(120, kFixedDt);
    CHECK(outward.controller.feetPosition().z == Approx(3.2f + 5.0f).margin(1e-3));

    // Beside the doorway the south wall stops the walker; east and west walls too.
    TestRoomWalker beside;
    beside.controller.teleport(glm::vec3(2.0f, 0.0f, 3.2f), rv::core::kPi, 0.0f);
    beside.walk(120, kFixedDt);
    CHECK(beside.controller.feetPosition().z == Approx(5.0f - 0.3f).margin(1e-4));
    CHECK(beside.nearestTag() == "room/south_east");

    TestRoomWalker east;
    east.controller.teleport(glm::vec3(0.0f, 0.0f, 3.2f), 0.5f * rv::core::kPi, 0.0f);
    east.walk(120, kFixedDt);
    CHECK(east.controller.feetPosition().x == Approx(4.5f - 0.3f).margin(1e-4));
    CHECK(east.nearestTag() == "room/east");
}

TEST_CASE("integration/free_roam: sprinting with long frames never tunnels through walls or furniture",
          "[integration][free_roam]") {
    // 4.5 m/s at the clamped 0.1 s frame time: 45 cm per frame, more than the walls are thick.
    for (int direction = 0; direction < 16; ++direction) {
        const float yaw = static_cast<float>(direction) * rv::core::kTwoPi / 16.0f;
        INFO("yaw " << rv::core::radiansToDegrees(yaw) << " deg");
        TestRoomWalker walker;
        walker.controller.teleport(glm::vec3(0.0f, 0.0f, 1.0f), yaw, 0.0f);
        walker.walk(100, 0.1f, true);
        const glm::vec3 feet = walker.controller.feetPosition();
        const bool insideRoom = feet.x >= -4.5f + 0.3f - 1e-4f && feet.x <= 4.5f - 0.3f + 1e-4f &&
                                feet.z >= -5.0f + 0.3f - 1e-4f && feet.z <= 5.0f - 0.3f + 1e-4f;
        const bool leftThroughDoorway = feet.z > 5.0f && feet.x > -0.8f + 0.3f - 1e-4f && feet.x < 0.8f - 0.3f + 1e-4f;
        CHECK((insideRoom || leftThroughDoorway));
    }
}
