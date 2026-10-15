#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

#include <glm/glm.hpp>

#include <rv/core/aabb.hpp>
#include <rv/core/math.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/orbit_controller.hpp>
#include <rv/scene/viewpoint.hpp>

using Catch::Approx;
using rv::core::Aabb;
using rv::core::degreesToRadians;
using rv::scene::Camera;
using rv::scene::CameraLens;
using rv::scene::OrbitController;
using rv::scene::OrbitInput;
using rv::scene::ViewArea;

namespace {

constexpr float kAspect = 16.0f / 9.0f;

/// A CRT standing on the origin.
const Aabb kCrt = Aabb::fromMinMax(glm::vec3(-0.355f, 0.0f, -0.28f), glm::vec3(0.355f, 0.61f, 0.28f));

Camera viewCamera() {
    Camera camera;
    camera.setAspect(kAspect);
    return camera;
}

/// Where `point` appears in `camera`'s view, in normalized device coordinates.
glm::vec2 ndcOf(const Camera& camera, const glm::vec3& point) {
    const glm::vec4 clip = camera.viewProjection() * glm::vec4(point, 1.0f);
    return glm::vec2(clip) / clip.w;
}

/// The 8 corners of `box`.
std::array<glm::vec3, 8> cornersOf(const Aabb& box) {
    std::array<glm::vec3, 8> corners{};
    for (int corner = 0; corner < 8; ++corner) {
        corners[static_cast<std::size_t>(corner)] =
            glm::vec3((corner & 1) != 0 ? box.max.x : box.min.x, (corner & 2) != 0 ? box.max.y : box.min.y,
                      (corner & 4) != 0 ? box.max.z : box.min.z);
    }
    return corners;
}

} // namespace

TEST_CASE("scene/orbit: framing shows the bounds whole, centered, from the angles asked for", "[scene][orbit]") {
    OrbitController orbit;
    orbit.frame(kCrt, -35.0f, -25.0f, CameraLens{}, kAspect);
    CHECK(rv::core::nearlyEqual(orbit.target(), kCrt.center()));
    CHECK(orbit.yaw() == Approx(degreesToRadians(-35.0f)));
    CHECK(orbit.pitch() == Approx(degreesToRadians(-25.0f)));
    // From the front right and above: the eye is at +X, +Y and +Z of the target.
    const glm::vec3 offset = orbit.eye() - orbit.target();
    CHECK(offset.x > 0.0f);
    CHECK(offset.y > 0.0f);
    CHECK(offset.z > 0.0f);
    CHECK(glm::length(offset) == Approx(orbit.distance()));

    Camera camera = viewCamera();
    orbit.apply(camera);
    const glm::vec2 center = ndcOf(camera, kCrt.center());
    CHECK(center.x == Approx(0.0f).margin(1e-4));
    CHECK(center.y == Approx(0.0f).margin(1e-4));
    for (const glm::vec3& corner : cornersOf(kCrt)) {
        const glm::vec2 ndc = ndcOf(camera, corner);
        CHECK(std::abs(ndc.x) < 1.0f);
        CHECK(std::abs(ndc.y) < 1.0f);
    }
    // The clip planes follow the distance: the bounds lie between them.
    CHECK(camera.lens().nearPlane < orbit.distance() - 0.5f * glm::length(kCrt.size()));
    CHECK(camera.lens().farPlane > orbit.distance() + 0.5f * glm::length(kCrt.size()));
}

TEST_CASE("scene/orbit: framing in a part of the view centers the target there", "[scene][orbit]") {
    // The see-through center of the overlay's default layout in a 1280 x 720 window.
    const ViewArea visible{glm::vec2(-0.54f, -0.52f), glm::vec2(0.40f, 1.0f)};
    OrbitController orbit;
    orbit.frame(kCrt, -35.0f, -25.0f, CameraLens{}, kAspect, visible);
    Camera camera = viewCamera();
    orbit.apply(camera, visible);
    const glm::vec2 center = ndcOf(camera, kCrt.center());
    CHECK(center.x == Approx(-0.07f).margin(1e-3));
    CHECK(center.y == Approx(0.24f).margin(1e-3));
    for (const glm::vec3& corner : cornersOf(kCrt)) {
        const glm::vec2 ndc = ndcOf(camera, corner);
        CHECK(ndc.x > visible.min.x);
        CHECK(ndc.x < visible.max.x);
        CHECK(ndc.y > visible.min.y);
        CHECK(ndc.y < visible.max.y);
    }
    // A smaller area needs more distance than the whole view.
    OrbitController whole;
    whole.frame(kCrt, -35.0f, -25.0f, CameraLens{}, kAspect);
    CHECK(orbit.distance() > whole.distance());
}

