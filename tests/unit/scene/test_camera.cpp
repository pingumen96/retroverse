#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/quaternion.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/math.hpp>
#include <rv/scene/camera.hpp>

using Catch::Approx;
using rv::core::Aabb;
using rv::core::degreesToRadians;
using rv::core::radiansToDegrees;
using rv::scene::Camera;
using rv::scene::CameraLens;
using rv::scene::Frustum;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

/// Angle between two directions, in degrees.
float angleDeg(const glm::vec3& a, const glm::vec3& b) {
    return radiansToDegrees(std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f)));
}

/// Camera at the origin looking down -Z with the default lens and a 16:9 viewport.
Camera originCamera() {
    Camera camera;
    camera.setAspect(16.0f / 9.0f);
    camera.setPose(glm::vec3(0.0f), 0.0f, 0.0f);
    return camera;
}

} // namespace

TEST_CASE("scene/camera: horizontal FOV converts to vertical with the aspect ratio", "[scene][camera]") {
    // 2 * atan(tan(37.5 deg) / (16 / 9)) = 46.692 degrees (the configured FOV is horizontal).
    const float vertical = Camera::verticalFovRadians(degreesToRadians(75.0f), 16.0f / 9.0f);
    CHECK(radiansToDegrees(vertical) == Approx(46.6921).margin(1e-3));
    CHECK(Camera::verticalFovRadians(1.0f, 1.0f) == Approx(1.0f));
    CHECK(Camera::verticalFovRadians(1.0f, 2.0f) < 1.0f);
    // Invalid aspect ratios count as 1 instead of producing NaN or infinity.
    CHECK(Camera::verticalFovRadians(1.0f, 0.0f) == Approx(1.0f));
    CHECK(Camera::verticalFovRadians(1.0f, kNaN) == Approx(1.0f));

    Camera camera = originCamera();
    CHECK(radiansToDegrees(camera.verticalFovRadians()) == Approx(46.6921).margin(1e-3));
    const glm::mat4 projection = camera.projection();
    // x scale = 1 / tan(horizontal / 2), y scale = 1 / tan(vertical / 2): the horizontal FOV is preserved.
    CHECK(projection[0][0] == Approx(1.0f / std::tan(degreesToRadians(37.5f))));
    CHECK(projection[1][1] == Approx(1.0f / std::tan(0.5f * vertical)));
    // OpenGL clip space: a point on the near plane maps to depth -1, on the far plane to +1.
    const glm::vec4 nearPoint = projection * glm::vec4(0.0f, 0.0f, -0.05f, 1.0f);
    const glm::vec4 farPoint = projection * glm::vec4(0.0f, 0.0f, -100.0f, 1.0f);
    CHECK(nearPoint.z / nearPoint.w == Approx(-1.0f));
    CHECK(farPoint.z / farPoint.w == Approx(1.0f));
}

TEST_CASE("scene/camera: basis vectors are orthonormal at any yaw and pitch", "[scene][camera]") {
    Camera camera;
    for (const float yaw : {-3.0f, -1.2f, 0.0f, 0.7f, 2.5f}) {
        for (const float pitch : {-1.55f, -0.5f, 0.0f, 0.4f, 1.55f}) {
            camera.setPose(glm::vec3(1.0f, 2.0f, 3.0f), yaw, pitch);
            const glm::vec3 f = camera.forward();
            const glm::vec3 r = camera.right();
            const glm::vec3 u = camera.up();
            CHECK(glm::length(f) == Approx(1.0f));
            CHECK(glm::length(r) == Approx(1.0f));
            CHECK(glm::length(u) == Approx(1.0f));
            CHECK(glm::dot(f, r) == Approx(0.0f).margin(1e-6));
            CHECK(glm::dot(f, u) == Approx(0.0f).margin(1e-6));
            CHECK(glm::dot(r, u) == Approx(0.0f).margin(1e-6));
            CHECK(r.y == 0.0f);                                        // no roll
            CHECK(rv::core::nearlyEqual(glm::cross(r, u), -f, 1e-5f)); // right-handed
        }
    }
}

