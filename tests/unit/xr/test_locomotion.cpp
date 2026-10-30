// The movement in VR (I10 step 3, ADR-0017 §5): the head placed above the walker, turns about the head, the stick along
// the head's heading, the capsule following the head (a lean faded and pushed back), the seated posture, the runtime's
// recentering, the teleport's arc and its blink, the comfort vignette.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include <glm/geometric.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/math.hpp>
#include <rv/scene/collision.hpp>
#include <rv/xr/locomotion.hpp>
#include <rv/xr/pose.hpp>

using Catch::Approx;
using rv::core::degreesToRadians;
using rv::core::radiansToDegrees;
using rv::xr::Angles;
using rv::xr::Locomotion;
using rv::xr::LocomotionInput;
using rv::xr::LocomotionSettings;
using rv::xr::Pose;

namespace {

constexpr float kDt = 1.0f / 72.0f;

Pose tracked(const glm::vec3& position, float yawDeg = 0.0f, float pitchDeg = 0.0f) {
    return Pose{rv::xr::orientationOf(Angles{degreesToRadians(yawDeg), degreesToRadians(pitchDeg), 0.0f}), position};
}

void checkNear(const glm::vec3& actual, const glm::vec3& expected, float margin = 1e-4f) {
    CHECK(actual.x == Approx(expected.x).margin(margin));
    CHECK(actual.y == Approx(expected.y).margin(margin));
    CHECK(actual.z == Approx(expected.z).margin(margin));
}

/// A room 10 m square with its walls' inner faces at x, z = +-5, 3 m high, and a floor slab under it.
rv::scene::CollisionWorld room() {
    rv::scene::CollisionWorld world;
    world.addBox(rv::core::Aabb{glm::vec3(-6.0f, -0.2f, -6.0f), glm::vec3(6.0f, 0.0f, 6.0f)}, "floor");
    world.addBox(rv::core::Aabb{glm::vec3(-6.0f, 0.0f, -6.0f), glm::vec3(6.0f, 3.0f, -5.0f)}, "wall_north");
    world.addBox(rv::core::Aabb{glm::vec3(-6.0f, 0.0f, 5.0f), glm::vec3(6.0f, 3.0f, 6.0f)}, "wall_south");
    world.addBox(rv::core::Aabb{glm::vec3(-6.0f, 0.0f, -6.0f), glm::vec3(-5.0f, 3.0f, 6.0f)}, "wall_west");
    world.addBox(rv::core::Aabb{glm::vec3(5.0f, 0.0f, -6.0f), glm::vec3(6.0f, 3.0f, 6.0f)}, "wall_east");
    return world;
}

} // namespace

