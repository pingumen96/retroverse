#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <initializer_list>
#include <limits>

#include <glm/glm.hpp>

#include <rv/app/model_viewer.hpp>
#include <rv/assets/model_data.hpp>
#include <rv/core/math.hpp>
#include <rv/render/light_packing.hpp>
#include <rv/render/tonemap.hpp>
#include <rv/scene/camera.hpp>

using Catch::Approx;
using rv::core::Aabb;
using rv::scene::SceneDescription;

namespace {

/// The viewer camera as the renderer builds it for a 16:9 view.
rv::scene::Camera viewerCamera(const SceneDescription& scene) {
    rv::scene::Camera camera;
    camera.setLens(scene.camera.lens);
    camera.setAspect(rv::app::kModelViewerAspect);
    camera.setPose(scene.camera.position, rv::core::degreesToRadians(scene.camera.yawDeg),
                   rv::core::degreesToRadians(scene.camera.pitchDeg));
    return camera;
}

/// True when the view contains every corner of `box`.
bool framesBox(const rv::scene::Camera& camera, const Aabb& box) {
    const rv::scene::Frustum frustum = camera.frustum();
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) != 0 ? box.max.x : box.min.x, (i & 2) != 0 ? box.max.y : box.min.y,
                               (i & 4) != 0 ? box.max.z : box.min.z);
        if (!frustum.contains(corner)) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("app/model_viewer: the camera frames the model from the front right, above", "[app][model_viewer]") {
    const Aabb bounds = Aabb::fromMinMax(glm::vec3(-0.25f, 0.0f, -0.25f), glm::vec3(0.25f, 0.5f, 0.25f));
    const SceneDescription scene = rv::app::makeModelViewerScene("build/models/cube.gltf", bounds);
    CHECK(scene.name == rv::app::kModelViewerSceneName);

    const rv::scene::Camera camera = viewerCamera(scene);
    CHECK(framesBox(camera, bounds));
    // It looks at the center of the bounds...
    const glm::vec3 toCenter = glm::normalize(bounds.center() - camera.position());
    CHECK(glm::dot(toCenter, camera.forward()) == Approx(1.0f).margin(1e-5));
    // ...from the front (+Z), to the right (+X) and above.
    const glm::vec3 offset = camera.position() - bounds.center();
    CHECK(offset.z > 0.0f);
    CHECK(offset.x > 0.0f);
    CHECK(offset.y > 0.0f);
    CHECK(std::atan2(offset.x, offset.z) == Approx(rv::core::degreesToRadians(rv::app::kModelViewerAzimuthDeg)));
    CHECK(scene.camera.pitchDeg == Approx(-rv::app::kModelViewerElevationDeg));
    // Not much farther than needed: the bounding sphere fills most of the view.
    const float radius = 0.5f * glm::length(bounds.size());
    CHECK(glm::length(offset) < 1.5f * radius / std::sin(0.5f * camera.verticalFovRadians()));

    // The model object: the file as given, at the origin, unrotated.
    REQUIRE(scene.objects.size() == 1);
    const rv::scene::SceneObject& model = scene.objects.front();
    CHECK(model.id == rv::app::kModelViewerObjectId);
    CHECK(model.source == rv::scene::SceneObject::Source::Model);
    CHECK(model.modelPath == "build/models/cube.gltf");
    CHECK(model.transform == glm::mat4(1.0f));
    CHECK_FALSE(model.collides); // the camera may come closer than the walker's capsule radius
}

TEST_CASE("app/model_viewer: millimeter and 20 m models both fit between the clip planes", "[app][model_viewer]") {
    for (const float size : {0.005f, 0.2f, 20.0f}) {
        INFO("model size " << size);
        const Aabb bounds =
            Aabb::fromMinMax(glm::vec3(-0.5f * size, 0.0f, -0.5f * size), glm::vec3(0.5f * size, size, 0.5f * size));
        const SceneDescription scene = rv::app::makeModelViewerScene("m.glb", bounds);
        CHECK(scene.camera.lens.nearPlane > 0.0f);
        CHECK(scene.camera.lens.nearPlane < scene.camera.lens.farPlane);
        CHECK(framesBox(viewerCamera(scene), bounds));
    }
}

TEST_CASE("app/model_viewer: a three-point rig lights the model whatever its size", "[app][model_viewer]") {
    for (const float size : {0.1f, 1.0f, 10.0f}) {
        INFO("model size " << size);
        const Aabb bounds =
            Aabb::fromMinMax(glm::vec3(-0.5f * size, 0.0f, -0.5f * size), glm::vec3(0.5f * size, size, 0.5f * size));
        const SceneDescription scene = rv::app::makeModelViewerScene("m.glb", bounds);
        REQUIRE(scene.lights.size() == 3);
        const rv::scene::Light& key = scene.lights[0];
        CHECK(key.type == rv::scene::LightType::Directional);
        // From the front left above: it travels down, towards the back and to the right.
        CHECK(key.direction.y < 0.0f);
        CHECK(key.direction.z < 0.0f);
        CHECK(key.direction.x > 0.0f);
        CHECK(glm::length(key.direction) == Approx(1.0f));

        const rv::scene::Light& fill = scene.lights[1];
        const rv::scene::Light& rim = scene.lights[2];
        CHECK(fill.type == rv::scene::LightType::Point);
        CHECK(rim.type == rv::scene::LightType::Point);
        CHECK(fill.position.z > bounds.center().z); // in front
        CHECK(fill.position.x > bounds.center().x); // on the right
        CHECK(rim.position.z < bounds.center().z);  // behind
        CHECK(rim.position.y > bounds.max.y);       // above
        // Inverse-square falloff: the irradiance at the model is the same for every size, well inside the range.
        for (const rv::scene::Light* light : {&fill, &rim}) {
            const float distance = glm::length(light->position - bounds.center());
            CHECK(light->range > 2.0f * distance);
            CHECK(light->intensity / (distance * distance) == Approx(light == &fill ? 0.8f : 2.0f));
        }
        // Every light reaches the lights block.
        CHECK(rv::gl::packLights(scene.lights, scene.environment).dropped.empty());
    }
}