TEST_CASE("scene/camera: yaw and pitch conventions", "[scene][camera]") {
    Camera camera;
    camera.setPose(glm::vec3(0.0f), 0.0f, 0.0f);
    CHECK(rv::core::nearlyEqual(camera.forward(), glm::vec3(0.0f, 0.0f, -1.0f)));
    CHECK(rv::core::nearlyEqual(camera.right(), glm::vec3(1.0f, 0.0f, 0.0f)));
    CHECK(rv::core::nearlyEqual(camera.up(), glm::vec3(0.0f, 1.0f, 0.0f)));

    camera.setPose(glm::vec3(0.0f), rv::core::kHalfPi, 0.0f); // positive yaw turns right, towards +X
    CHECK(rv::core::nearlyEqual(camera.forward(), glm::vec3(1.0f, 0.0f, 0.0f)));
    CHECK(rv::core::nearlyEqual(camera.right(), glm::vec3(0.0f, 0.0f, 1.0f)));

    camera.setPose(glm::vec3(0.0f), rv::core::kPi, 0.0f);
    CHECK(rv::core::nearlyEqual(camera.forward(), glm::vec3(0.0f, 0.0f, 1.0f)));

    camera.setPose(glm::vec3(0.0f), 0.0f, degreesToRadians(30.0f)); // positive pitch looks up
    CHECK(camera.forward().y == Approx(0.5f));
    CHECK(camera.forward().z == Approx(-std::cos(degreesToRadians(30.0f))));
    CHECK(camera.up().z > 0.0f); // looking up tilts the top of the view back, towards +Z behind the viewer

    const float yaw = 0.6f;
    const float pitch = -0.3f;
    camera.setPose(glm::vec3(0.0f), yaw, pitch);
    const glm::vec3 expected(std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
    CHECK(rv::core::nearlyEqual(camera.forward(), expected));
    CHECK(camera.yaw() == yaw);
    CHECK(camera.pitch() == pitch);
}

TEST_CASE("scene/camera: view matrix moves the eye to the origin looking down -Z", "[scene][camera]") {
    Camera camera;
    const glm::vec3 eye(1.5f, 1.65f, -2.0f);
    camera.setPose(eye, 0.8f, -0.25f);
    const glm::mat4 view = camera.view();

    CHECK(rv::core::nearlyEqual(glm::vec3(view * glm::vec4(eye, 1.0f)), glm::vec3(0.0f)));
    CHECK(rv::core::nearlyEqual(glm::vec3(view * glm::vec4(eye + 5.0f * camera.forward(), 1.0f)),
                                glm::vec3(0.0f, 0.0f, -5.0f)));
    CHECK(rv::core::nearlyEqual(glm::vec3(view * glm::vec4(eye + camera.right(), 1.0f)), glm::vec3(1.0f, 0.0f, 0.0f)));
    CHECK(rv::core::nearlyEqual(glm::vec3(view * glm::vec4(eye + camera.up(), 1.0f)), glm::vec3(0.0f, 1.0f, 0.0f)));
    CHECK(rv::core::nearlyEqual(camera.viewProjection(), camera.projection() * view));
}

TEST_CASE("scene/camera: frustum planes face inwards", "[scene][camera][frustum]") {
    const Frustum frustum = originCamera().frustum();
    for (const auto& plane : frustum.planes) {
        CHECK(glm::length(plane.normal) == Approx(1.0f));
    }
    CHECK(rv::core::nearlyEqual(frustum.planes[Frustum::Near].normal, glm::vec3(0.0f, 0.0f, -1.0f)));
    CHECK(rv::core::nearlyEqual(frustum.planes[Frustum::Far].normal, glm::vec3(0.0f, 0.0f, 1.0f)));
    // The eye sits 5 cm behind the near plane and 100 m in front of the far plane.
    CHECK(frustum.planes[Frustum::Near].signedDistance(glm::vec3(0.0f)) == Approx(-0.05f).margin(1e-4));
    CHECK(frustum.planes[Frustum::Far].signedDistance(glm::vec3(0.0f)) == Approx(100.0f).epsilon(1e-3));
    // Side planes pass through the eye and are tilted by half the horizontal FOV.
    CHECK(frustum.planes[Frustum::Left].signedDistance(glm::vec3(0.0f)) == Approx(0.0f).margin(1e-5));
    CHECK(angleDeg(frustum.planes[Frustum::Left].normal, glm::vec3(1.0f, 0.0f, 0.0f)) == Approx(37.5f).margin(1e-3));
}

TEST_CASE("scene/camera: frustum culls boxes outside and keeps boxes inside or straddling",
          "[scene][camera][frustum]") {
    const Frustum frustum = originCamera().frustum();
    const auto box = [](glm::vec3 center, float half) {
        return Aabb::fromCenterHalfExtents(center, glm::vec3(half));
    };

    CHECK(frustum.intersects(box({0.0f, 0.0f, -10.0f}, 1.0f)));      // straight ahead
    CHECK_FALSE(frustum.intersects(box({0.0f, 0.0f, 10.0f}, 1.0f))); // behind
    // The left plane crosses z = -10 at x = -10 tan(37.5 deg) = -7.67: this box straddles it.
    CHECK(frustum.intersects(Aabb::fromMinMax({-9.0f, -1.0f, -11.0f}, {-7.0f, 1.0f, -9.0f})));
    CHECK_FALSE(frustum.intersects(Aabb::fromMinMax({-20.0f, -1.0f, -11.0f}, {-18.0f, 1.0f, -9.0f}))); // left
    CHECK_FALSE(frustum.intersects(Aabb::fromMinMax({18.0f, -1.0f, -11.0f}, {20.0f, 1.0f, -9.0f})));   // right
    // The top plane crosses z = -10 at y = 10 tan(23.35 deg) = 4.32.
    CHECK_FALSE(frustum.intersects(box({0.0f, 6.0f, -10.0f}, 1.0f)));
    CHECK(frustum.intersects(box({0.0f, 4.5f, -10.0f}, 1.0f)));
    CHECK_FALSE(frustum.intersects(box({0.0f, -6.0f, -10.0f}, 1.0f)));
    CHECK_FALSE(frustum.intersects(Aabb::fromMinMax({-0.01f, -0.01f, -0.04f}, {0.01f, 0.01f, -0.01f}))); // near
    CHECK_FALSE(frustum.intersects(box({0.0f, 0.0f, -120.0f}, 5.0f)));                                   // far
    CHECK(frustum.intersects(box({0.0f, 0.0f, -100.0f}, 1.0f))); // straddles far
    CHECK(frustum.intersects(box({0.0f, 0.0f, 0.0f}, 0.5f)));    // contains the eye, crosses the near plane
    CHECK_FALSE(frustum.intersects(Aabb{}));

    CHECK(frustum.contains(glm::vec3(0.0f, 0.0f, -10.0f)));
    CHECK(frustum.contains(glm::vec3(0.0f, 0.0f, -99.0f)));
    CHECK_FALSE(frustum.contains(glm::vec3(0.0f, 0.0f, -101.0f)));
    CHECK_FALSE(frustum.contains(glm::vec3(0.0f, 0.0f, -0.01f)));
    CHECK_FALSE(frustum.contains(glm::vec3(0.0f, 0.0f, 10.0f)));

    // The frustum follows the pose: looking towards +X.
    Camera turned = originCamera();
    turned.setPose(glm::vec3(0.0f, 1.65f, 0.0f), rv::core::kHalfPi, 0.0f);
    CHECK(turned.frustum().intersects(box({10.0f, 1.65f, 0.0f}, 0.5f)));
    CHECK_FALSE(turned.frustum().intersects(box({-10.0f, 1.65f, 0.0f}, 0.5f)));
    CHECK_FALSE(turned.frustum().intersects(box({0.0f, 1.65f, -10.0f}, 0.5f)));
}

TEST_CASE("scene/camera: rays from NDC start at the eye and match the projection", "[scene][camera]") {
    Camera camera;
    camera.setAspect(16.0f / 9.0f);
    const glm::vec3 eye(-1.0f, 1.65f, 2.0f);
    camera.setPose(eye, 0.4f, -0.2f);

    const rv::core::Ray center = camera.rayFromNdc(glm::vec2(0.0f));
    CHECK(rv::core::nearlyEqual(center.origin, eye));
    CHECK(rv::core::nearlyEqual(center.direction, camera.forward()));
    CHECK(glm::length(center.direction) == Approx(1.0f));

    // NDC x = 1 is the right edge: half the horizontal FOV away from the view axis.
    CHECK(angleDeg(camera.rayFromNdc({1.0f, 0.0f}).direction, camera.forward()) == Approx(37.5f).margin(1e-3));
    const glm::vec3 top = camera.rayFromNdc({0.0f, 1.0f}).direction;
    CHECK(angleDeg(top, camera.forward()) == Approx(0.5f * radiansToDegrees(camera.verticalFovRadians())).margin(1e-3));
    CHECK(glm::dot(top, camera.up()) > 0.0f); // NDC y grows upwards

    // A point along any ray projects back to the NDC it came from.
    for (const glm::vec2 ndc : {glm::vec2(0.3f, -0.6f), glm::vec2(-0.9f, 0.8f), glm::vec2(1.0f, -1.0f)}) {
        const glm::vec3 point = camera.rayFromNdc(ndc).at(7.0f);
        const glm::vec4 clip = camera.viewProjection() * glm::vec4(point, 1.0f);
        CHECK(clip.x / clip.w == Approx(ndc.x).margin(1e-4));
        CHECK(clip.y / clip.w == Approx(ndc.y).margin(1e-4));
    }
}

TEST_CASE("scene/camera: invalid lens, aspect and pose values are rejected", "[scene][camera]") {
    Camera camera;
    camera.setLens(CameraLens{80.0f, 0.1f, 50.0f});
    CHECK(camera.lens().horizontalFovDeg == 80.0f);
    CHECK(camera.lens().nearPlane == 0.1f);
    CHECK(camera.lens().farPlane == 50.0f);

    camera.setLens(CameraLens{200.0f, 0.1f, 50.0f}); // FOV is clamped, the rest applies
    CHECK(camera.lens().horizontalFovDeg == Camera::kMaxHorizontalFovDeg);
    camera.setLens(CameraLens{0.0f, 0.1f, 50.0f});
    CHECK(camera.lens().horizontalFovDeg == Camera::kMinHorizontalFovDeg);

    camera.setLens(CameraLens{70.0f, 0.1f, 50.0f});
    camera.setLens(CameraLens{75.0f, 0.0f, 50.0f}); // near must be > 0
    camera.setLens(CameraLens{75.0f, 1.0f, 0.5f});  // far must exceed near
    camera.setLens(CameraLens{kNaN, 0.1f, 50.0f});  // non-finite
    CHECK(camera.lens().horizontalFovDeg == 70.0f);
    CHECK(camera.lens().nearPlane == 0.1f);
    CHECK(camera.lens().farPlane == 50.0f);

    camera.setAspect(2.0f);
    camera.setAspect(0.0f);
    camera.setAspect(-1.0f);
    camera.setAspect(std::numeric_limits<float>::infinity());
    CHECK(camera.aspect() == 2.0f);

    camera.setPose(glm::vec3(1.0f, 2.0f, 3.0f), 0.5f, 0.25f);
    camera.setPose(glm::vec3(kNaN, 5.0f, kNaN), kNaN, std::numeric_limits<float>::infinity());
    CHECK(camera.position() == glm::vec3(1.0f, 5.0f, 3.0f));
    CHECK(camera.yaw() == 0.5f);
    CHECK(camera.pitch() == 0.25f);
    CHECK(rv::core::isFinite(camera.viewProjection()));
}

namespace {

/// The left eye of a headset measured in the I10 spike (Quest 3S over Link): wider outwards than inwards.
rv::scene::EyeFov questLeftEye() {
    return {degreesToRadians(-52.0f), degreesToRadians(45.0f), degreesToRadians(48.0f), degreesToRadians(-50.0f)};
}

} // namespace

TEST_CASE("scene/camera: the off-axis projection puts the field's edges on NDC's (I10 step 1)",
          "[scene][camera][eye]") {
    using rv::scene::offAxisProjection;
    const rv::scene::EyeFov fov = questLeftEye();
    const glm::mat4 projection = offAxisProjection(fov, 0.05f, 100.0f);
    const auto ndcOf = [&](float x, float y, float z) {
        const glm::vec4 clip = projection * glm::vec4(x, y, z, 1.0f);
        return glm::vec3(clip) / clip.w;
    };
    // A point 2 m ahead on each edge of the field.
    CHECK(ndcOf(2.0f * std::tan(fov.left), 0.0f, -2.0f).x == Approx(-1.0f));
    CHECK(ndcOf(2.0f * std::tan(fov.right), 0.0f, -2.0f).x == Approx(1.0f));
    CHECK(ndcOf(0.0f, 2.0f * std::tan(fov.up), -2.0f).y == Approx(1.0f));
    CHECK(ndcOf(0.0f, 2.0f * std::tan(fov.down), -2.0f).y == Approx(-1.0f));
    CHECK(ndcOf(0.0f, 0.0f, -0.05f).z == Approx(-1.0f));
    CHECK(ndcOf(0.0f, 0.0f, -100.0f).z == Approx(1.0f));

    // A symmetric field is glm's perspective (the desktop's projection).
    const float half = degreesToRadians(30.0f);
    const glm::mat4 symmetric = offAxisProjection({-half, half, half, -half}, 0.05f, 100.0f);
    const glm::mat4 reference = glm::perspectiveRH_NO(2.0f * half, 1.0f, 0.05f, 100.0f);
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            CHECK(symmetric[column][row] == Approx(reference[column][row]).margin(1e-6));
        }
    }
}

