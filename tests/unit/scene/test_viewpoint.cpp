#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/math.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/viewpoint.hpp>

using Catch::Approx;
using rv::core::Aabb;
using rv::core::degreesToRadians;
using rv::scene::CameraLens;
using rv::scene::frontDirection;
using rv::scene::frontViewpoint;
using rv::scene::ViewArea;
using rv::scene::WalkerPose;

namespace {

constexpr float kEyeHeight = 1.65f;
constexpr float kAspect = 16.0f / 9.0f;

/// The forward vector of a walker pose, as the Camera computes it.
glm::vec3 forwardOf(const WalkerPose& pose) {
    rv::scene::Camera camera;
    camera.setPose(glm::vec3(0.0f), pose.yaw, pose.pitch);
    return camera.forward();
}

/// Where `point` appears in the view of a walker at `pose` (eye kEyeHeight above the feet), in normalized device
/// coordinates.
glm::vec2 ndcOf(const WalkerPose& pose, const CameraLens& lens, const glm::vec3& point) {
    rv::scene::Camera camera;
    camera.setLens(lens);
    camera.setAspect(kAspect);
    camera.setPose(pose.feet + glm::vec3(0.0f, kEyeHeight, 0.0f), pose.yaw, pose.pitch);
    const glm::vec4 clip = camera.viewProjection() * glm::vec4(point, 1.0f);
    return glm::vec2(clip) / clip.w;
}

} // namespace

