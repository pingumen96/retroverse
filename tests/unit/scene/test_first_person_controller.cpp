#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include <rv/core/aabb.hpp>
#include <rv/core/math.hpp>
#include <rv/input/action_state.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/first_person_controller.hpp>

using Catch::Approx;
using rv::core::degreesToRadians;
using rv::input::Action;
using rv::input::ActionState;
using rv::scene::Camera;
using rv::scene::FirstPersonController;
using rv::scene::FirstPersonSettings;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

ActionState moving(float forward, float right, bool sprint = false) {
    ActionState state;
    state.setAxis(Action::MoveForward, forward);
    state.setAxis(Action::MoveRight, right);
    state.setButton(Action::Sprint, sprint, sprint, false);
    return state;
}

ActionState looking(Action action, float value) {
    ActionState state;
    state.setAxis(action, value);
    return state;
}

} // namespace

TEST_CASE("scene/first_person: walks 2.5 m in one second", "[scene][first_person]") {
    FirstPersonController controller;
    Camera camera;
    controller.update(moving(1.0f, 0.0f), 1.0f, camera);
    CHECK(rv::core::nearlyEqual(controller.feetPosition(), glm::vec3(0.0f, 0.0f, -2.5f))); // yaw 0 faces -Z

    // The same distance at 60 Hz.
    FirstPersonController stepped;
    for (int frame = 0; frame < 60; ++frame) {
        stepped.update(moving(1.0f, 0.0f), 1.0f / 60.0f, camera);
    }
    CHECK(stepped.feetPosition().z == Approx(-2.5f).margin(1e-4));
    CHECK(stepped.feetPosition().x == Approx(0.0f).margin(1e-6));

    // Analog deflection below 1 scales the speed; backwards and strafing follow the axes.
    FirstPersonController analog;
    analog.update(moving(-0.5f, 0.0f), 1.0f, camera);
    CHECK(rv::core::nearlyEqual(analog.feetPosition(), glm::vec3(0.0f, 0.0f, 1.25f)));
    analog.update(moving(0.0f, 1.0f), 1.0f, camera);
    CHECK(rv::core::nearlyEqual(analog.feetPosition(), glm::vec3(2.5f, 0.0f, 1.25f)));
}

TEST_CASE("scene/first_person: sprint moves at 4.5 m/s while held", "[scene][first_person]") {
    FirstPersonController controller;
    Camera camera;
    controller.update(moving(1.0f, 0.0f, true), 1.0f, camera);
    CHECK(controller.feetPosition().z == Approx(-4.5f));
    controller.update(moving(1.0f, 0.0f, false), 1.0f, camera);
    CHECK(controller.feetPosition().z == Approx(-7.0f));
}

TEST_CASE("scene/first_person: diagonal input is not faster", "[scene][first_person]") {
    FirstPersonController controller;
    Camera camera;
    controller.update(moving(1.0f, 1.0f), 1.0f, camera);
    const glm::vec3 feet = controller.feetPosition();
    CHECK(glm::length(feet) == Approx(2.5f));
    CHECK(feet.x == Approx(2.5f / std::sqrt(2.0f)));
    CHECK(feet.z == Approx(-2.5f / std::sqrt(2.0f)));
    CHECK(feet.y == 0.0f);
}

TEST_CASE("scene/first_person: movement is relative to the yaw", "[scene][first_person]") {
    FirstPersonController controller;
    Camera camera;
    controller.teleport(glm::vec3(0.0f), rv::core::kHalfPi, 0.4f); // facing +X, looking up
    controller.update(moving(1.0f, 0.0f), 1.0f, camera);
    CHECK(rv::core::nearlyEqual(controller.feetPosition(), glm::vec3(2.5f, 0.0f, 0.0f), 1e-5f)); // pitch ignored
    controller.update(moving(0.0f, 1.0f), 1.0f, camera);
    CHECK(rv::core::nearlyEqual(controller.feetPosition(), glm::vec3(2.5f, 0.0f, 2.5f), 1e-5f)); // right = +Z
}

