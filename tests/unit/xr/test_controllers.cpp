// The controllers in VR (I10 step 4, ADR-0017 §5): their gamepad merged into a frame's device input, the pointing hand
// and its ray, the shipped bindings of every interaction profile.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <set>
#include <string>

#include <glm/geometric.hpp>
#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/json.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/input/raw_input.hpp>
#include <rv/xr/controllers.hpp>
#include <rv/xr/locomotion.hpp>
#include <rv/xr/xr_bindings.hpp>

#include "support/temp_dir.hpp"

using Catch::Approx;
using rv::core::degreesToRadians;
using rv::input::GamepadAxis;
using rv::input::GamepadButton;
using rv::xr::Hand;
using rv::xr::HandState;
using rv::xr::Pose;

namespace {

const std::filesystem::path& assetRoot() {
    static const std::filesystem::path root = rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
    return root;
}

Pose pointing(const glm::vec3& position, float yawDeg, float pitchDeg = 0.0f) {
    return Pose{rv::xr::orientationOf(rv::xr::Angles{degreesToRadians(yawDeg), degreesToRadians(pitchDeg), 0.0f}),
                position};
}

HandState held(const Pose& pose, float select = 0.0f) {
    HandState hand;
    hand.tracked = true;
    hand.grip = pose;
    hand.aim = pose;
    hand.select = select;
    return hand;
}

} // namespace

TEST_CASE("xr/controllers: the controllers' gamepad joins the frame's, pressed and released once",
          "[xr][controllers]") {
    rv::xr::VirtualPad pad;
    rv::xr::PadState state;
    state.buttons.set(rv::input::toIndex(GamepadButton::South));
    state.axes[rv::input::toIndex(GamepadAxis::LeftY)] = 0.8f;
    pad.update(state);

    rv::input::RawInputFrame raw;
    raw.gamepadAxes[rv::input::toIndex(GamepadAxis::LeftY)] = 0.0f;
    pad.mergeInto(raw);
    CHECK(raw.gamepadConnected);
    CHECK(raw.gamepadButtonsDown.test(rv::input::toIndex(GamepadButton::South)));
    CHECK(raw.gamepadButtonsPressed.test(rv::input::toIndex(GamepadButton::South)));
    CHECK(raw.gamepadAxes[rv::input::toIndex(GamepadAxis::LeftY)] == 0.8f);
    CHECK(raw.lastActiveDevice == rv::input::DeviceKind::Gamepad);

    // Held on: no second press; the physical pad's stick, farther from rest, wins.
    pad.update(state);
    rv::input::RawInputFrame next;
    next.gamepadConnected = true;
    next.gamepadAxes[rv::input::toIndex(GamepadAxis::LeftY)] = -0.9f;
    pad.mergeInto(next);
    CHECK(next.gamepadButtonsDown.test(rv::input::toIndex(GamepadButton::South)));
    CHECK_FALSE(next.gamepadButtonsPressed.test(rv::input::toIndex(GamepadButton::South)));
    CHECK(next.gamepadAxes[rv::input::toIndex(GamepadAxis::LeftY)] == -0.9f);

    // Let go: released once, then nothing.
    pad.release();
    rv::input::RawInputFrame released;
    pad.mergeInto(released);
    CHECK(released.gamepadButtonsReleased.test(rv::input::toIndex(GamepadButton::South)));
    CHECK_FALSE(released.gamepadButtonsDown.test(rv::input::toIndex(GamepadButton::South)));
    CHECK(released.lastActiveDevice == rv::input::DeviceKind::KeyboardMouse);
    pad.release();
    rv::input::RawInputFrame idle;
    pad.mergeInto(idle);
    CHECK(idle.gamepadButtonsReleased.none());
}