TEST_CASE("xr/locomotion: a placement puts the head above the walker's feet, facing its yaw", "[xr][locomotion]") {
    Locomotion locomotion;
    locomotion.place(glm::vec3(1.0f, 0.0f, 2.0f), degreesToRadians(90.0f));
    // The user stands 0.4 m to the side of the space's origin, turned 30 degrees left in it.
    locomotion.update(tracked(glm::vec3(0.4f, 1.7f, 0.1f), -30.0f), 0.0f, LocomotionInput{}, kDt, nullptr);
    checkNear(locomotion.head().position, glm::vec3(1.0f, 1.7f, 2.0f));
    checkNear(locomotion.feet(), glm::vec3(1.0f, 0.0f, 2.0f));
    CHECK(radiansToDegrees(locomotion.heading()) == Approx(90.0f).margin(1e-3));
    // Looking along the heading: +X.
    checkNear(locomotion.head().orientation * glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
    // The eyes' anchor composes the same way.
    checkNear(rv::xr::toWorld(locomotion.anchor(), tracked(glm::vec3(0.4f, 1.7f, 0.1f), -30.0f)).position,
              locomotion.head().position);

    // The head moving in the space moves in the world, the capsule under it.
    locomotion.update(tracked(glm::vec3(0.4f, 1.7f, -0.4f), -30.0f), 0.0f, LocomotionInput{}, kDt, nullptr);
    const float moved = glm::length(locomotion.head().position - glm::vec3(1.0f, 1.7f, 2.0f));
    CHECK(moved == Approx(0.5f).margin(1e-4));
    CHECK(locomotion.feet().x == Approx(locomotion.head().position.x).margin(1e-5));
    CHECK(locomotion.feet().z == Approx(locomotion.head().position.z).margin(1e-5));
}

TEST_CASE("xr/locomotion: snap turns once per push, about the head; smooth turns at its rate", "[xr][locomotion]") {
    Locomotion locomotion;
    locomotion.place(glm::vec3(0.0f), 0.0f);
    const Pose head = tracked(glm::vec3(0.3f, 1.6f, 0.0f));
    locomotion.update(head, 0.0f, LocomotionInput{}, kDt, nullptr);
    const glm::vec3 before = locomotion.head().position;

    LocomotionInput push;
    push.turn = 0.9f;
    for (int frame = 0; frame < 10; ++frame) {
        locomotion.update(head, 0.0f, push, kDt, nullptr);
    }
    CHECK(radiansToDegrees(locomotion.heading()) == Approx(30.0f).margin(1e-3));
    CHECK(locomotion.counters().snapTurns == 1);
    checkNear(locomotion.head().position, before);
    // Let go, pushed again: another turn; to the left, back.
    locomotion.update(head, 0.0f, LocomotionInput{}, kDt, nullptr);
    locomotion.update(head, 0.0f, push, kDt, nullptr);
    CHECK(radiansToDegrees(locomotion.heading()) == Approx(60.0f).margin(1e-3));
    locomotion.update(head, 0.0f, LocomotionInput{}, kDt, nullptr);
    push.turn = -1.0f;
    locomotion.update(head, 0.0f, push, kDt, nullptr);
    CHECK(radiansToDegrees(locomotion.heading()) == Approx(30.0f).margin(1e-3));
    checkNear(locomotion.head().position, before);
    CHECK(locomotion.counters().snapTurns == 3);

    LocomotionSettings settings;
    settings.comfort.turn = rv::xr::TurnMode::Smooth;
    settings.comfort.smoothDegreesPerSecond = 72.0f;
    Locomotion smooth;
    smooth.setSettings(settings);
    smooth.place(glm::vec3(0.0f), 0.0f);
    LocomotionInput half;
    half.turn = 0.5f;
    for (int frame = 0; frame < 72; ++frame) {
        smooth.update(head, 0.0f, half, kDt, nullptr);
    }
    // 72 frames of 1/72 s at half deflection: 36 degrees.
    CHECK(radiansToDegrees(smooth.heading()) == Approx(36.0f).margin(1e-2));
    CHECK(smooth.counters().snapTurns == 0);
}

TEST_CASE("xr/locomotion: the stick walks along the head's heading, through the walls", "[xr][locomotion]") {
    const rv::scene::CollisionWorld world = room();
    Locomotion locomotion;
    locomotion.place(glm::vec3(0.0f), 0.0f);
    locomotion.update(tracked(glm::vec3(0.0f, 1.65f, 0.0f)), 0.0f, LocomotionInput{}, kDt, &world);
    // The head then turned 90 degrees right in the space: forward is +X in the world.
    const Pose head = tracked(glm::vec3(0.0f, 1.65f, 0.0f), 90.0f);
    LocomotionInput forward;
    forward.move = glm::vec2(0.0f, 1.0f);
    for (int frame = 0; frame < 72; ++frame) {
        locomotion.update(head, 0.0f, forward, kDt, &world);
    }
    // A second at 2.5 m/s.
    checkNear(locomotion.feet(), glm::vec3(2.5f, 0.0f, 0.0f), 1e-3f);
    checkNear(locomotion.head().position, glm::vec3(2.5f, 1.65f, 0.0f), 1e-3f);
    // Sprinting into the east wall: the capsule stops its radius from the face, the head with it.
    forward.sprint = true;
    for (int frame = 0; frame < 144; ++frame) {
        locomotion.update(head, 0.0f, forward, kDt, &world);
    }
    CHECK(locomotion.feet().x == Approx(5.0f - 0.3f).margin(1e-3));
    CHECK(locomotion.head().position.x == Approx(5.0f - 0.3f).margin(1e-3));
    CHECK(locomotion.fade() == 0.0f);
}

TEST_CASE("xr/locomotion: a head leaning into a wall fades, then is pushed back", "[xr][locomotion]") {
    const rv::scene::CollisionWorld world = room();
    Locomotion locomotion;
    locomotion.place(glm::vec3(0.0f, 0.0f, -4.5f), 0.0f);
    locomotion.update(tracked(glm::vec3(0.0f, 1.65f, 0.0f)), 0.0f, LocomotionInput{}, kDt, &world);
    // The capsule stands 0.2 m from the north wall's face; the user steps 0.1 m, 0.35 m, 0.8 m towards it.
    locomotion.update(tracked(glm::vec3(0.0f, 1.65f, -0.1f)), 0.0f, LocomotionInput{}, kDt, &world);
    CHECK(locomotion.feet().z == Approx(-4.6f).margin(1e-4));
    CHECK(locomotion.fade() == 0.0f);
    CHECK(locomotion.head().position.z == Approx(-4.6f).margin(1e-4));

    locomotion.update(tracked(glm::vec3(0.0f, 1.65f, -0.35f)), 0.0f, LocomotionInput{}, kDt, &world);
    // 0.15 m beyond the capsule: fading, not black.
    CHECK(locomotion.head().position.z == Approx(-4.85f).margin(1e-4));
    CHECK(locomotion.fade() > 0.0f);
    CHECK(locomotion.fade() < 1.0f);

    locomotion.update(tracked(glm::vec3(0.0f, 1.65f, -0.8f)), 0.0f, LocomotionInput{}, kDt, &world);
    // Pushed back to 0.3 m beyond the capsule: the head stays out of the wall, the view black.
    CHECK(locomotion.head().position.z == Approx(-4.7f - Locomotion::kMaxHeadOffset).margin(1e-4));
    CHECK(locomotion.fade() == Approx(1.0f));
    CHECK(locomotion.counters().pushedFrames == 1);
    CHECK(locomotion.counters().largestHeadOffset == Approx(0.6f).margin(1e-4));

    // Stepping back where the user stood: the room was pushed 0.3 m, the head comes back in view.
    locomotion.update(tracked(glm::vec3(0.0f, 1.65f, 0.0f)), 0.0f, LocomotionInput{}, kDt, &world);
    CHECK(locomotion.head().position.z == Approx(-4.7f + 0.5f).margin(1e-4));
    CHECK(locomotion.fade() == 0.0f);
}

TEST_CASE("xr/locomotion: seated, the head is made up to the eye height; recentering keeps it in place",
          "[xr][locomotion]") {
    LocomotionSettings settings;
    settings.posture = rv::xr::Posture::Seated;
    settings.eyeHeight = 1.65f;
    Locomotion locomotion;
    locomotion.setSettings(settings);
    locomotion.place(glm::vec3(2.0f, 0.5f, 0.0f), 0.0f);
    locomotion.update(tracked(glm::vec3(0.0f, 1.15f, 0.0f)), 0.0f, LocomotionInput{}, kDt, nullptr);
    CHECK(locomotion.head().position.y == Approx(0.5f + 1.65f).margin(1e-4));
    // Leaning down 0.2 m keeps the offset.
    locomotion.update(tracked(glm::vec3(0.0f, 0.95f, 0.0f)), 0.0f, LocomotionInput{}, kDt, nullptr);
    CHECK(locomotion.head().position.y == Approx(0.5f + 1.45f).margin(1e-4));

    // The runtime recenters: the space's origin moves under the head, turned 50 degrees; the head stays where it was in
    // the world, its height measured again.
    const Pose before = locomotion.head();
    const float heading = locomotion.heading();
    locomotion.recenter();
    locomotion.update(tracked(glm::vec3(0.0f, 0.95f, 0.0f), 50.0f), 0.0f, LocomotionInput{}, kDt, nullptr);
    CHECK(locomotion.head().position.x == Approx(before.position.x).margin(1e-4));
    CHECK(locomotion.head().position.z == Approx(before.position.z).margin(1e-4));
    CHECK(locomotion.head().position.y == Approx(0.5f + 1.65f).margin(1e-4));
    CHECK(locomotion.heading() == Approx(heading).margin(1e-4));

    // Standing: the tracked height as it is, through a LOCAL space's floor offset too.
    Locomotion standing;
    standing.place(glm::vec3(0.0f), 0.0f);
    standing.update(tracked(glm::vec3(0.0f, 0.05f, 0.0f)), 1.6f, LocomotionInput{}, kDt, nullptr);
    CHECK(standing.head().position.y == Approx(1.65f).margin(1e-4));
}

TEST_CASE("xr/locomotion: the teleport's arc lands on the floor, never through a wall or into furniture",
          "[xr][locomotion]") {
    rv::scene::CollisionWorld world = room();
    const rv::scene::Capsule capsule;
    // Horizontally from 1.3 m: it falls for sqrt(2 * 1.3 / 9.81) s, about 0.515 s, so lands about 4.12 m away.
    const rv::xr::TeleportArc flat =
        rv::xr::computeTeleportArc(glm::vec3(0.0f, 1.3f, 2.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.0f, &world, capsule);
    REQUIRE(flat.landing);
    CHECK(flat.landing->y == 0.0f);
    CHECK(flat.landing->z == Approx(2.0f - 8.0f * std::sqrt(2.0f * 1.3f / 9.81f)).margin(0.01));
    CHECK(flat.points.front() == glm::vec3(0.0f, 1.3f, 2.0f));
    CHECK(flat.points.back() == *flat.landing);

    // Towards the north wall from 1 m in front of it: it ends on the wall, no landing.
    const rv::xr::TeleportArc wall =
        rv::xr::computeTeleportArc(glm::vec3(0.0f, 1.3f, -4.0f), glm::vec3(0.0f, 0.2f, -1.0f), 0.0f, &world, capsule);
    CHECK_FALSE(wall.landing);
    CHECK(wall.points.back().z == Approx(-5.0f).margin(1e-3));

    // A landing beside a chest leaves no room for the capsule; a table on the way stops the arc against its side.
    rv::scene::CollisionWorld chest = room();
    chest.addBox(rv::core::Aabb{glm::vec3(0.2f, 0.0f, -2.6f), glm::vec3(1.0f, 0.9f, -1.6f)}, "chest");
    const rv::xr::TeleportArc beside =
        rv::xr::computeTeleportArc(glm::vec3(0.0f, 1.3f, 2.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.0f, &chest, capsule);
    CHECK_FALSE(beside.landing);
    CHECK(beside.points.back().y == 0.0f);
    world.addBox(rv::core::Aabb{glm::vec3(-1.0f, 0.0f, -2.6f), glm::vec3(1.0f, 0.75f, -1.6f)}, "table");
    const rv::xr::TeleportArc table =
        rv::xr::computeTeleportArc(glm::vec3(0.0f, 1.3f, 2.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.0f, &world, capsule);
    CHECK_FALSE(table.landing);
    CHECK(table.points.back().z == Approx(-1.6f).margin(1e-3));

    // A floor slab under the feet, or a rug, never stops it; no collision world lands anywhere.
    const rv::xr::TeleportArc free =
        rv::xr::computeTeleportArc(glm::vec3(0.0f, 1.3f, 2.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.0f, nullptr, capsule);
    CHECK(free.landing);
    // A degenerate direction draws nothing.
    CHECK(rv::xr::computeTeleportArc(glm::vec3(0.0f), glm::vec3(0.0f), 0.0f, &world, capsule).points.empty());
}

TEST_CASE("xr/locomotion: aiming up shows the arc, letting go jumps there with a blink", "[xr][locomotion]") {
    const rv::scene::CollisionWorld world = room();
    Locomotion locomotion;
    locomotion.place(glm::vec3(0.0f, 0.0f, 3.0f), 0.0f);
    // Looking 20 degrees down along -Z, the head 1.65 m up.
    const Pose head = tracked(glm::vec3(0.0f, 1.65f, 0.0f), 0.0f, -20.0f);
    locomotion.update(head, 0.0f, LocomotionInput{}, kDt, &world);
    CHECK_FALSE(locomotion.arc());

    LocomotionInput aim;
    aim.aim = 1.0f;
    locomotion.update(head, 0.0f, aim, kDt, &world);
    REQUIRE(locomotion.arc());
    REQUIRE(locomotion.arc()->landing);
    const glm::vec3 landing = *locomotion.arc()->landing;
    CHECK(landing.z < 3.0f - 1.0f);
    CHECK(locomotion.arc()->points.front().y == Approx(1.65f - Locomotion::kArcBelowHead).margin(1e-4));
    // Still held: still aiming, nothing moved.
    locomotion.update(head, 0.0f, aim, kDt, &world);
    CHECK(locomotion.feet().z == Approx(3.0f));

    locomotion.update(head, 0.0f, LocomotionInput{}, kDt, &world);
    CHECK_FALSE(locomotion.arc());
    CHECK(locomotion.counters().teleports == 1);
    checkNear(locomotion.feet(), landing);
    CHECK(locomotion.head().position.x == Approx(landing.x).margin(1e-4));
    CHECK(locomotion.head().position.z == Approx(landing.z).margin(1e-4));
    // The blink: black at once, back after kBlinkSeconds.
    CHECK(locomotion.fade() == Approx(1.0f));
    for (int frame = 0; frame < 12; ++frame) {
        locomotion.update(head, 0.0f, LocomotionInput{}, kDt, &world);
    }
    CHECK(locomotion.fade() == 0.0f);

    // Aimed at the wall and let go: no jump.
    Locomotion blocked;
    blocked.place(glm::vec3(0.0f, 0.0f, -4.4f), 0.0f);
    const Pose ahead = tracked(glm::vec3(0.0f, 1.65f, 0.0f), 0.0f, 10.0f);
    blocked.update(ahead, 0.0f, aim, kDt, &world);
    REQUIRE(blocked.arc());
    CHECK_FALSE(blocked.arc()->landing);
    blocked.update(ahead, 0.0f, LocomotionInput{}, kDt, &world);
    CHECK(blocked.counters().teleports == 0);
    CHECK(blocked.feet().z == Approx(-4.4f));
}

TEST_CASE("xr/locomotion: the comfort vignette closes in while the stick walks, off by default", "[xr][locomotion]") {
    const Pose head = tracked(glm::vec3(0.0f, 1.65f, 0.0f));
    LocomotionInput walk;
    walk.move = glm::vec2(0.0f, 1.0f);

    Locomotion plain;
    plain.place(glm::vec3(0.0f), 0.0f);
    for (int frame = 0; frame < 30; ++frame) {
        plain.update(head, 0.0f, walk, kDt, nullptr);
    }
    CHECK(plain.tunnel() == 0.0f);

    LocomotionSettings settings;
    settings.comfort.vignette = 0.6f;
    Locomotion comfort;
    comfort.setSettings(settings);
    comfort.place(glm::vec3(0.0f), 0.0f);
    comfort.update(head, 0.0f, walk, kDt, nullptr);
    CHECK(comfort.tunnel() > 0.0f);
    CHECK(comfort.tunnel() < 0.6f);
    for (int frame = 0; frame < 30; ++frame) {
        comfort.update(head, 0.0f, walk, kDt, nullptr);
    }
    CHECK(comfort.tunnel() == Approx(0.6f));
    // A snap turn moves nothing smoothly: the vignette opens again while standing.
    LocomotionInput snap;
    snap.turn = 1.0f;
    for (int frame = 0; frame < 30; ++frame) {
        comfort.update(head, 0.0f, snap, kDt, nullptr);
    }
    CHECK(comfort.tunnel() == 0.0f);
}

TEST_CASE("xr/locomotion: bad input changes nothing", "[xr][locomotion]") {
    Locomotion locomotion;
    locomotion.place(glm::vec3(1.0f, 0.0f, 1.0f), 0.0f);
    locomotion.update(tracked(glm::vec3(0.0f, 1.65f, 0.0f)), 0.0f, LocomotionInput{}, kDt, nullptr);
    const glm::vec3 before = locomotion.head().position;
    locomotion.update(tracked(glm::vec3(std::numeric_limits<float>::quiet_NaN(), 1.65f, 0.0f)), 0.0f,
                      LocomotionInput{}, kDt, nullptr);
    checkNear(locomotion.head().position, before);
    LocomotionInput nonsense;
    nonsense.move = glm::vec2(std::numeric_limits<float>::infinity(), 0.0f);
    nonsense.turn = std::numeric_limits<float>::quiet_NaN();
    nonsense.turnRadians = std::numeric_limits<float>::infinity();
    locomotion.update(tracked(glm::vec3(0.0f, 1.65f, 0.0f)), 0.0f, nonsense, -1.0f, nullptr);
    checkNear(locomotion.head().position, before);
    locomotion.place(glm::vec3(std::numeric_limits<float>::quiet_NaN()), 0.0f);
    locomotion.update(tracked(glm::vec3(0.0f, 1.65f, 0.0f)), 0.0f, LocomotionInput{}, kDt, nullptr);
    checkNear(locomotion.head().position, before);
}