TEST_CASE("scene/orbit: tiny parts and large models both fit, empty bounds fall back to a 1 m cube", "[scene][orbit]") {
    for (const float size : {0.01f, 20.0f}) {
        INFO(size);
        const Aabb box =
            Aabb::fromMinMax(glm::vec3(-0.5f * size, 0.0f, -0.5f * size), glm::vec3(0.5f * size, size, 0.5f * size));
        OrbitController orbit;
        orbit.frame(box, 0.0f, -20.0f, CameraLens{}, kAspect);
        Camera camera = viewCamera();
        orbit.apply(camera);
        CHECK(camera.lens().nearPlane < orbit.distance() - 0.5f * glm::length(box.size()));
        for (const glm::vec3& corner : cornersOf(box)) {
            const glm::vec2 ndc = ndcOf(camera, corner);
            CHECK(std::abs(ndc.x) < 1.0f);
            CHECK(std::abs(ndc.y) < 1.0f);
        }
    }
    OrbitController fallback;
    fallback.frame(Aabb{}, 0.0f, 0.0f, CameraLens{}, kAspect);
    CHECK(rv::core::nearlyEqual(fallback.target(), glm::vec3(0.0f, 0.5f, 0.0f)));
}

TEST_CASE("scene/orbit: drags orbit and pan, the wheel zooms, the pitch stays within its limit", "[scene][orbit]") {
    OrbitController orbit;
    orbit.frame(kCrt, 0.0f, 0.0f, CameraLens{}, kAspect);
    const Camera view = viewCamera();
    const float distance = orbit.distance();

    // A drag to the right turns the eye to the left around the target: the model turns to the right.
    OrbitInput drag;
    drag.orbitPixels = glm::vec2(100.0f, 0.0f);
    orbit.update(drag, 0.016f, view, 720.0f);
    CHECK(orbit.yaw() == Approx(degreesToRadians(30.0f)));
    CHECK((orbit.eye() - orbit.target()).x < 0.0f);
    // A drag down raises the eye: the camera looks down on the model.
    drag.orbitPixels = glm::vec2(0.0f, 50.0f);
    orbit.update(drag, 0.016f, view, 720.0f);
    CHECK(orbit.pitch() == Approx(degreesToRadians(-15.0f)));
    drag.orbitPixels = glm::vec2(0.0f, 10000.0f);
    orbit.update(drag, 0.016f, view, 720.0f);
    CHECK(orbit.pitch() == Approx(degreesToRadians(-89.0f)));

    // Wheel: closer, never closer than the limit nor farther than the other.
    OrbitInput wheel;
    wheel.zoomNotches = 1.0f;
    orbit.update(wheel, 0.016f, view, 720.0f);
    CHECK(orbit.distance() == Approx(distance / 1.15f));
    wheel.zoomNotches = 1000.0f;
    orbit.update(wheel, 0.016f, view, 720.0f);
    CHECK(orbit.distance() == Approx(orbit.minDistance()));
    wheel.zoomNotches = -1000.0f;
    orbit.update(wheel, 0.016f, view, 720.0f);
    CHECK(orbit.distance() == Approx(orbit.maxDistance()));

    // A pan drag moves the target across the view, keeping its distance: a drag of the view's height moves it by the
    // height of the view at the target.
    OrbitController level;
    level.frame(kCrt, 0.0f, 0.0f, CameraLens{}, kAspect);
    const glm::vec3 start = level.target();
    OrbitInput pan;
    pan.panPixels = glm::vec2(0.0f, 720.0f);
    level.update(pan, 0.016f, view, 720.0f);
    const float viewHeight = 2.0f * level.distance() * std::tan(0.5f * view.verticalFovRadians());
    CHECK(level.target().y - start.y == Approx(viewHeight));
    pan.panPixels = glm::vec2(360.0f, 0.0f);
    level.update(pan, 0.016f, view, 720.0f);
    CHECK(level.target().x - start.x == Approx(-0.5f * viewHeight)); // the model follows the pointer to the right
    CHECK(level.target().z == Approx(start.z).margin(1e-6));
}