TEST_CASE("app/model_viewer: bounds that cannot be framed are treated as a 1 m cube", "[app][model_viewer]") {
    const Aabb cube = Aabb::fromMinMax(glm::vec3(-0.5f, 0.0f, -0.5f), glm::vec3(0.5f, 1.0f, 0.5f));
    const SceneDescription reference = rv::app::makeModelViewerScene("m.glb", cube);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const Aabb& bounds : {Aabb{}, Aabb::fromMinMax(glm::vec3(nan), glm::vec3(1.0f))}) {
        const SceneDescription scene = rv::app::makeModelViewerScene("m.glb", bounds);
        CHECK(rv::core::nearlyEqual(scene.camera.position, reference.camera.position));
        CHECK(rv::core::isFinite(scene.camera.position));
    }
}

TEST_CASE("app/model_viewer: the background sets off dark and light models alike", "[app][model_viewer]") {
    const SceneDescription scene = rv::app::makeModelViewerScene("m.glb", Aabb{});
    // The clear color is radiance: exposed and tonemapped like the surfaces, then encoded to sRGB.
    const glm::vec3 shown =
        rv::core::linearToSrgb(rv::gl::tonemap(scene.environment.clearColor, scene.environment.exposure)) * 255.0f;
    INFO("background shown as " << shown.r << ", " << shown.g << ", " << shown.b);
    // A neutral dark mid grey, about 61: under the rig the faces of black plastic (#1C1C1E) and graphite (#252A2E)
    // show at 8-40, those of grey plastic (#9A9A96) at 83-169 (scripts/make_test_models.py, material_swatches.gltf),
    // so dark silhouettes read as well as light ones.
    for (int channel = 0; channel < 3; ++channel) {
        CHECK(shown[channel] >= 55.0f);
        CHECK(shown[channel] <= 68.0f);
    }
    CHECK(std::abs(shown.r - shown.b) < 8.0f);
}

TEST_CASE("app/model_viewer: the model summary names its size, or that it has none", "[app][model_viewer]") {
    rv::assets::ModelData model;
    model.stats.nodes = 3;
    model.stats.meshes = 2;
    model.stats.primitives = 2;
    model.stats.triangles = 24;
    model.stats.materials = 1;
    model.bounds = Aabb::fromMinMax(glm::vec3(-0.3f, 0.0f, -0.25f), glm::vec3(0.3f, 0.4f, 0.25f));
    CHECK(rv::app::describeModel("build/models/box.gltf", model) ==
          "Model build/models/box.gltf: 3 node(s), 2 mesh(es) with 2 primitive(s), 24 triangle(s), 1 material(s), 0 "
          "image(s); bounds 0.600 x 0.400 x 0.500 m from (-0.300, 0.000, -0.250) to (0.300, 0.400, 0.250)");

    // Only LINES or POINTS primitives: nothing is drawn, and the empty box's sentinels are not printed.
    rv::assets::ModelData lines;
    lines.stats.nodes = 1;
    lines.stats.meshes = 1;
    CHECK(rv::app::describeModel("lines.gltf", lines) ==
          "Model lines.gltf: 1 node(s), 1 mesh(es) with 0 primitive(s), 0 triangle(s), 0 material(s), 0 image(s); no "
          "bounds (nothing to draw)");
}

TEST_CASE("app/model_viewer: the debug grid follows the size of the model", "[app][model_viewer]") {
    // A 0.5 m cube on the origin: 10 cm cells, 20 of them each way.
    const rv::app::DebugGrid cube =
        rv::app::modelViewerGrid(Aabb::fromMinMax(glm::vec3(-0.25f, 0.0f, -0.25f), glm::vec3(0.25f, 0.5f, 0.25f)));
    CHECK(cube.spacing == Approx(0.1f));
    CHECK(cube.halfExtent == Approx(2.0f));
    // A 1 cm part: 1 mm cells.
    const rv::app::DebugGrid part =
        rv::app::modelViewerGrid(Aabb::fromMinMax(glm::vec3(-0.005f, 0.0f, -0.005f), glm::vec3(0.005f, 0.01f, 0.005f)));
    CHECK(part.spacing == Approx(0.001f));
    CHECK(part.halfExtent == Approx(0.02f));
    // A 12 m model: 1 m cells; a model away from the origin: out to twice its horizontal distance from it.
    const rv::app::DebugGrid hall =
        rv::app::modelViewerGrid(Aabb::fromMinMax(glm::vec3(-6.0f, 0.0f, -3.0f), glm::vec3(6.0f, 4.0f, 3.0f)));
    CHECK(hall.spacing == Approx(1.0f));
    CHECK(hall.halfExtent == Approx(20.0f));
    const rv::app::DebugGrid offset =
        rv::app::modelViewerGrid(Aabb::fromMinMax(glm::vec3(20.0f, 0.0f, 0.0f), glm::vec3(24.0f, 4.0f, 4.0f)));
    CHECK(offset.spacing == Approx(1.0f));
    CHECK(offset.halfExtent == Approx(48.0f));
    // Unusable bounds are a 1 m cube.
    const rv::app::DebugGrid none = rv::app::modelViewerGrid(Aabb{});
    CHECK(none.spacing == Approx(0.1f));
    CHECK(none.halfExtent == Approx(2.0f));
}