TEST_CASE("scene/viewpoint: the front of an object is its local +Z on the floor", "[scene][viewpoint]") {
    const glm::vec3 identity = frontDirection(glm::mat4(1.0f));
    CHECK(identity.x == Approx(0.0f).margin(1e-6));
    CHECK(identity.z == Approx(1.0f));

    // Turned 90 degrees about +Y, the front faces +X; scale does not matter.
    const glm::mat4 turned =
        glm::scale(glm::rotate(glm::mat4(1.0f), degreesToRadians(90.0f), glm::vec3(0, 1, 0)), glm::vec3(3.0f));
    const glm::vec3 right = frontDirection(turned);
    CHECK(right.x == Approx(1.0f));
    CHECK(right.y == 0.0f);
    CHECK(right.z == Approx(0.0f).margin(1e-6));

    // Tipped back so that the front points straight up: no horizontal front, +Z is used.
    const glm::mat4 tipped = glm::rotate(glm::mat4(1.0f), degreesToRadians(-90.0f), glm::vec3(1, 0, 0));
    CHECK(frontDirection(tipped) == glm::vec3(0.0f, 0.0f, 1.0f));
    glm::mat4 broken(1.0f);
    broken[2][0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(frontDirection(broken) == glm::vec3(0.0f, 0.0f, 1.0f));
}

TEST_CASE("scene/viewpoint: the walker faces an object from far enough to see all of it", "[scene][viewpoint]") {
    // The test room's shelf: 1.2 x 2.0 x 0.35 m standing at (-1.5, 0, -4.8), facing +Z.
    const Aabb shelf = Aabb::fromMinMax(glm::vec3(-2.1f, 0.0f, -4.975f), glm::vec3(-0.9f, 2.0f, -4.625f));
    const CameraLens lens; // 75 degrees horizontal
    const WalkerPose pose = frontViewpoint(shelf, glm::vec3(0, 0, 1), 0.0f, kEyeHeight, lens, kAspect);

    // Its 2 m height decides: 1 m / tan(half the vertical FOV) with the margin, from its front at z = -4.625.
    const float tanHalfVertical =
        std::tan(0.5f * rv::scene::Camera::verticalFovRadians(degreesToRadians(75.0f), kAspect));
    const float distance = 1.0f / tanHalfVertical * rv::scene::kViewMargin;
    CHECK(pose.feet.x == Approx(-1.5f));
    CHECK(pose.feet.y == 0.0f);
    CHECK(pose.feet.z == Approx(-4.625f + distance));
    // Looking at -Z (yaw 0), slightly down at the shelf's center 1 m above the floor.
    CHECK(pose.yaw == Approx(0.0f).margin(1e-6));
    CHECK(pose.pitch == Approx(std::atan2(1.0f - kEyeHeight, pose.feet.z + 4.8f)));
    const glm::vec3 forward = forwardOf(pose);
    CHECK(forward.z < 0.0f);
    CHECK(forward.y < 0.0f);

    // The feet keep the requested floor height.
    CHECK(frontViewpoint(shelf, glm::vec3(0, 0, 1), 0.25f, kEyeHeight, lens, kAspect).feet.y == 0.25f);
}

TEST_CASE("scene/viewpoint: turned objects are seen from their front", "[scene][viewpoint]") {
    // A 0.6 m cube facing +X at (2, 0, 1): the walker stands on its +X side looking towards -X.
    const Aabb cube = Aabb::fromMinMax(glm::vec3(1.7f, 0.0f, 0.7f), glm::vec3(2.3f, 0.6f, 1.3f));
    const WalkerPose pose = frontViewpoint(cube, glm::vec3(1, 0, 0), 0.0f, kEyeHeight, CameraLens{}, kAspect);
    CHECK(pose.feet.x > 2.3f + rv::scene::kMinViewDistance - 1e-4f);
    CHECK(pose.feet.z == Approx(1.0f));
    CHECK(pose.yaw == Approx(-rv::core::kHalfPi));
    const glm::vec3 forward = forwardOf(pose);
    CHECK(forward.x < -0.5f);
}

TEST_CASE("scene/viewpoint: distances stay between the near and far limits", "[scene][viewpoint]") {
    const CameraLens lens;
    // A cartridge is closer than the nearest viewpoint allows: the walker stops at the minimum distance.
    const Aabb cartridge = Aabb::fromMinMax(glm::vec3(-0.05f, 1.0f, -0.01f), glm::vec3(0.05f, 1.15f, 0.01f));
    const WalkerPose close = frontViewpoint(cartridge, glm::vec3(0, 0, 1), 0.0f, kEyeHeight, lens, kAspect);
    CHECK(close.feet.z == Approx(0.01f + rv::scene::kMinViewDistance));
    CHECK(close.pitch < 0.0f); // looking down at it

    // A 3.9 m wide wall unit is seen from the farthest distance.
    const Aabb wide = Aabb::fromMinMax(glm::vec3(-1.95f, 0.0f, -0.2f), glm::vec3(1.95f, 3.5f, 0.2f));
    const WalkerPose far = frontViewpoint(wide, glm::vec3(0, 0, 1), 0.0f, kEyeHeight, lens, kAspect);
    CHECK(far.feet.z == Approx(0.2f + rv::scene::kMaxViewDistance));
}

TEST_CASE("scene/viewpoint: a room is entered at its center, looking from its front to its back",
          "[scene][viewpoint]") {
    const Aabb room = Aabb::fromMinMax(glm::vec3(-4.0f, 0.0f, -5.0f), glm::vec3(4.0f, 3.0f, 5.0f));
    const WalkerPose pose = frontViewpoint(room, glm::vec3(0, 0, 1), 0.0f, kEyeHeight, CameraLens{}, kAspect);
    CHECK(pose.feet == glm::vec3(0.0f));
    CHECK(pose.yaw == Approx(0.0f).margin(1e-6));
    CHECK(pose.pitch == 0.0f);

    // Empty bounds count as a point at the origin.
    const WalkerPose nothing = frontViewpoint(Aabb{}, glm::vec3(0, 0, 1), 0.0f, kEyeHeight, CameraLens{}, kAspect);
    CHECK(nothing.feet == glm::vec3(0.0f, 0.0f, rv::scene::kMinViewDistance));
}

TEST_CASE("scene/viewpoint: objects long in one direction only are looked at, not entered", "[scene][viewpoint]") {
    // A 4.5 m shelf against a wall, facing +Z: wider than kLargeObjectExtent, but only 0.35 m deep.
    const Aabb longShelf = Aabb::fromMinMax(glm::vec3(-2.25f, 0.0f, -4.975f), glm::vec3(2.25f, 2.0f, -4.625f));
    const WalkerPose pose = frontViewpoint(longShelf, glm::vec3(0, 0, 1), 0.0f, kEyeHeight, CameraLens{}, kAspect);
    CHECK(pose.feet.x == Approx(0.0f).margin(1e-5));
    CHECK(pose.feet.z > -4.625f + rv::scene::kMinViewDistance - 1e-4f);
    CHECK(pose.feet.z <= -4.625f + rv::scene::kMaxViewDistance + 1e-4f);
    CHECK(forwardOf(pose).z < 0.0f);

    // The same counter turned to face +X: long along z only.
    const Aabb turned = Aabb::fromMinMax(glm::vec3(3.9f, 0.0f, -2.25f), glm::vec3(4.25f, 1.0f, 2.25f));
    const WalkerPose side = frontViewpoint(turned, glm::vec3(-1, 0, 0), 0.0f, kEyeHeight, CameraLens{}, kAspect);
    CHECK(side.feet.x < 3.9f - rv::scene::kMinViewDistance + 1e-4f);
    CHECK(side.feet.z == Approx(0.0f).margin(1e-5));
}

TEST_CASE("scene/viewpoint: the object is framed in the part of the view the overlay leaves visible",
          "[scene][viewpoint]") {
    const Aabb shelf = Aabb::fromMinMax(glm::vec3(-2.1f, 0.0f, -4.975f), glm::vec3(-0.9f, 2.0f, -4.625f));
    const CameraLens lens;
    // The see-through center of the overlay's default layout in a 1280 x 720 window: the side columns and the log row
    // under the scene cover the rest.
    const ViewArea visible{glm::vec2(-0.54f, -0.52f), glm::vec2(0.54f, 1.0f)};
    const WalkerPose pose = frontViewpoint(shelf, glm::vec3(0, 0, 1), 0.0f, kEyeHeight, lens, kAspect, visible);

    // The center of the shelf appears at the center of that area, and every corner inside it.
    const glm::vec2 center = ndcOf(pose, lens, shelf.center());
    CHECK(center.x == Approx(0.0f).margin(1e-3));
    CHECK(center.y == Approx(0.24f).margin(1e-3));
    for (int corner = 0; corner < 8; ++corner) {
        const glm::vec3 point((corner & 1) != 0 ? shelf.max.x : shelf.min.x, (corner & 2) != 0 ? shelf.max.y : shelf.min.y,
                              (corner & 4) != 0 ? shelf.max.z : shelf.min.z);
        const glm::vec2 ndc = ndcOf(pose, lens, point);
        CHECK(ndc.x > visible.min.x);
        CHECK(ndc.x < visible.max.x);
        CHECK(ndc.y > visible.min.y);
        CHECK(ndc.y < visible.max.y);
    }
    // A smaller area needs more distance than the whole view.
    const WalkerPose whole = frontViewpoint(shelf, glm::vec3(0, 0, 1), 0.0f, kEyeHeight, lens, kAspect);
    CHECK(pose.feet.z > whole.feet.z);
    CHECK(pose.feet.x == Approx(whole.feet.x));
}

TEST_CASE("scene/viewpoint: an area off to one side turns the walker away from the object", "[scene][viewpoint]") {
    // The right 60% of the view: the object must appear right of the view's center, so the walker looks left of it.
    const Aabb cube = Aabb::fromMinMax(glm::vec3(-0.3f, 0.6f, -0.3f), glm::vec3(0.3f, 1.2f, 0.3f));
    const CameraLens lens;
    const ViewArea right{glm::vec2(-0.2f, -1.0f), glm::vec2(1.0f, 1.0f)};
    const WalkerPose pose = frontViewpoint(cube, glm::vec3(0, 0, 1), 0.0f, kEyeHeight, lens, kAspect, right);
    const glm::vec2 center = ndcOf(pose, lens, cube.center());
    CHECK(center.x == Approx(0.4f).margin(1e-3));
    CHECK(center.y == Approx(0.0f).margin(1e-3));
    CHECK(pose.yaw < 0.0f); // turned left of -Z

    // Empty, inverted or non-finite areas count as the whole view.
    const WalkerPose whole = frontViewpoint(cube, glm::vec3(0, 0, 1), 0.0f, kEyeHeight, lens, kAspect);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const ViewArea& unusable : {ViewArea{glm::vec2(0.5f), glm::vec2(0.5f)},
                                     ViewArea{glm::vec2(1.0f, 1.0f), glm::vec2(-1.0f, -1.0f)},
                                     ViewArea{glm::vec2(nan, -1.0f), glm::vec2(1.0f, 1.0f)}}) {
        const WalkerPose fallback = frontViewpoint(cube, glm::vec3(0, 0, 1), 0.0f, kEyeHeight, lens, kAspect, unusable);
        CHECK(fallback.feet == whole.feet);
        CHECK(fallback.yaw == whole.yaw);
        CHECK(fallback.pitch == whole.pitch);
    }
}
