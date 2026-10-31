#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec4.hpp>

#include <spike_xr/xr_math.hpp>

using Catch::Matchers::WithinAbs;
using rv::spikes::xr::EyeFov;
using rv::spikes::xr::EyePose;
using rv::spikes::xr::projectionFromFov;
using rv::spikes::xr::viewFromPose;

namespace {

/// The NDC position of a view-space point.
glm::vec3 ndcOf(const glm::mat4& projection, const glm::vec3& point) {
    const glm::vec4 clip = projection * glm::vec4(point, 1.0f);
    return glm::vec3(clip) / clip.w;
}

} // namespace

TEST_CASE("xr_spike/math: a symmetric field projects like glm::perspectiveRH_NO", "[xr_spike][math]") {
    const float half = glm::radians(45.0f);
    const EyeFov fov{-half, half, half, -half};
    const glm::mat4 ours = projectionFromFov(fov, 0.05f, 100.0f);
    const glm::mat4 glms = glm::perspectiveRH_NO(2.0f * half, 1.0f, 0.05f, 100.0f);
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            CHECK_THAT(ours[column][row], WithinAbs(glms[column][row], 1e-5));
        }
    }
}

TEST_CASE("xr_spike/math: the edges of an asymmetric field map to the edges of NDC", "[xr_spike][math]") {
    // A left eye of a headset: the inner (right) edge narrower than the outer one, the lower edge wider.
    const EyeFov fov{glm::radians(-52.0f), glm::radians(42.0f), glm::radians(44.0f), glm::radians(-50.0f)};
    const float nearPlane = 0.05f;
    const float farPlane = 100.0f;
    const glm::mat4 projection = projectionFromFov(fov, nearPlane, farPlane);
    const float depth = 2.0f;

    CHECK_THAT(ndcOf(projection, {std::tan(fov.left) * depth, 0.0f, -depth}).x, WithinAbs(-1.0, 1e-5));
    CHECK_THAT(ndcOf(projection, {std::tan(fov.right) * depth, 0.0f, -depth}).x, WithinAbs(1.0, 1e-5));
    CHECK_THAT(ndcOf(projection, {0.0f, std::tan(fov.up) * depth, -depth}).y, WithinAbs(1.0, 1e-5));
    CHECK_THAT(ndcOf(projection, {0.0f, std::tan(fov.down) * depth, -depth}).y, WithinAbs(-1.0, 1e-5));
    // The view direction is off the centre, towards the narrower edges.
    CHECK(ndcOf(projection, {0.0f, 0.0f, -depth}).x > 0.0f);
    CHECK(ndcOf(projection, {0.0f, 0.0f, -depth}).y > 0.0f);
    // Depth in [-1, 1] from the near to the far plane.
    CHECK_THAT(ndcOf(projection, {0.0f, 0.0f, -nearPlane}).z, WithinAbs(-1.0, 1e-4));
    CHECK_THAT(ndcOf(projection, {0.0f, 0.0f, -farPlane}).z, WithinAbs(1.0, 1e-4));
}

TEST_CASE("xr_spike/math: the view of a pose brings its position to the origin, looking along -Z",
          "[xr_spike][math]") {
    SECTION("position only") {
        const EyePose pose{glm::quat(1.0f, 0.0f, 0.0f, 0.0f), {0.5f, 1.6f, 2.0f}};
        const glm::vec4 ahead = viewFromPose(pose) * glm::vec4(0.5f, 1.6f, 1.0f, 1.0f);
        CHECK_THAT(ahead.x, WithinAbs(0.0, 1e-5));
        CHECK_THAT(ahead.y, WithinAbs(0.0, 1e-5));
        CHECK_THAT(ahead.z, WithinAbs(-1.0, 1e-5));
    }
    SECTION("turned left by 90 degrees about +Y") {
        const EyePose pose{glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)), {0.0f, 1.6f, 0.0f}};
        // Turned left (counter-clockwise seen from above), the eye looks along -X.
        const glm::vec4 ahead = viewFromPose(pose) * glm::vec4(-3.0f, 1.6f, 0.0f, 1.0f);
        CHECK_THAT(ahead.x, WithinAbs(0.0, 1e-5));
        CHECK_THAT(ahead.y, WithinAbs(0.0, 1e-5));
        CHECK_THAT(ahead.z, WithinAbs(-3.0, 1e-5));
    }
}