TEST_CASE("xr/controllers: the hand whose trigger went down last points, its ray in the world", "[xr][controllers]") {
    rv::xr::Pointer pointer;
    // The space turned 90 degrees right and moved to (2, 0, 1): a hand pointing along the space's -Z points along +X.
    const rv::xr::TrackingAnchor anchor{glm::vec3(2.0f, 0.0f, 1.0f), degreesToRadians(90.0f), 0.0f};
    std::array<HandState, rv::xr::kHandCount> hands{};
    hands[0] = held(pointing(glm::vec3(-0.2f, 1.1f, -0.3f), 0.0f));
    hands[1] = held(pointing(glm::vec3(0.2f, 1.1f, -0.3f), 0.0f));
    pointer.update(hands, anchor);
    // The right hand points first.
    REQUIRE(pointer.hand() == Hand::Right);
    REQUIRE(pointer.ray());
    CHECK(pointer.ray()->direction.x == Approx(1.0f).margin(1e-5));
    CHECK(pointer.ray()->origin.x == Approx(2.0f + 0.3f).margin(1e-5));
    CHECK(pointer.ray()->origin.z == Approx(1.0f + 0.2f).margin(1e-5));
    CHECK_FALSE(pointer.selectPressed());

    // The left trigger past the press threshold: the left hand points, pressed once.
    hands[0].select = 0.7f;
    pointer.update(hands, anchor);
    CHECK(pointer.hand() == Hand::Left);
    CHECK(pointer.selectPressed());
    CHECK(pointer.selectHeld());
    // Within the hysteresis it stays held, below it is released.
    hands[0].select = 0.5f;
    pointer.update(hands, anchor);
    CHECK_FALSE(pointer.selectPressed());
    CHECK(pointer.selectHeld());
    hands[0].select = 0.2f;
    pointer.update(hands, anchor);
    CHECK(pointer.selectReleased());
    CHECK_FALSE(pointer.selectHeld());

    // The left controller lost: the right one points again.
    hands[0].tracked = false;
    pointer.update(hands, anchor);
    CHECK(pointer.hand() == Hand::Right);
    CHECK_FALSE(pointer.grip(Hand::Left));
    REQUIRE(pointer.grip(Hand::Right));
    // Nothing tracked: nothing points.
    hands[1].tracked = false;
    pointer.update(hands, anchor);
    CHECK_FALSE(pointer.hand());
    CHECK_FALSE(pointer.ray());
    pointer.clear();
    CHECK_FALSE(pointer.ray());
}