TEST_CASE("scene/first_person: mouse deltas turn by sensitivity, independent of dt", "[scene][first_person]") {
    Camera camera;
    for (const float dt : {0.5f, 0.001f}) {
        FirstPersonController controller;
        ActionState state = looking(Action::LookDeltaX, 100.0f);
        state.setAxis(Action::LookDeltaY, 50.0f);
        controller.update(state, dt, camera);
        CHECK(controller.yaw() == Approx(0.22f));   // 100 px * 0.0022 rad/px, positive = right
        CHECK(controller.pitch() == Approx(0.11f)); // positive = up
        CHECK(camera.yaw() == controller.yaw());
        CHECK(camera.pitch() == controller.pitch());
    }
}

TEST_CASE("scene/first_person: stick look turns at a rate times dt", "[scene][first_person]") {
    FirstPersonController controller;
    Camera camera;
    controller.update(looking(Action::LookX, 1.0f), 0.5f, camera); // 180 deg/s * 0.5 s
    CHECK(controller.yaw() == Approx(rv::core::kHalfPi));
    controller.update(looking(Action::LookY, -0.5f), 0.2f, camera); // half deflection downwards
    CHECK(controller.pitch() == Approx(-0.5f * rv::core::kPi * 0.2f));
    // Rates are clamped to [-1, 1].
    FirstPersonController clamped;
    clamped.update(looking(Action::LookX, 5.0f), 0.25f, camera);
    CHECK(clamped.yaw() == Approx(0.25f * rv::core::kPi));
}

TEST_CASE("scene/first_person: pitch is clamped to the limit", "[scene][first_person]") {
    FirstPersonController controller;
    Camera camera;
    controller.update(looking(Action::LookDeltaY, 10000.0f), 0.016f, camera);
    CHECK(controller.pitch() == Approx(degreesToRadians(85.0f)));
    CHECK(camera.pitch() == Approx(degreesToRadians(85.0f)));
    controller.update(looking(Action::LookDeltaY, -20000.0f), 0.016f, camera);
    CHECK(controller.pitch() == Approx(degreesToRadians(-85.0f)));

    // Lowering the limit re-clamps the current pitch.
    FirstPersonSettings settings;
    settings.pitchLimitDeg = 60.0f;
    controller.setSettings(settings);
    CHECK(controller.pitch() == Approx(degreesToRadians(-60.0f)));
}

TEST_CASE("scene/first_person: yaw wraps to (-pi, pi]", "[scene][first_person]") {
    FirstPersonController controller;
    Camera camera;
    controller.update(looking(Action::LookX, 1.0f), 1.5f, camera); // +270 degrees
    CHECK(controller.yaw() == Approx(-rv::core::kHalfPi));
    for (int frame = 0; frame < 100; ++frame) {
        controller.update(looking(Action::LookX, 1.0f), 0.1f, camera);
        CHECK(controller.yaw() > -rv::core::kPi);
        CHECK(controller.yaw() <= rv::core::kPi);
    }
    // 1.5 s + 10 s at pi rad/s = 11.5 pi = -0.5 pi after wrapping.
    CHECK(controller.yaw() == Approx(-rv::core::kHalfPi).margin(1e-3));
    CHECK(rv::core::nearlyEqual(camera.forward(), glm::vec3(-1.0f, 0.0f, 0.0f), 1e-3f));
}