TEST_CASE("scene/camera: an eye looks along its quaternion with its own field (I10 step 1)", "[scene][camera][eye]") {
    Camera camera = originCamera();
    const glm::vec3 position(0.3f, 1.6f, -2.0f);
    // Turned 90 degrees to the right (towards +X) and rolled 10 degrees.
    const glm::quat orientation = glm::angleAxis(degreesToRadians(-90.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
                                  glm::angleAxis(degreesToRadians(10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    camera.setEye(position, orientation, questLeftEye());
    REQUIRE(camera.eyeFov());
    CHECK(angleDeg(camera.forward(), glm::vec3(1.0f, 0.0f, 0.0f)) == Approx(0.0f).margin(1e-3));
    CHECK(radiansToDegrees(camera.yaw()) == Approx(90.0f).margin(1e-3));
    CHECK(radiansToDegrees(camera.pitch()) == Approx(0.0f).margin(1e-3));
    // The roll tips up() away from +Y.
    CHECK(angleDeg(camera.up(), glm::vec3(0.0f, 1.0f, 0.0f)) == Approx(10.0f).margin(1e-2));

    // The view brings the eye to the origin looking down -Z; a point ahead lands ahead.
    const glm::vec4 eyeInView = camera.view() * glm::vec4(position, 1.0f);
    CHECK(glm::length(glm::vec3(eyeInView)) == Approx(0.0f).margin(1e-5));
    const glm::vec4 ahead = camera.view() * glm::vec4(position + glm::vec3(3.0f, 0.0f, 0.0f), 1.0f);
    CHECK(ahead.z == Approx(-3.0f));

    // The aspect ratio and the lens's field no longer shape the projection.
    const glm::mat4 before = camera.projection();
    camera.setAspect(3.0f);
    camera.setLens(CameraLens{40.0f, 0.05f, 100.0f});
    CHECK(camera.projection() == before);
    CHECK(radiansToDegrees(camera.verticalFovRadians()) == Approx(98.0f).margin(1e-3));

    // Rays through NDC match the projection: the ray through a pixel projects back onto it.
    for (const glm::vec2 ndc : {glm::vec2(0.0f), glm::vec2(-0.9f, 0.4f), glm::vec2(0.7f, -0.8f)}) {
        const rv::core::Ray ray = camera.rayFromNdc(ndc);
        CHECK(ray.origin == position);
        const glm::vec4 clip = camera.viewProjection() * glm::vec4(ray.origin + ray.direction * 5.0f, 1.0f);
        CHECK(clip.x / clip.w == Approx(ndc.x).margin(1e-4));
        CHECK(clip.y / clip.w == Approx(ndc.y).margin(1e-4));
    }

    // setPose returns to the yaw/pitch mode, exactly as a camera that never was an eye.
    camera.setLens(CameraLens{});
    camera.setAspect(16.0f / 9.0f);
    camera.setPose(glm::vec3(0.0f), 0.0f, 0.0f);
    CHECK_FALSE(camera.eyeFov());
    CHECK(camera.viewProjection() == originCamera().viewProjection());
}

TEST_CASE("scene/camera: the yaw/pitch orientation is the quaternion of its basis (I10 step 1)",
          "[scene][camera][eye]") {
    Camera camera = originCamera();
    camera.setPose(glm::vec3(0.0f), degreesToRadians(35.0f), degreesToRadians(-20.0f));
    const glm::quat orientation = camera.orientation();
    CHECK(angleDeg(orientation * glm::vec3(0.0f, 0.0f, -1.0f), camera.forward()) == Approx(0.0f).margin(1e-3));
    CHECK(angleDeg(orientation * glm::vec3(1.0f, 0.0f, 0.0f), camera.right()) == Approx(0.0f).margin(1e-3));
    CHECK(angleDeg(orientation * glm::vec3(0.0f, 1.0f, 0.0f), camera.up()) == Approx(0.0f).margin(1e-3));
}

TEST_CASE("scene/camera: invalid eyes are rejected (I10 step 1)", "[scene][camera][eye]") {
    Camera camera = originCamera();
    rv::scene::EyeFov backwards = questLeftEye();
    std::swap(backwards.left, backwards.right);
    camera.setEye(glm::vec3(1.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), backwards);
    CHECK_FALSE(camera.eyeFov()); // ignored outside eye mode
    CHECK(camera.position() == glm::vec3(0.0f));

    camera.setEye(glm::vec3(1.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), questLeftEye());
    rv::scene::EyeFov tooWide = questLeftEye();
    tooWide.right = degreesToRadians(95.0f);
    camera.setEye(glm::vec3(kNaN, 2.0f, 2.0f), glm::quat(0.0f, 0.0f, 0.0f, 0.0f), tooWide);
    REQUIRE(camera.eyeFov());
    CHECK(camera.eyeFov()->right == Approx(degreesToRadians(45.0f))); // the previous field
    CHECK(camera.position() == glm::vec3(1.0f, 2.0f, 2.0f));
    CHECK(angleDeg(camera.forward(), glm::vec3(0.0f, 0.0f, -1.0f)) == Approx(0.0f).margin(1e-3));
    CHECK(rv::core::isFinite(camera.viewProjection()));
}

TEST_CASE("scene/camera: a frustum enclosing both eyes keeps what either sees (I10 step 1)",
          "[scene][camera][frustum][eye]") {
    // Two eyes 64 mm apart, mirrored fields, looking down -Z.
    const rv::scene::EyeFov leftFov = questLeftEye();
    const rv::scene::EyeFov rightFov{-leftFov.right, -leftFov.left, leftFov.up, leftFov.down};
    Camera leftEye;
    Camera rightEye;
    const glm::quat ahead(1.0f, 0.0f, 0.0f, 0.0f);
    leftEye.setEye(glm::vec3(-0.032f, 1.6f, 0.0f), ahead, leftFov);
    rightEye.setEye(glm::vec3(0.032f, 1.6f, 0.0f), ahead, rightFov);
    const std::array<glm::mat4, 2> both{leftEye.viewProjection(), rightEye.viewProjection()};
    const Frustum enclosing = Frustum::enclosing(both);

    // Every corner of both frusta is inside.
    for (const glm::mat4& viewProjection : both) {
        for (const glm::vec3& corner : Frustum::corners(viewProjection)) {
            for (const rv::scene::Plane& plane : enclosing.planes) {
                CHECK(plane.signedDistance(corner) >= -1e-3f);
            }
        }
    }
    // A box at the far left of the left eye's field, outside the right eye's: kept.
    const float x = -0.032f + 2.0f * std::tan(leftFov.left) + 0.05f;
    const Aabb leftOnly{glm::vec3(x, 1.5f, -2.05f), glm::vec3(x + 0.1f, 1.7f, -1.95f)};
    CHECK(leftEye.frustum().intersects(leftOnly));
    CHECK_FALSE(rightEye.frustum().intersects(leftOnly));
    CHECK(enclosing.intersects(leftOnly));
    // Behind both eyes: culled.
    CHECK_FALSE(enclosing.intersects(Aabb{glm::vec3(-0.5f, 1.0f, 1.0f), glm::vec3(0.5f, 2.0f, 2.0f)}));

    // One view: its own frustum. None: nothing rejected.
    const Frustum single = Frustum::enclosing(std::span<const glm::mat4>(both.data(), 1));
    const Frustum own = leftEye.frustum();
    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(single.planes[i].d == own.planes[i].d);
    }
    CHECK(Frustum::enclosing({}).intersects(Aabb{glm::vec3(0.0f, 0.0f, 50.0f), glm::vec3(1.0f, 1.0f, 51.0f)}));
}