TEST_CASE("xr/controllers: the laser stops at the first collider or the floor", "[xr][controllers]") {
    rv::scene::CollisionWorld world;
    world.addBox(rv::core::Aabb{glm::vec3(-5.0f, -0.2f, -5.0f), glm::vec3(5.0f, 0.0f, 5.0f)}, "floor");
    world.addBox(rv::core::Aabb{glm::vec3(-5.0f, 0.0f, -3.0f), glm::vec3(5.0f, 3.0f, -2.5f)}, "wall");
    const rv::core::Ray ahead{glm::vec3(0.0f, 1.2f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    CHECK(rv::xr::castRay(ahead, 4.0f, 0.0f, &world) == Approx(2.5f).margin(1e-4));
    const rv::core::Ray down{glm::vec3(0.0f, 1.2f, 0.0f), glm::normalize(glm::vec3(0.0f, -1.0f, -1.0f))};
    CHECK(rv::xr::castRay(down, 4.0f, 0.0f, &world) == Approx(1.2f * std::sqrt(2.0f)).margin(1e-4));
    const rv::core::Ray back{glm::vec3(0.0f, 1.2f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)};
    CHECK(rv::xr::castRay(back, 4.0f, 0.0f, &world) == 4.0f);
    CHECK(rv::xr::castRay(ahead, 4.0f, 0.0f, nullptr) == 4.0f);
}

TEST_CASE("xr/controllers: the right controller aims the arc, the left one may lead the walk", "[xr][controllers]") {
    rv::xr::Locomotion locomotion;
    locomotion.place(glm::vec3(0.0f), 0.0f);
    const Pose head{glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.65f, 0.0f)};
    locomotion.update(head, 0.0f, rv::xr::LocomotionInput{}, 1.0f / 72.0f, nullptr);
    rv::xr::LocomotionInput aim;
    aim.aim = 1.0f;
    // The right hand at the hip, pointing 90 degrees right and 20 degrees up: the arc starts there and goes along +X.
    aim.arcFrom = pointing(glm::vec3(0.25f, 1.0f, -0.2f), 90.0f, 20.0f);
    locomotion.update(head, 0.0f, aim, 1.0f / 72.0f, nullptr);
    REQUIRE(locomotion.arc());
    CHECK(locomotion.arc()->points.front().x == Approx(0.25f).margin(1e-5));
    CHECK(locomotion.arc()->points.front().y == Approx(1.0f).margin(1e-5));
    REQUIRE(locomotion.arc()->landing);
    CHECK(locomotion.arc()->landing->x > 1.0f);
    CHECK(std::abs(locomotion.arc()->landing->z + 0.2f) < 1e-3f);

    rv::xr::LocomotionSettings settings;
    settings.comfort.moveWithHand = true;
    rv::xr::Locomotion hand;
    hand.setSettings(settings);
    hand.place(glm::vec3(0.0f), 0.0f);
    hand.update(head, 0.0f, rv::xr::LocomotionInput{}, 1.0f / 72.0f, nullptr);
    rv::xr::LocomotionInput walk;
    walk.move = glm::vec2(0.0f, 1.0f);
    walk.moveAlong = pointing(glm::vec3(-0.2f, 1.0f, -0.2f), -90.0f);
    for (int frame = 0; frame < 72; ++frame) {
        hand.update(head, 0.0f, walk, 1.0f / 72.0f, nullptr);
    }
    // A second along the left hand's heading (west) at 2.5 m/s, the head still facing north.
    CHECK(hand.feet().x == Approx(-2.5f).margin(1e-3));
    CHECK(hand.feet().z == Approx(0.0f).margin(1e-3));
    CHECK(hand.heading() == Approx(0.0f).margin(1e-5));
}

TEST_CASE("xr/bindings: the shipped profiles parse, each driving the pad, the select, the poses and the vibration",
          "[xr][xr_bindings]") {
    auto document = rv::core::readJsonFile(assetRoot() / "config" / "xr_bindings.json");
    REQUIRE(document.has_value());
    auto parsed = rv::xr::parseXrBindings(*document, "xr_bindings.json");
    REQUIRE(parsed.has_value());
    std::set<std::string> profiles;
    for (const rv::xr::InteractionProfile& profile : parsed->profiles) {
        INFO(profile.path);
        profiles.insert(profile.path);
        std::set<rv::xr::ControllerAction> bound;
        for (const rv::xr::ProfileBinding& binding : profile.bindings) {
            bound.insert(binding.action);
        }
        // Every profile selects, points, holds and vibrates with both hands, and has a way back and a menu.
        for (const rv::xr::ControllerAction action :
             {rv::xr::ControllerAction::Select, rv::xr::ControllerAction::Grip, rv::xr::ControllerAction::Aim,
              rv::xr::ControllerAction::Haptic, rv::xr::ControllerAction::East, rv::xr::ControllerAction::Start}) {
            CHECK(bound.count(action) == 1);
        }
        const auto perHand = [&](rv::xr::ControllerAction action) {
            int hands = 0;
            for (const rv::xr::ProfileBinding& binding : profile.bindings) {
                hands += binding.action == action ? 1 : 0;
            }
            return hands;
        };
        CHECK(perHand(rv::xr::ControllerAction::Aim) == 2);
        CHECK(perHand(rv::xr::ControllerAction::Grip) == 2);
        // Every profile but the simple controller walks and turns.
        if (profile.path != "/interaction_profiles/khr/simple_controller") {
            CHECK(bound.count(rv::xr::ControllerAction::LeftStick) == 1);
            CHECK(bound.count(rv::xr::ControllerAction::RightStick) == 1);
            CHECK(bound.count(rv::xr::ControllerAction::Back) == 1);
            CHECK(bound.count(rv::xr::ControllerAction::South) == 1);
            CHECK(bound.count(rv::xr::ControllerAction::North) == 1);
        }
    }
    CHECK(profiles == std::set<std::string>{"/interaction_profiles/oculus/touch_controller",
                                            "/interaction_profiles/valve/index_controller",
                                            "/interaction_profiles/htc/vive_controller",
                                            "/interaction_profiles/microsoft/motion_controller",
                                            "/interaction_profiles/bytedance/pico4_controller",
                                            "/interaction_profiles/khr/simple_controller"});
    const auto pico = std::find_if(parsed->profiles.begin(), parsed->profiles.end(), [](const auto& profile) {
        return profile.path == "/interaction_profiles/bytedance/pico4_controller";
    });
    REQUIRE(pico != parsed->profiles.end());
    CHECK(pico->extension == std::optional<std::string>("XR_BD_controller_interaction"));
}

TEST_CASE("xr/bindings: a binding that does not fit its action is refused; the user's profile replaces one",
          "[xr][xr_bindings]") {
    const auto refused = [](const char* text) {
        return !rv::xr::parseXrBindings(nlohmann::json::parse(text), "test").has_value();
    };
    const char* stickToButton = R"({"schemaVersion": 1, "profiles": [{"profile": "/interaction_profiles/x/y",
        "bindings": {"left_stick": ["/user/hand/left/input/a/click"]}}]})";
    const char* buttonToPose = R"({"schemaVersion": 1, "profiles": [{"profile": "/interaction_profiles/x/y",
        "bindings": {"south": ["/user/hand/right/input/grip/pose"]}}]})";
    const char* notAHand = R"({"schemaVersion": 1, "profiles": [{"profile": "/interaction_profiles/x/y",
        "bindings": {"south": ["/user/head/input/a/click"]}}]})";
    const char* unknownAction = R"({"schemaVersion": 1, "profiles": [{"profile": "/interaction_profiles/x/y",
        "bindings": {"jump": ["/user/hand/right/input/a/click"]}}]})";
    const char* twice = R"({"schemaVersion": 1, "profiles": [
        {"profile": "/interaction_profiles/x/y", "bindings": {"south": ["/user/hand/right/input/a/click"]}},
        {"profile": "/interaction_profiles/x/y", "bindings": {"south": ["/user/hand/right/input/a/click"]}}]})";
    const char* badProfile = R"({"schemaVersion": 1, "profiles": [{"profile": "touch",
        "bindings": {"south": ["/user/hand/right/input/a/click"]}}]})";
    CHECK(refused(stickToButton));
    CHECK(refused(buttonToPose));
    CHECK(refused(notAHand));
    CHECK(refused(unknownAction));
    CHECK(refused(twice));
    CHECK(refused(badProfile));
    CHECK_FALSE(refused(R"({"schemaVersion": 1, "profiles": [{"profile": "/interaction_profiles/x/y",
        "bindings": {"select": ["/user/hand/right/input/trigger/value"]}}]})"));

    const rv::test::TempDir dir("rv-xr-bindings");
    REQUIRE(rv::core::writeTextFileAtomic(dir.path() / "xr_bindings.json", R"({"schemaVersion": 1, "profiles": [
        {"profile": "/interaction_profiles/oculus/touch_controller",
         "bindings": {"select": ["/user/hand/right/input/trigger/value"], "south": ["/user/hand/right/input/b/click"]}},
        {"profile": "/interaction_profiles/acme/wand",
         "bindings": {"select": ["/user/hand/right/input/select/click"]}}]})")
                .has_value());
    auto loaded = rv::xr::loadXrBindings(assetRoot(), dir.path());
    REQUIRE(loaded.has_value());
    CHECK(loaded->issues.empty());
    CHECK(loaded->bindings.profiles.size() == 7);
    CHECK(loaded->bindings.profiles.front().path == "/interaction_profiles/oculus/touch_controller");
    CHECK(loaded->bindings.profiles.front().bindings.size() == 2);
    CHECK(loaded->bindings.profiles.back().path == "/interaction_profiles/acme/wand");
}
