// The key light's shadow and the ambient occlusion, each judged by rendering a small test scene (tests/smoke/scenes)
// with and without the effect and comparing the pixels where known parts of the scene appear: world points projected
// through the scene camera, where the walker stays (no input moves it).

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

#include <glm/glm.hpp>

#include <rv/scene/scene_description.hpp>

#include "image_checks.hpp"
#include "smoke_support.hpp"

using namespace rv::smoke;

namespace {

constexpr glm::ivec2 kShotSize{960, 540};
/// Pixels around a probe: a 9 x 9 window.
constexpr int kProbeRadius = 4;

/// A point of the scene whose pixels the test compares.
struct Probe {
    const char* name;
    glm::vec3 world;
};

/// The scene's camera, through which the probes are projected.
rv::scene::CameraSetup sceneCamera(const std::filesystem::path& scene) {
    const auto description = rv::scene::loadSceneDescription(scene);
    INFO("scene " << arg(scene));
    REQUIRE(description.has_value());
    return description->camera;
}

glm::ivec2 probePixel(const rv::scene::CameraSetup& camera, const Probe& probe) {
    const auto pixel = projectToPixel(camera, kShotSize, probe.world);
    INFO("probe " << probe.name);
    REQUIRE(pixel.has_value());
    return *pixel;
}

/// Renders `scene` (3 frames) into <case>/<name>.png; checks the run and returns the screenshot.
rv::assets::ImageData renderShot(const std::filesystem::path& caseDir, const std::string& name,
                                 const std::filesystem::path& scene) {
    const std::filesystem::path shot = caseDir / (name + ".png");
    const AppRun run = runApp(retroverseExe(), caseDir, name,
                              {"--scene", arg(scene), "--smoke", "--frames", "3", "--screenshot", arg(shot),
                               "--screenshot-size", "960x540"});
    skipWithoutOpenGl(run);
    requireCleanRun(run);
    return requireScreenshot(run, shot, kShotSize);
}

/// Pixels whose luma differs by more than one 8-bit step between `a` and `b` outside the screen rectangle [min, max].
std::size_t changedOutside(const rv::assets::ImageData& a, const rv::assets::ImageData& b, glm::ivec2 min,
                           glm::ivec2 max) {
    std::size_t count = 0;
    for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
            const bool inside = x >= min.x && x <= max.x && y >= min.y && y <= max.y;
            count += !inside && std::abs(lumaAt(a, x, y) - lumaAt(b, x, y)) > 1.0 ? 1 : 0;
        }
    }
    return count;
}

} // namespace

TEST_CASE("effects/shadow: the key light's shadow darkens what the caster hides and leaves lit surfaces alone",
          "[gpu][effects]") {
    const std::filesystem::path dir = freshCaseDir("effects_shadow");
    const std::filesystem::path scene = sourceDir() / "tests/smoke/scenes/shadow_caster.json";
    const std::filesystem::path unshadowed = dir / "shadow_caster_unshadowed.json";
    writeSceneVariant(scene, unshadowed, [](nlohmann::json& json) { json["lights"][0]["castsShadows"] = false; });
    const rv::assets::ImageData shadowed = renderShot(dir, "shadowed", scene);
    const rv::assets::ImageData lit = renderShot(dir, "unshadowed", unshadowed);
    const rv::scene::CameraSetup camera = sceneCamera(scene);

    // The floor behind the box, seen from the light (at -X, above): in the shadow.
    const glm::ivec2 behind = probePixel(camera, {"behind the box", {0.7f, 0.0f, 0.0f}});
    const double shadowedLuma = windowLuma(shadowed, behind, kProbeRadius);
    const double litLuma = windowLuma(lit, behind, kProbeRadius);
    INFO("floor behind the box: luma " << shadowedLuma << " with the shadow, " << litLuma << " without");
    CHECK(litLuma > 40.0);
    CHECK(shadowedLuma < 0.5 * litLuma);

    // Lit surfaces the shadow does not reach keep their pixels exactly: the floor towards the light, beside the shadow
    // and the box's top. Shadow acne would darken them in speckles.
    for (const Probe& probe : {Probe{"floor towards the light", {-1.0f, 0.0f, 0.0f}},
                               Probe{"floor beside the shadow", {0.7f, 0.0f, 1.2f}},
                               Probe{"top of the box", {0.0f, 1.0f, 0.0f}}}) {
        INFO("probe " << probe.name);
        CHECK(windowMaxDifference(shadowed, lit, probePixel(camera, probe), kProbeRadius) <= 1);
    }
    // A shadow only takes light away, and only on the floor behind and beside the box (its base included).
    CHECK(brighterPixels(lit, shadowed, 1.0) == 0);
    glm::ivec2 low(kShotSize);
    glm::ivec2 high(0);
    for (const float x : {-0.4f, 1.2f}) {
        for (const float z : {-0.5f, 0.5f}) {
            const glm::ivec2 corner = probePixel(camera, {"shadow region corner", {x, 0.0f, z}});
            low = glm::min(low, corner);
            high = glm::max(high, corner);
        }
    }
    INFO("shadow region on screen (" << low.x << ", " << low.y << ") - (" << high.x << ", " << high.y << ")");
    CHECK(changedOutside(shadowed, lit, low, high) == 0);
}

