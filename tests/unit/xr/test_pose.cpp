#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <rv/core/math.hpp>
#include <rv/scene/camera.hpp>
#include <rv/xr/pose.hpp>

using Catch::Approx;
using rv::core::degreesToRadians;
using rv::xr::Angles;
using rv::xr::Pose;

namespace {

void checkNear(const glm::vec3& actual, const glm::vec3& expected, float margin = 1e-5f) {
    CHECK(actual.x == Approx(expected.x).margin(margin));
    CHECK(actual.y == Approx(expected.y).margin(margin));
    CHECK(actual.z == Approx(expected.z).margin(margin));
}

} // namespace

TEST_CASE("xr/pose: yaw, pitch and roll match the camera's and come back (I10 step 2)", "[xr][pose]") {
    const Angles angles{degreesToRadians(40.0f), degreesToRadians(-20.0f), degreesToRadians(15.0f)};
    const glm::quat q = rv::xr::orientationOf(angles);
    const Angles back = rv::xr::anglesOf(q);
    CHECK(back.yaw == Approx(angles.yaw).margin(1e-5));
    CHECK(back.pitch == Approx(angles.pitch).margin(1e-5));
    CHECK(back.roll == Approx(angles.roll).margin(1e-5));

    // Without roll, the camera's own yaw/pitch orientation.
    rv::scene::Camera camera;
    camera.setPose(glm::vec3(0.0f), angles.yaw, angles.pitch);
    const glm::quat noRoll = rv::xr::orientationOf(Angles{angles.yaw, angles.pitch, 0.0f});
    checkNear(noRoll * glm::vec3(0.0f, 0.0f, -1.0f), camera.forward());
    checkNear(noRoll * glm::vec3(1.0f, 0.0f, 0.0f), camera.right());

    // A positive roll tilts the head to the right: its right vector goes down.
    const glm::quat tilted = rv::xr::orientationOf(Angles{0.0f, 0.0f, degreesToRadians(30.0f)});
    CHECK((tilted * glm::vec3(1.0f, 0.0f, 0.0f)).y < 0.0f);
}

TEST_CASE("xr/pose: the tracking space stands on the walker's feet, turned by its yaw (I10 step 2)", "[xr][pose]") {
    const rv::xr::TrackingAnchor anchor{glm::vec3(2.0f, 0.5f, -3.0f), degreesToRadians(90.0f), 0.0f};
    // A head 1.6 m up and 0.2 m in front of the space's origin, looking along the space's -Z.
    const Pose head{glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.6f, -0.2f)};
    const Pose world = rv::xr::toWorld(anchor, head);
    // A walker at yaw 90 degrees faces +X: the head is 0.2 m along +X from the feet and looks along +X.
    checkNear(world.position, glm::vec3(2.2f, 2.1f, -3.0f));
    checkNear(world.orientation * glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(1.0f, 0.0f, 0.0f));

    // LOCAL's origin is the head itself: raised by the floor offset.
    const rv::xr::TrackingAnchor local{glm::vec3(0.0f), 0.0f, 1.6f};
    checkNear(rv::xr::toWorld(local, Pose{}).position, glm::vec3(0.0f, 1.6f, 0.0f));
}

TEST_CASE("xr/pose: the head between two eyes and the field holding both (I10 step 2)", "[xr][pose]") {
    rv::xr::EyeViews eyes{};
    eyes[0].pose.position = glm::vec3(-0.032f, 1.6f, 0.0f);
    eyes[1].pose.position = glm::vec3(0.032f, 1.6f, 0.0f);
    eyes[0].fov = rv::scene::EyeFov{-0.9f, 0.8f, 0.85f, -0.87f};
    eyes[1].fov = rv::scene::EyeFov{-0.8f, 0.9f, 0.84f, -0.88f};
    const Pose head = rv::xr::headOf(eyes);
    checkNear(head.position, glm::vec3(0.0f, 1.6f, 0.0f));
    const rv::scene::EyeFov field = rv::xr::combinedField(eyes);
    CHECK(field.left == Approx(-0.9f));
    CHECK(field.right == Approx(0.9f));
    CHECK(field.up == Approx(0.85f));
    CHECK(field.down == Approx(-0.88f));
}
