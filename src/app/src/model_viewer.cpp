#include <rv/app/model_viewer.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

#include <fmt/format.h>
#include <glm/glm.hpp>

#include <rv/core/math.hpp>
#include <rv/scene/camera.hpp>

namespace rv::app {
namespace {

/// Smallest radius framed, so a degenerate (flat or point-like) model still gets a sensible camera.
constexpr float kMinRadius = 0.005f;

/// `bounds` when they can be framed, else a 1 m cube standing on the origin.
core::Aabb framedBounds(const core::Aabb& bounds) {
    const bool usable = !bounds.isEmpty() && core::isFinite(bounds.min) && core::isFinite(bounds.max);
    return usable ? bounds : core::Aabb::fromMinMax(glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 1.0f, 0.5f));
}

/// Unit vector from the model towards a point `azimuthDeg` to the right of its front (+Z) and `elevationDeg` above.
glm::vec3 directionFrom(float azimuthDeg, float elevationDeg) {
    const float azimuth = core::degreesToRadians(azimuthDeg);
    const float elevation = core::degreesToRadians(elevationDeg);
    return glm::vec3(std::sin(azimuth) * std::cos(elevation), std::sin(elevation),
                     std::cos(azimuth) * std::cos(elevation));
}

/// Linear RGB of an sRGB color given as 0-255 components.
glm::vec3 srgb(float red, float green, float blue) {
    return core::srgbToLinear(glm::vec3(red, green, blue) / 255.0f);
}

/// Point light `distance` away from `center` along `direction` that gives `irradiance` (intensity / distance^2, the
/// renderer's inverse-square falloff) at the center, with a range four times the distance (window ~1 there).
scene::Light pointLight(std::string name, const glm::vec3& center, const glm::vec3& direction, float distance,
                        const glm::vec3& color, float irradiance) {
    scene::Light light;
    light.name = std::move(name);
    light.type = scene::LightType::Point;
    light.position = center + direction * distance;
    light.color = color;
    light.intensity = irradiance * distance * distance;
    light.range = 4.0f * distance;
    return light;
}

} // namespace

scene::SceneDescription makeModelViewerScene(std::string modelPath, const core::Aabb& bounds) {
    const core::Aabb framed = framedBounds(bounds);
    const glm::vec3 center = framed.center();
    const float radius = std::max(0.5f * glm::length(framed.size()), kMinRadius);

    scene::SceneDescription scene;
    scene.name = std::string(kModelViewerSceneName);

    // Camera: the bounding sphere fits the narrower of the two fields of view.
    scene::CameraSetup& camera = scene.camera;
    const float horizontalFov = core::degreesToRadians(camera.lens.horizontalFovDeg);
    const float verticalFov = scene::Camera::verticalFovRadians(horizontalFov, kModelViewerAspect);
    const float distance = kModelViewerMargin * radius / std::sin(0.5f * std::min(horizontalFov, verticalFov));
    camera.position = center + directionFrom(kModelViewerAzimuthDeg, kModelViewerElevationDeg) * distance;
    camera.yawDeg = -kModelViewerAzimuthDeg; // looking back at the center: towards -X and -Z
    camera.pitchDeg = -kModelViewerElevationDeg;
    camera.lens.nearPlane = std::clamp(0.25f * (distance - radius), 0.001f, 0.05f);
    camera.lens.farPlane = std::max(100.0f, 4.0f * (distance + radius));

    // Neutral environment: the model's own colors read as authored. The background is a dark mid grey (radiance shown
    // as about sRGB 61 after exposure and tonemapping), clear of the faces of the material presets under the rig: black
    // plastic and graphite show at 8-40, grey plastic at 83-169, aged ivory at 118-207 (material_swatches.gltf of
    // scripts/make_test_models.py), so the silhouette of a dark console or CRT body reads as well as that of a light
    // one (style §2.1).
    scene.environment.clearColor = srgb(90.0f, 92.0f, 95.0f);
    scene.environment.ambientSky = srgb(255.0f, 255.0f, 255.0f);
    scene.environment.ambientGround = srgb(90.0f, 86.0f, 80.0f);
    scene.environment.ambientIntensity = 0.35f;
    scene.environment.exposure = 1.0f;
    // No bloom and no vignette: every part of the model, emissive or glossy, in the middle or in a corner of the view,
    // shows its own colors (the overlay's Rendering panel can still turn them on).
    scene.environment.bloomStrength = 0.0f;
    scene.environment.vignette = 0.0f;

    // Three-point rig: key (directional, warm), fill (cool, dimmer) and rim (from behind), around the bounds.
    scene::Light key;
    key.name = "viewer_key";
    key.type = scene::LightType::Directional;
    key.direction = -directionFrom(-40.0f, 45.0f); // travels from the front left above towards the model
    key.color = srgb(255.0f, 241.0f, 224.0f);
    key.intensity = 3.0f;
    scene.lights.push_back(key);
    const float lightDistance = 1.5f * distance;
    scene.lights.push_back(pointLight("viewer_fill", center, directionFrom(60.0f, 15.0f), lightDistance,
                                      srgb(214.0f, 228.0f, 255.0f), 0.8f));
    scene.lights.push_back(pointLight("viewer_rim", center, directionFrom(160.0f, 50.0f), lightDistance,
                                      srgb(255.0f, 255.0f, 255.0f), 2.0f));

    scene::SceneObject& model = scene.objects.emplace_back();
    model.id = std::string(kModelViewerObjectId);
    model.source = scene::SceneObject::Source::Model;
    model.modelPath = std::move(modelPath);
    // The walker's capsule would push the camera off a model smaller than its radius; inspecting means getting close.
    model.collides = false;
    return scene;
}

DebugGrid modelViewerGrid(const core::Aabb& bounds) noexcept {
    const core::Aabb measured = framedBounds(bounds);
    const glm::vec3 size = measured.size();
    const float largest = std::max({size.x, size.y, size.z, kMinRadius});
    const float spacing = std::pow(10.0f, std::floor(std::log10(0.5f * largest)));
    const float reach = std::max(
        {std::abs(measured.min.x), std::abs(measured.max.x), std::abs(measured.min.z), std::abs(measured.max.z)});
    return DebugGrid{std::max(20.0f * spacing, std::ceil(2.0f * reach / spacing) * spacing), spacing};
}

std::string describeModel(std::string_view path, const assets::ModelData& model) {
    std::string summary = fmt::format(
        "Model {}: {} node(s), {} mesh(es) with {} primitive(s), {} triangle(s), {} material(s), {} image(s); ", path,
        model.stats.nodes, model.stats.meshes, model.stats.primitives, model.stats.triangles, model.stats.materials,
        model.stats.images);
    // An empty box holds the +/-FLT_MAX sentinels: nothing to print.
    if (model.bounds.isEmpty()) {
        return summary + "no bounds (nothing to draw)";
    }
    const glm::vec3 size = model.bounds.size();
    const glm::vec3& min = model.bounds.min;
    const glm::vec3& max = model.bounds.max;
    return summary + fmt::format("bounds {:.3f} x {:.3f} x {:.3f} m from ({:.3f}, {:.3f}, {:.3f}) to ({:.3f}, {:.3f}, "
                                 "{:.3f})",
                                 size.x, size.y, size.z, min.x, min.y, min.z, max.x, max.y, max.z);
}

} // namespace rv::app