TEST_CASE("effects/ambient occlusion: darkens the block's base and leaves the open floor alone", "[gpu][effects]") {
    const std::filesystem::path dir = freshCaseDir("effects_ambient_occlusion");
    const std::filesystem::path scene = sourceDir() / "tests/smoke/scenes/ambient_corner.json";
    const std::filesystem::path unoccluded = dir / "ambient_corner_unoccluded.json";
    writeSceneVariant(scene, unoccluded,
                      [](nlohmann::json& json) { json["environment"]["ssao"]["enabled"] = false; });
    const rv::assets::ImageData occluded = renderShot(dir, "occluded", scene);
    const rv::assets::ImageData open = renderShot(dir, "unoccluded", unoccluded);
    const rv::scene::CameraSetup camera = sceneCamera(scene);

    // Where the block meets the floor the ambient light is occluded: the floor in front of its base and the base of
    // its front face darken.
    for (const Probe& probe : {Probe{"floor at the block's base", {0.0f, 0.0f, 0.56f}},
                               Probe{"base of the block's face", {0.0f, 0.05f, 0.5f}}}) {
        const glm::ivec2 pixel = probePixel(camera, probe);
        const double withOcclusion = windowLuma(occluded, pixel, kProbeRadius);
        const double without = windowLuma(open, pixel, kProbeRadius);
        INFO("probe " << probe.name << ": luma " << withOcclusion << " with ambient occlusion, " << without
                      << " without");
        CHECK(withOcclusion < 0.95 * without);
    }

    // Away from the block (farther than the radius, also next to its silhouette on screen) the occlusion is 1: the
    // pixels stay exactly as they are.
    const Probe left{"open floor left", {-1.5f, 0.0f, 0.6f}};
    for (const Probe& probe : {left, Probe{"open floor right", {1.5f, 0.0f, 0.6f}},
                               Probe{"floor behind the block's silhouette", {0.0f, 0.0f, -4.3f}}}) {
        INFO("probe " << probe.name);
        CHECK(windowMaxDifference(occluded, open, probePixel(camera, probe), kProbeRadius) <= 1);
    }
    // Without ambient occlusion the floor at the block's base reads like the open floor at the same distance (the
    // occlusion target holds 1 everywhere), and with it nothing gets brighter.
    const double baseWithout = windowLuma(open, probePixel(camera, {"floor at the base", {0.0f, 0.0f, 0.56f}}), 1);
    const double openWithout = windowLuma(open, probePixel(camera, left), kProbeRadius);
    INFO("without ambient occlusion: floor at the base " << baseWithout << ", open floor " << openWithout);
    CHECK(std::abs(baseWithout - openWithout) <= 0.02 * openWithout);
    CHECK(brighterPixels(open, occluded, 1.0) == 0);
}