TEST_CASE("scene/first_person: teleport sets feet and orientation; the camera sits at eye height",
          "[scene][first_person]") {
    FirstPersonController controller;
    Camera camera;
    controller.teleport(glm::vec3(3.0f, 0.5f, -2.0f), 1.0f, 0.3f);
    CHECK(controller.feetPosition() == glm::vec3(3.0f, 0.5f, -2.0f));
    CHECK(controller.yaw() == 1.0f);
    CHECK(controller.pitch() == 0.3f);

    controller.update(ActionState{}, 0.0f, camera);
    CHECK(rv::core::nearlyEqual(camera.position(), glm::vec3(3.0f, 2.15f, -2.0f))); // 1.65 m above the feet
    CHECK(camera.yaw() == 1.0f);
    CHECK(camera.pitch() == 0.3f);

    controller.teleport(glm::vec3(0.0f), 4.0f, 2.0f); // yaw wrapped, pitch clamped
    CHECK(controller.yaw() == Approx(4.0f - rv::core::kTwoPi));
    CHECK(controller.pitch() == Approx(degreesToRadians(85.0f)));

    controller.teleport(glm::vec3(kNaN, 1.0f, kNaN), kNaN, kNaN); // non-finite values are ignored
    CHECK(controller.feetPosition() == glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(controller.yaw() == Approx(4.0f - rv::core::kTwoPi));

    FirstPersonSettings settings;
    settings.eyeHeight = 1.6f;
    controller.setSettings(settings);
    controller.update(ActionState{}, 0.0f, camera);
    CHECK(camera.position().y == Approx(2.6f));
}

TEST_CASE("scene/first_person: settings are sanitized", "[scene][first_person]") {
    FirstPersonController controller;
    FirstPersonSettings settings;
    settings.walkSpeed = kNaN;
    settings.sprintSpeed = -3.0f;
    settings.eyeHeight = std::numeric_limits<float>::infinity();
    settings.mouseRadiansPerPixel = -0.001f; // inverted look is allowed
    settings.pitchLimitDeg = 120.0f;
    settings.capsule.radius = kNaN;
    settings.capsule.height = 1.5f;
    controller.setSettings(settings);
    CHECK(controller.settings().walkSpeed == 2.5f);
    CHECK(controller.settings().sprintSpeed == 0.0f);
    CHECK(controller.settings().eyeHeight == 1.65f);
    CHECK(controller.settings().mouseRadiansPerPixel == -0.001f);
    CHECK(controller.settings().pitchLimitDeg == 89.0f);
    CHECK(controller.settings().capsule.radius == 0.30f);
    CHECK(controller.settings().capsule.height == 1.5f);
}

TEST_CASE("scene/first_person: invalid frame input never moves or corrupts the walker", "[scene][first_person]") {
    FirstPersonController controller;
    Camera camera;
    controller.update(moving(1.0f, 0.0f), kNaN, camera);
    controller.update(moving(1.0f, 0.0f), -1.0f, camera);
    controller.update(moving(kNaN, kNaN), 1.0f, camera);
    ActionState badLook;
    badLook.setAxis(Action::LookDeltaX, kNaN);
    badLook.setAxis(Action::LookY, kNaN);
    controller.update(badLook, 1.0f, camera);
    CHECK(controller.feetPosition() == glm::vec3(0.0f));
    CHECK(controller.yaw() == 0.0f);
    CHECK(controller.pitch() == 0.0f);

    // An absurd dt overflows the displacement: the walker stays put instead of jumping to infinity.
    controller.update(moving(1.0f, 0.0f), std::numeric_limits<float>::max(), camera);
    CHECK(rv::core::isFinite(controller.feetPosition()));
    CHECK(rv::core::isFinite(camera.view()));
}

TEST_CASE("scene/first_person: collision world stops the walker at walls", "[scene][first_person][collision]") {
    rv::scene::CollisionWorld world;
    world.addBox(rv::core::Aabb::fromMinMax({-5.0f, 0.0f, -2.2f}, {5.0f, 3.0f, -2.0f}), "wall_north");
    FirstPersonController controller;
    Camera camera;
    for (int frame = 0; frame < 120; ++frame) {
        controller.update(moving(1.0f, 0.0f, true), 1.0f / 60.0f, camera, &world);
    }
    CHECK(controller.feetPosition().z == Approx(-2.0f + 0.30f).margin(1e-4)); // capsule radius from the wall
    CHECK(camera.position().z == Approx(controller.feetPosition().z));
    CHECK_FALSE(world.overlaps(controller.feetPosition(), controller.settings().capsule));
}