TEST_CASE("scene/orbit: sticks, triggers and the auto-rotation act per second", "[scene][orbit]") {
    OrbitController orbit;
    orbit.frame(kCrt, 0.0f, -10.0f, CameraLens{}, kAspect);
    const Camera view = viewCamera();
    const float distance = orbit.distance();
    OrbitInput stick;
    stick.orbitRate = glm::vec2(1.0f, 0.5f);
    stick.zoomRate = 1.0f;
    orbit.update(stick, 0.5f, view, 720.0f);
    CHECK(orbit.yaw() == Approx(degreesToRadians(60.0f)));
    CHECK(orbit.pitch() == Approx(degreesToRadians(-40.0f))); // pushed up: the eye rises
    CHECK(orbit.distance() == Approx(distance / std::sqrt(3.0f)));

    OrbitController turning;
    turning.frame(kCrt, 0.0f, 0.0f, CameraLens{}, kAspect);
    turning.setAutoRotate(true);
    for (int frame = 0; frame < 10; ++frame) {
        turning.update(OrbitInput{}, 0.1f, view, 720.0f);
    }
    CHECK(turning.yaw() == Approx(degreesToRadians(20.0f)));
    turning.setAutoRotate(false);
    turning.update(OrbitInput{}, 1.0f, view, 720.0f);
    CHECK(turning.yaw() == Approx(degreesToRadians(20.0f)));
}

TEST_CASE("scene/orbit: non-finite input changes nothing", "[scene][orbit]") {
    OrbitController orbit;
    orbit.frame(kCrt, 10.0f, -10.0f, CameraLens{}, kAspect);
    const Camera view = viewCamera();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    OrbitInput broken;
    broken.orbitPixels = glm::vec2(nan, 1.0f);
    broken.panPixels = glm::vec2(1.0f, nan);
    broken.zoomNotches = nan;
    broken.orbitRate = glm::vec2(nan);
    broken.panRate = glm::vec2(nan);
    broken.zoomRate = std::numeric_limits<float>::infinity();
    const glm::vec3 target = orbit.target();
    const float yaw = orbit.yaw();
    const float pitch = orbit.pitch();
    const float distance = orbit.distance();
    orbit.update(broken, nan, view, 720.0f);
    CHECK(orbit.target() == target);
    CHECK(orbit.yaw() == yaw);
    CHECK(orbit.pitch() == pitch);
    CHECK(orbit.distance() == distance);
}

TEST_CASE("scene/viewpoint: aiming puts a target at the center of a view area", "[scene][viewpoint]") {
    const glm::vec3 eye(1.0f, 1.65f, 2.0f);
    const glm::vec3 target(-0.5f, 0.8f, -1.0f);
    const ViewArea area{glm::vec2(-0.2f, -0.6f), glm::vec2(0.9f, 0.8f)};
    for (const ViewArea& frame : {ViewArea{}, area}) {
        const rv::scene::ViewAngles angles = rv::scene::aimAt(eye, target, CameraLens{}, kAspect, frame);
        Camera camera = viewCamera();
        camera.setPose(eye, angles.yaw, angles.pitch);
        const glm::vec2 ndc = ndcOf(camera, target);
        const glm::vec2 expected = 0.5f * (frame.min + frame.max);
        CHECK(ndc.x == Approx(expected.x).margin(1e-4));
        CHECK(ndc.y == Approx(expected.y).margin(1e-4));
    }
    // Straight above: the fallback yaw, looking down.
    const rv::scene::ViewAngles down =
        rv::scene::aimAt(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f), CameraLens{}, kAspect, {}, 0.25f);
    CHECK(down.yaw == Approx(0.25f));
    CHECK(down.pitch == Approx(-rv::core::kHalfPi));
}
