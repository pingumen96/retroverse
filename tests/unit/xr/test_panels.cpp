// The VR mode's UI panel (I10 step 5, ADR-0017 §6): the world panel in front of the head, the heads-up display locked
// to it, the ray's pixel on a panel, and the heads-up display's field mapping a direction to the pixel the panel shows
// it at.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <rv/core/math.hpp>
#include <rv/scene/camera.hpp>
#include <rv/xr/panels.hpp>
#include <rv/xr/pose.hpp>

using Catch::Approx;
using rv::core::degreesToRadians;
using rv::xr::Pose;

namespace {

Pose headAt(const glm::vec3& position, float yawDeg, float pitchDeg = 0.0f, float rollDeg = 0.0f) {
    return Pose{rv::xr::orientationOf(rv::xr::Angles{degreesToRadians(yawDeg), degreesToRadians(pitchDeg),
                                                     degreesToRadians(rollDeg)}),
                position};
}

constexpr glm::vec2 kCanvas(1280.0f, 720.0f);
constexpr float kAspect = 1280.0f / 720.0f;

} // namespace

TEST_CASE("xr/panels: the world panel stands upright in front of the head's heading, facing it", "[xr][panels]") {
    // Looking 90 degrees right and 40 degrees down: the panel ignores the pitch.
    const Pose head = headAt(glm::vec3(1.0f, 1.6f, 2.0f), 90.0f, -40.0f);
    const rv::xr::PanelPose panel = rv::xr::worldPanel(head, kAspect);
    CHECK(panel.center.x == Approx(1.0f + rv::xr::kWorldPanelDistance).margin(1e-5));
    CHECK(panel.center.y == Approx(1.6f - rv::xr::kWorldPanelDrop).margin(1e-5));
    CHECK(panel.center.z == Approx(2.0f).margin(1e-5));
    CHECK(panel.size.x == Approx(rv::xr::kWorldPanelWidth));
    CHECK(panel.size.y == Approx(rv::xr::kWorldPanelWidth / kAspect));
    const glm::mat3 axes = glm::mat3_cast(panel.orientation);
    CHECK(axes[2].x == Approx(-1.0f).margin(1e-5)); // its front towards the head
    CHECK(axes[1].y == Approx(1.0f).margin(1e-5));  // upright

    // The unit square's corners on the panel: (0, 0) bottom-left, (1, 1) top-right seen from the head.
    const glm::mat4 model = panel.model();
    const glm::vec3 bottomLeft(model * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    const glm::vec3 topRight(model * glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
    CHECK(bottomLeft.z == Approx(2.0f - 0.5f * panel.size.x).margin(1e-5)); // the head's left is -Z here
    CHECK(topRight.z == Approx(2.0f + 0.5f * panel.size.x).margin(1e-5));
    CHECK(topRight.y - bottomLeft.y == Approx(panel.size.y).margin(1e-5));
}

TEST_CASE("xr/panels: the ray's pixel on a panel, from the front only", "[xr][panels]") {
    const Pose head = headAt(glm::vec3(0.0f, 1.6f, 0.0f), 0.0f);
    const rv::xr::PanelPose panel = rv::xr::worldPanel(head, kAspect);
    // Straight at the centre: the canvas's middle.
    const rv::core::Ray centre{glm::vec3(0.0f, panel.center.y, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)};
    const auto hit = rv::xr::panelHit(panel, centre, kCanvas);
    REQUIRE(hit);
    CHECK(hit->pixel.x == Approx(640.0f).margin(1e-2));
    CHECK(hit->pixel.y == Approx(360.0f).margin(1e-2));
    CHECK(hit->distance == Approx(rv::xr::kWorldPanelDistance).margin(1e-4));
    // At the top-left corner's area: small x and y (y counts down from the top).
    const glm::vec3 nearTopLeft = panel.center + glm::vec3(-0.45f * panel.size.x, 0.45f * panel.size.y, 0.0f);
    const auto corner =
        rv::xr::panelHit(panel, rv::core::Ray{glm::vec3(0.0f, 1.6f, 0.0f), glm::normalize(nearTopLeft - head.position)},
                         kCanvas);
    REQUIRE(corner);
    CHECK(corner->pixel.x == Approx(0.05f * 1280.0f).margin(0.5));
    CHECK(corner->pixel.y == Approx(0.05f * 720.0f).margin(0.5));
    // Beside it, from behind it, or away from it: nothing.
    CHECK_FALSE(rv::xr::panelHit(panel, rv::core::Ray{glm::vec3(3.0f, 1.5f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)},
                                 kCanvas));
    CHECK_FALSE(rv::xr::panelHit(panel, rv::core::Ray{glm::vec3(0.0f, 1.5f, -3.0f), glm::vec3(0.0f, 0.0f, 1.0f)},
                                 kCanvas));
    CHECK_FALSE(rv::xr::panelHit(panel, rv::core::Ray{glm::vec3(0.0f, 1.5f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)},
                                 kCanvas));
}

TEST_CASE("xr/panels: the heads-up display shows a direction where its field projects it", "[xr][panels]") {
    // A head turned and tilted: what its HUD camera projects to a pixel, the HUD panel shows on that pixel's spot.
    const Pose head = headAt(glm::vec3(0.5f, 1.7f, -1.0f), 25.0f, -10.0f, 5.0f);
    const rv::xr::PanelPose panel = rv::xr::hudPanel(head, kAspect);
    rv::scene::Camera camera;
    camera.setEye(head.position, head.orientation, rv::xr::hudField(kAspect));
    const glm::mat4 viewProjection = camera.viewProjection();
    for (const glm::vec3 local : {glm::vec3(0.3f, 0.1f, -2.0f), glm::vec3(-0.6f, -0.2f, -3.0f)}) {
        const glm::vec3 point = head.position + head.orientation * local;
        const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        const glm::vec2 pixel((ndc.x + 1.0f) * 0.5f * kCanvas.x, (1.0f - ndc.y) * 0.5f * kCanvas.y);
        const auto hit = rv::xr::panelHit(
            panel, rv::core::Ray{head.position, glm::normalize(point - head.position)}, kCanvas);
        REQUIRE(hit);
        CHECK(hit->pixel.x == Approx(pixel.x).margin(0.05));
        CHECK(hit->pixel.y == Approx(pixel.y).margin(0.05));
        CHECK(hit->distance > rv::xr::kHudDistance);
    }
    // Its field holds the canvas's aspect.
    const rv::scene::EyeFov field = rv::xr::hudField(kAspect);
    CHECK(std::tan(field.right) / std::tan(field.up) == Approx(kAspect).margin(1e-4));
    CHECK(rv::core::radiansToDegrees(field.right - field.left) == Approx(rv::xr::kHudFieldDeg).margin(1e-3));
}
