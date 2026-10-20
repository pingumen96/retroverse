#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include <glm/ext/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <rv/core/file_io.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/scene/light.hpp>
#include <rv/scene/scene_description.hpp>

#include "support/temp_dir.hpp"

using Catch::Approx;
using nlohmann::json;
using rv::scene::LightType;
using rv::scene::SceneDescription;
using rv::scene::SceneObject;
using rv::scene::TextureSource;

namespace {

rv::Expected<SceneDescription> parse(std::string_view text, std::string_view context = "test.json") {
    return rv::scene::parseSceneDescription(json::parse(text, nullptr, true, /*ignore_comments*/ true), context);
}

glm::vec3 linear(std::string_view hex) {
    return rv::core::hexToLinearRgb(hex).value();
}

glm::vec3 translationOf(const glm::mat4& transform) {
    return glm::vec3(transform[3]);
}

} // namespace

TEST_CASE("scene/scene_description: light type names round-trip", "[scene][scene_description]") {
    for (const LightType type : {LightType::Point, LightType::Spot, LightType::Directional}) {
        CHECK(rv::scene::lightTypeFromString(rv::scene::toString(type)) == type);
    }
    CHECK(rv::scene::toString(LightType::Spot) == "spot");
    CHECK_FALSE(rv::scene::lightTypeFromString("area").has_value());
    CHECK_FALSE(rv::scene::lightTypeFromString("Point").has_value()); // names are case-sensitive
}

TEST_CASE("scene/scene_description: omitted sections take the documented defaults", "[scene][scene_description]") {
    const auto scene = parse(R"({"schemaVersion": 1, "name": "minimal"})");
    REQUIRE(scene.has_value());
    CHECK(scene->name == "minimal");
    CHECK(scene->camera.position == glm::vec3(0.0f, 1.65f, 0.0f));
    CHECK(scene->camera.yawDeg == 0.0f);
    CHECK(scene->camera.lens.horizontalFovDeg == 75.0f);
    CHECK(scene->camera.lens.nearPlane == 0.05f);
    CHECK(scene->camera.lens.farPlane == 100.0f);
    // The struct defaults are the linear values of the documented hex colors.
    CHECK(rv::core::nearlyEqual(scene->environment.clearColor, linear("#383735")));
    CHECK(rv::core::nearlyEqual(scene->environment.ambientSky, linear("#FFFFFF")));
    CHECK(rv::core::nearlyEqual(scene->environment.ambientGround, linear("#808080")));
    CHECK(scene->environment.ambientIntensity == 0.2f);
    CHECK(scene->environment.exposure == 1.0f);
    // Post-processing: a subtle bloom and a minimal vignette (style §4.3, §18.2).
    CHECK(scene->environment.bloomStrength == 0.06f);
    CHECK(scene->environment.bloomRadius == 0.7f);
    CHECK(scene->environment.vignette == 0.05f);
    // Ambient occlusion on, 0.4 m radius, intensity 1 (the plan's defaults).
    CHECK(scene->environment.ssao.enabled);
    CHECK(scene->environment.ssao.radius == 0.4f);
    CHECK(scene->environment.ssao.intensity == 1.0f);
    CHECK(scene->lights.empty());
    CHECK(scene->objects.empty());
    CHECK(scene->findObject("anything") == nullptr);
}

TEST_CASE("scene/scene_description: camera, environment and lights", "[scene][scene_description]") {
    const auto scene = parse(R"({
        "schemaVersion": 1, "name": "lit",
        // Comments are allowed in scene files.
        "camera": {"position": [1, 1.6, 4], "yawDeg": -30, "pitchDeg": 5, "horizontalFovDeg": 80, "near": 0.1, "far": 60},
        "environment": {"clearColor": "#000000", "ambientSky": "#FFC88A", "ambientGround": "#202020",
                        "ambientIntensity": 0.3, "exposure": 1.5, "bloomStrength": 0.0, "bloomRadius": 1.0,
                        "vignette": 0.25},
        "lights": [
            {"type": "point", "position": [0, 2.5, -3]},
            {"name": "accent", "type": "spot", "position": [2, 3, 0], "target": [2, 0, -4], "color": "#B34DCE",
             "intensity": 0.5, "range": 6, "innerConeDeg": 20, "outerConeDeg": 30, "castsShadows": true},
            {"name": "sun", "type": "directional", "direction": [0, -2, 0], "intensity": 3, "range": 0}
        ]
    })");
    REQUIRE(scene.has_value());
    CHECK(scene->camera.position == glm::vec3(1.0f, 1.6f, 4.0f));
    CHECK(scene->camera.yawDeg == -30.0f);
    CHECK(scene->camera.pitchDeg == 5.0f);
    CHECK(scene->camera.lens.horizontalFovDeg == 80.0f);
    CHECK(scene->camera.lens.nearPlane == 0.1f);
    CHECK(scene->camera.lens.farPlane == 60.0f);
    CHECK(scene->environment.clearColor == glm::vec3(0.0f));
    CHECK(rv::core::nearlyEqual(scene->environment.ambientSky, linear("#FFC88A")));
    CHECK(scene->environment.ambientIntensity == 0.3f);
    CHECK(scene->environment.exposure == 1.5f);
    CHECK(scene->environment.bloomStrength == 0.0f);
    CHECK(scene->environment.bloomRadius == 1.0f);
    CHECK(scene->environment.vignette == 0.25f);

    REQUIRE(scene->lights.size() == 3);
    const auto& point = scene->lights[0];
    CHECK(point.name.empty());
    CHECK(point.type == LightType::Point);
    CHECK(point.position == glm::vec3(0.0f, 2.5f, -3.0f));
    CHECK(point.color == glm::vec3(1.0f));
    CHECK(point.intensity == 1.0f);
    CHECK(point.range == 10.0f);
    CHECK_FALSE(point.castsShadows);

    const auto& spot = scene->lights[1];
    CHECK(spot.type == LightType::Spot);
    CHECK(rv::core::nearlyEqual(spot.direction, glm::vec3(0.0f, -0.6f, -0.8f))); // normalize(target - position)
    CHECK(rv::core::nearlyEqual(spot.color, linear("#B34DCE")));
    CHECK(spot.color.x < 0.5f); // converted to linear, not the raw sRGB 0.70
    CHECK(spot.intensity == 0.5f);
    CHECK(spot.range == 6.0f);
    CHECK(spot.innerConeDeg == 20.0f);
    CHECK(spot.outerConeDeg == 30.0f);
    CHECK(spot.castsShadows);
    CHECK(spot.shadowStrength == 1.0f); // the default: its shadows take all of its light
    CHECK(point.shadowStrength == 1.0f);

    const auto& sun = scene->lights[2];
    CHECK(sun.type == LightType::Directional);
    CHECK(rv::core::nearlyEqual(sun.direction, glm::vec3(0.0f, -1.0f, 0.0f)));
    CHECK(sun.range == 0.0f); // ignored for directional lights, so not validated
}

TEST_CASE("scene/scene_description: ambient occlusion and shadow strength", "[scene][scene_description]") {
    const auto scene = parse(R"({
        "schemaVersion": 1, "name": "shaded",
        "environment": {"ssao": {"enabled": false, "radius": 0.25, "intensity": 2.0}},
        "lights": [{"name": "key", "type": "spot", "position": [0, 2.9, 0], "direction": [0, -1, 0],
                    "castsShadows": true, "shadowStrength": 0.75}]
    })");
    REQUIRE(scene.has_value());
    CHECK_FALSE(scene->environment.ssao.enabled);
    CHECK(scene->environment.ssao.radius == 0.25f);
    CHECK(scene->environment.ssao.intensity == 2.0f);
    REQUIRE(scene->lights.size() == 1);
    CHECK(scene->lights[0].castsShadows);
    CHECK(scene->lights[0].shadowStrength == 0.75f);

    // Keys left out keep their defaults; the documented bounds are accepted.
    const auto partial = parse(R"({"schemaVersion": 1, "name": "partial",
        "environment": {"ssao": {"intensity": 0}},
        "lights": [{"type": "spot", "position": [0, 2.9, 0], "direction": [0, -1, 0], "shadowStrength": 0}]})");
    REQUIRE(partial.has_value());
    CHECK(partial->environment.ssao.enabled);
    CHECK(partial->environment.ssao.radius == 0.4f);
    CHECK(partial->environment.ssao.intensity == 0.0f);
    CHECK(partial->lights[0].shadowStrength == 0.0f);
    const auto bounds = parse(R"({"schemaVersion": 1, "name": "bounds",
        "environment": {"ssao": {"radius": 5, "intensity": 4}}})");
    REQUIRE(bounds.has_value());
    CHECK(bounds->environment.ssao.radius == rv::scene::kMaxSsaoRadius);
    CHECK(bounds->environment.ssao.intensity == rv::scene::kMaxSsaoIntensity);
}

TEST_CASE("scene/scene_description: light directions stay unit vectors at extreme coordinates",
          "[scene][scene_description]") {
    // The float dot products of these axes overflow to infinity: normalizing in float gave (0, 0, 0) and NaN.
    const auto scene = parse(R"({"schemaVersion": 1, "name": "far", "lights": [
        {"type": "directional", "direction": [1e20, 0, 0]},
        {"type": "spot", "position": [-3e38, 0, 0], "target": [3e38, 0, 0]},
        {"type": "spot", "position": [0, 0, 0], "direction": [3e38, -3e38, 0]}]})");
    REQUIRE(scene.has_value());
    REQUIRE(scene->lights.size() == 3);
    CHECK(rv::core::nearlyEqual(scene->lights[0].direction, glm::vec3(1.0f, 0.0f, 0.0f)));
    CHECK(rv::core::nearlyEqual(scene->lights[1].direction, glm::vec3(1.0f, 0.0f, 0.0f)));
    CHECK(rv::core::nearlyEqual(scene->lights[2].direction, glm::normalize(glm::vec3(1.0f, -1.0f, 0.0f))));
}

TEST_CASE("scene/scene_description: spot cones may be hard-edged", "[scene][scene_description]") {
    // inner == outer is valid (0 <= inner <= outer < 90); the renderer contract in light.hpp keeps it defined.
    const auto scene = parse(R"({"schemaVersion": 1, "name": "hard", "lights": [
        {"type": "spot", "position": [0, 3, 0], "direction": [0, -1, 0], "innerConeDeg": 30, "outerConeDeg": 30},
        {"type": "spot", "position": [0, 3, 0], "direction": [0, -1, 0], "innerConeDeg": 0, "outerConeDeg": 0}]})");
    REQUIRE(scene.has_value());
    REQUIRE(scene->lights.size() == 2);
    CHECK(scene->lights[0].innerConeDeg == 30.0f);
    CHECK(scene->lights[0].outerConeDeg == 30.0f);
    CHECK(scene->lights[1].innerConeDeg == 0.0f);
    CHECK(scene->lights[1].outerConeDeg == 0.0f);
}

TEST_CASE("scene/scene_description: object sources, materials, textures and flags", "[scene][scene_description]") {
    const auto scene = parse(R"({
        "schemaVersion": 1, "name": "objects",
        "objects": [
            {"id": "cart", "prefab": "cartridge", "params": {"size": [0.125, 0.085, 0.02], "nested": {"k": [1, 2]}},
             "material": "MAT_Plastic_Gray", "materials": {"label": "MAT_Label_Print"},
             "textures": {"label": {"pattern": "cover", "title": "SUPER PLACEHOLDER WORLD", "seed": 3}},
             "castsShadows": false, "collides": false},
            {"id": "ball", "primitive": "sphere", "inlineMaterial": {"shading": "pbr", "roughness": 0.25}},
            {"id": "tv", "model": "models/electronics/crt_generic.glb",
             "textures": {"screen": {"file": "textures/test/bars.png"}, "dial": {"pattern": "checker"}}},
            {"id": "panel", "primitive": "box", "material": "MAT_Wood_Walnut",
             "textures": {"box": {"procedural": "oak_planks/baseColor"}}},
            {"id": "sign", "prefab": "poster_frame", "textures": {"picture": {"pattern": "sign", "title": "ARCADE"}}}
        ]
    })");
    REQUIRE(scene.has_value());
    REQUIRE(scene->objects.size() == 5);

    const SceneObject& cart = *scene->findObject("cart");
    CHECK(cart.source == SceneObject::Source::Prefab);
    CHECK(cart.kind == "cartridge");
    CHECK(cart.modelPath.empty());
    CHECK(cart.params == json::parse(R"({"size": [0.125, 0.085, 0.02], "nested": {"k": [1, 2]}})"));
    CHECK(cart.materialNameForRole("label") == "MAT_Label_Print");
    CHECK(cart.materialNameForRole("plastic_shell") == "MAT_Plastic_Gray"); // falls back to "material"
    const TextureSource& label = cart.texturesByRole.at("label");
    CHECK(label.kind == TextureSource::Kind::Pattern);
    CHECK(label.pattern == "cover");
    CHECK(label.title == "SUPER PLACEHOLDER WORLD");
    CHECK(label.seed == 3u);
    CHECK(cart.inlineMaterial.is_null());
    CHECK_FALSE(cart.castsShadows);
    CHECK_FALSE(cart.collides);

    const SceneObject& ball = *scene->findObject("ball");
    CHECK(ball.source == SceneObject::Source::Primitive);
    CHECK(ball.kind == "sphere");
    CHECK(ball.params == json::object());
    CHECK(ball.materialNameForRole("any").empty()); // the application uses inlineMaterial
    CHECK(ball.inlineMaterial == json::parse(R"({"shading": "pbr", "roughness": 0.25})"));
    CHECK(ball.castsShadows);
    CHECK(ball.collides);
    CHECK(ball.transform == glm::mat4(1.0f));
    REQUIRE(ball.instanceTransforms().size() == 1);
    CHECK(ball.instanceTransforms()[0] == ball.transform);

    const SceneObject& tv = *scene->findObject("tv");
    CHECK(tv.source == SceneObject::Source::Model);
    CHECK(tv.kind.empty());
    CHECK(tv.modelPath == "models/electronics/crt_generic.glb");
    CHECK(tv.texturesByRole.at("screen").kind == TextureSource::Kind::File);
    CHECK(tv.texturesByRole.at("screen").path == "textures/test/bars.png");
    CHECK(tv.texturesByRole.at("dial").pattern == "checker");
    CHECK(tv.texturesByRole.at("dial").seed == 0u);

    // A procedural surface texture of the catalog, named "<surface>/<map>", and the sign lettering pattern.
    const TextureSource& planks = scene->findObject("panel")->texturesByRole.at("box");
    CHECK(planks.kind == TextureSource::Kind::Procedural);
    CHECK(planks.procedural == "oak_planks/baseColor");
    CHECK(planks.pattern.empty());
    const TextureSource& lettering = scene->findObject("sign")->texturesByRole.at("picture");
    CHECK(lettering.kind == TextureSource::Kind::Pattern);
    CHECK(lettering.pattern == "sign");
    CHECK(lettering.title == "ARCADE");
}

TEST_CASE("scene/scene_description: transforms compose translate * Ry * Rx * Rz * scale",
          "[scene][scene_description]") {
    const auto scene = parse(R"({
        "schemaVersion": 1, "name": "transforms",
        "objects": [
            {"id": "a", "primitive": "box", "position": [1, 2, 3], "rotationDeg": [10, 20, 30], "scale": [2, 3, 4]},
            {"id": "b", "primitive": "box", "scale": 2.5}
        ]
    })");
    REQUIRE(scene.has_value());
    const glm::mat4 identity(1.0f);
    const glm::mat4 expected = glm::translate(identity, glm::vec3(1.0f, 2.0f, 3.0f)) *
                               glm::rotate(identity, glm::radians(20.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
                               glm::rotate(identity, glm::radians(10.0f), glm::vec3(1.0f, 0.0f, 0.0f)) *
                               glm::rotate(identity, glm::radians(30.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
                               glm::scale(identity, glm::vec3(2.0f, 3.0f, 4.0f));
    CHECK(rv::core::nearlyEqual(scene->objects[0].transform, expected));
    // Another rotation order gives a different matrix, so the check above pins the convention.
    const glm::mat4 xyz = glm::translate(identity, glm::vec3(1.0f, 2.0f, 3.0f)) *
                          glm::rotate(identity, glm::radians(10.0f), glm::vec3(1.0f, 0.0f, 0.0f)) *
                          glm::rotate(identity, glm::radians(20.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
                          glm::rotate(identity, glm::radians(30.0f), glm::vec3(0.0f, 0.0f, 1.0f)) *
                          glm::scale(identity, glm::vec3(2.0f, 3.0f, 4.0f));
    CHECK_FALSE(rv::core::nearlyEqual(scene->objects[0].transform, xyz, 1e-3f));
    CHECK(rv::core::nearlyEqual(scene->objects[1].transform, glm::scale(identity, glm::vec3(2.5f))));
}

TEST_CASE("scene/scene_description: grids expand in local space with x fastest", "[scene][scene_description]") {
    const auto scene = parse(R"({
        "schemaVersion": 1, "name": "grid",
        "objects": [{"id": "g", "primitive": "box", "position": [5, 0, 0], "rotationDeg": [0, 90, 0],
                     "grid": {"counts": [3, 2, 2], "spacing": [1, 10, 100]}}]
    })");
    REQUIRE(scene.has_value());
    const SceneObject& grid = scene->objects[0];
    REQUIRE(grid.instances.size() == 12);
    CHECK(grid.instanceTransforms().size() == 12);
    CHECK(rv::core::nearlyEqual(grid.instances[0], grid.transform));
    // Spacing is rotated with the object: +90 degrees about Y maps local +X to world -Z and local +Z to world +X.
    CHECK(rv::core::nearlyEqual(translationOf(grid.instances[1]), glm::vec3(5.0f, 0.0f, -1.0f), 1e-4f));
    CHECK(rv::core::nearlyEqual(translationOf(grid.instances[2]), glm::vec3(5.0f, 0.0f, -2.0f), 1e-4f));
    CHECK(rv::core::nearlyEqual(translationOf(grid.instances[3]), glm::vec3(5.0f, 10.0f, 0.0f), 1e-4f));
    CHECK(rv::core::nearlyEqual(translationOf(grid.instances[6]), glm::vec3(105.0f, 0.0f, 0.0f), 1e-3f));
    CHECK(rv::core::nearlyEqual(translationOf(grid.instances[11]), glm::vec3(105.0f, 10.0f, -2.0f), 1e-3f));
    // Instances keep the object's rotation.
    CHECK(rv::core::nearlyEqual(glm::vec3(grid.instances[7][0]), glm::vec3(grid.transform[0])));
}

TEST_CASE("scene/scene_description: the instance budget allows exactly 100000 instances",
          "[scene][scene_description]") {
    const auto atLimit = parse(R"({"schemaVersion": 1, "name": "big", "objects": [
        {"id": "g", "primitive": "box", "grid": {"counts": [100, 100, 10], "spacing": [1, 1, 1]}}]})");
    REQUIRE(atLimit.has_value());
    CHECK(atLimit->objects[0].instances.size() == rv::scene::kMaxSceneInstances);

    const auto overLimit = parse(R"({"schemaVersion": 1, "name": "big", "objects": [
        {"id": "g", "primitive": "box", "grid": {"counts": [100, 100, 10], "spacing": [1, 1, 1]}},
        {"id": "one_more", "primitive": "box"}]})");
    REQUIRE_FALSE(overLimit.has_value());
    CHECK(overLimit.error().code == rv::ErrorCode::ValidationFailed);
    CHECK(overLimit.error().context == "test.json:/objects/1");
}

TEST_CASE("scene/scene_description: every validation rule reports its JSON path", "[scene][scene_description]") {
    struct InvalidCase {
        const char* rule;
        const char* document;
        const char* pointer;
    };
    // Each document breaks exactly one rule.
    const InvalidCase cases[] = {
        {"schemaVersion is required", R"({"name": "x"})", "/schemaVersion"},
        {"schemaVersion must be 1", R"({"schemaVersion": 2, "name": "x"})", "/schemaVersion"},
        {"schemaVersion is an integer", R"({"schemaVersion": 1.5, "name": "x"})", "/schemaVersion"},
        {"name is required", R"({"schemaVersion": 1})", "/name"},
        {"name is not empty", R"({"schemaVersion": 1, "name": ""})", "/name"},
        {"unknown root key", R"({"schemaVersion": 1, "name": "x", "sceneName": "y"})", "/sceneName"},
        {"unknown camera key", R"({"schemaVersion": 1, "name": "x", "camera": {"fov": 70}})", "/camera/fov"},
        {"camera is an object", R"({"schemaVersion": 1, "name": "x", "camera": [0, 1, 2]})", "/camera"},
        {"near > 0", R"({"schemaVersion": 1, "name": "x", "camera": {"near": 0}})", "/camera/near"},
        {"far > near", R"({"schemaVersion": 1, "name": "x", "camera": {"near": 2, "far": 1}})", "/camera/far"},
        {"near beyond the default far is reported at near",
         R"({"schemaVersion": 1, "name": "x", "camera": {"near": 150}})", "/camera/near"},
        {"FOV range", R"({"schemaVersion": 1, "name": "x", "camera": {"horizontalFovDeg": 180}})",
         "/camera/horizontalFovDeg"},
        {"pitch range", R"({"schemaVersion": 1, "name": "x", "camera": {"pitchDeg": 95}})", "/camera/pitchDeg"},
        {"position has 3 numbers", R"({"schemaVersion": 1, "name": "x", "camera": {"position": [0, 1]}})",
         "/camera/position"},
        {"position elements are numbers", R"({"schemaVersion": 1, "name": "x", "camera": {"position": [0, "1", 2]}})",
         "/camera/position/1"},
        {"exposure > 0", R"({"schemaVersion": 1, "name": "x", "environment": {"exposure": 0}})",
         "/environment/exposure"},
        {"bloom strength >= 0", R"({"schemaVersion": 1, "name": "x", "environment": {"bloomStrength": -0.01}})",
         "/environment/bloomStrength"},
        {"bloom strength <= 1", R"({"schemaVersion": 1, "name": "x", "environment": {"bloomStrength": 1.5}})",
         "/environment/bloomStrength"},
        {"bloom radius within [0, 1]", R"({"schemaVersion": 1, "name": "x", "environment": {"bloomRadius": 2}})",
         "/environment/bloomRadius"},
        {"bloom radius is a number", R"({"schemaVersion": 1, "name": "x", "environment": {"bloomRadius": "wide"}})",
         "/environment/bloomRadius"},
        {"vignette within [0, 1]", R"({"schemaVersion": 1, "name": "x", "environment": {"vignette": -0.05}})",
         "/environment/vignette"},
        {"unknown environment key", R"({"schemaVersion": 1, "name": "x", "environment": {"bloomThreshold": 1}})",
         "/environment/bloomThreshold"},
        {"ssao is an object", R"({"schemaVersion": 1, "name": "x", "environment": {"ssao": true}})",
         "/environment/ssao"},
        {"ssao enabled is a boolean", R"({"schemaVersion": 1, "name": "x", "environment": {"ssao": {"enabled": 1}}})",
         "/environment/ssao/enabled"},
        {"ssao radius > 0", R"({"schemaVersion": 1, "name": "x", "environment": {"ssao": {"radius": 0}}})",
         "/environment/ssao/radius"},
        {"ssao radius <= 5 m", R"({"schemaVersion": 1, "name": "x", "environment": {"ssao": {"radius": 5.5}}})",
         "/environment/ssao/radius"},
        {"ssao intensity >= 0", R"({"schemaVersion": 1, "name": "x", "environment": {"ssao": {"intensity": -1}}})",
         "/environment/ssao/intensity"},
        {"ssao intensity <= 4", R"({"schemaVersion": 1, "name": "x", "environment": {"ssao": {"intensity": 4.5}}})",
         "/environment/ssao/intensity"},
        {"unknown ssao key", R"({"schemaVersion": 1, "name": "x", "environment": {"ssao": {"samples": 16}}})",
         "/environment/ssao/samples"},
        {"ambient intensity >= 0", R"({"schemaVersion": 1, "name": "x", "environment": {"ambientIntensity": -1}})",
         "/environment/ambientIntensity"},
        {"colors are hex strings", R"({"schemaVersion": 1, "name": "x", "environment": {"clearColor": "#12345"}})",
         "/environment/clearColor"},
        {"colors are strings", R"({"schemaVersion": 1, "name": "x", "environment": {"ambientSky": [1, 1, 1]}})",
         "/environment/ambientSky"},
        {"colors have no alpha", R"({"schemaVersion": 1, "name": "x", "environment": {"clearColor": "#20222480"}})",
         "/environment/clearColor"},
        {"colors start with '#'",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "point", "position": [0, 2, 0], "color": "FFC88A"}]})",
         "/lights/0/color"},
        {"lights is an array", R"({"schemaVersion": 1, "name": "x", "lights": {}})", "/lights"},
        {"light type is required", R"({"schemaVersion": 1, "name": "x", "lights": [{"position": [0, 0, 0]}]})",
         "/lights/0/type"},
        {"known light type",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "area", "position": [0, 0, 0]}]})", "/lights/0/type"},
        {"point lights need a position", R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "point"}]})",
         "/lights/0/position"},
        {"spot lights need a direction",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "spot", "position": [0, 2, 0]}]})",
         "/lights/0/direction"},
        {"directional lights need a direction",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "directional"}]})", "/lights/0/direction"},
        {"direction and target are exclusive",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "spot", "position": [0, 2, 0], "direction": [0, -1, 0], "target": [0, 0, 0]}]})",
         "/lights/0/target"},
        {"direction is not zero",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "directional", "direction": [0, 0, 0]}]})",
         "/lights/0/direction"},
        {"target differs from position",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "spot", "position": [1, 2, 3], "target": [1, 2, 3]}]})",
         "/lights/0/target"},
        {"intensity >= 0",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "point", "position": [0, 2, 0]}, {"type": "point", "position": [0, 2, 0], "intensity": -0.5}]})",
         "/lights/1/intensity"},
        {"range > 0 for point lights",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "point", "position": [0, 2, 0], "range": 0}]})",
         "/lights/0/range"},
        {"range > 0 for spot lights",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "spot", "position": [0, 2, 0], "direction": [0, -1, 0], "range": -3}]})",
         "/lights/0/range"},
        {"inner cone >= 0",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "point", "position": [0, 2, 0], "innerConeDeg": -1}]})",
         "/lights/0/innerConeDeg"},
        {"inner cone <= outer cone",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "spot", "position": [0, 2, 0], "direction": [0, -1, 0], "innerConeDeg": 40, "outerConeDeg": 35}]})",
         "/lights/0/innerConeDeg"},
        {"outer cone narrower than the default inner cone is reported at outerConeDeg",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "spot", "position": [0, 2, 0], "direction": [0, -1, 0], "outerConeDeg": 20}]})",
         "/lights/0/outerConeDeg"},
        {"outer cone < 90",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "spot", "position": [0, 2, 0], "direction": [0, -1, 0], "outerConeDeg": 90}]})",
         "/lights/0/outerConeDeg"},
        {"unknown light key",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "point", "position": [0, 2, 0], "colour": "#FFFFFF"}]})",
         "/lights/0/colour"},
        {"shadow strength within [0, 1]",
         R"({"schemaVersion": 1, "name": "x", "lights": [)"
         R"({"type": "point", "position": [0, 2, 0], "shadowStrength": 1.5}]})",
         "/lights/0/shadowStrength"},
        {"shadow strength >= 0",
         R"({"schemaVersion": 1, "name": "x", "lights": [)"
         R"({"type": "point", "position": [0, 2, 0], "shadowStrength": -0.1}]})",
         "/lights/0/shadowStrength"},
        {"casts shadows is a boolean",
         R"({"schemaVersion": 1, "name": "x", "lights": [)"
         R"({"type": "point", "position": [0, 2, 0], "castsShadows": "yes"}]})",
         "/lights/0/castsShadows"},
        {"objects is an array", R"({"schemaVersion": 1, "name": "x", "objects": {"id": "a"}})", "/objects"},
        {"object id is required", R"({"schemaVersion": 1, "name": "x", "objects": [{"primitive": "box"}]})",
         "/objects/0/id"},
        {"object ids are unique",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "primitive": "box"}, {"id": "a", "prefab": "rug"}]})",
         "/objects/1/id"},
        {"an object needs a source", R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a"}]})", "/objects/0"},
        {"an object has exactly one source",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "prefab": "rug", "primitive": "box"}]})",
         "/objects/0"},
        {"source kinds are not empty", R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "prefab": ""}]})",
         "/objects/0/prefab"},
        {"model paths stay inside the asset root",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "model": "models/../../secret.glb"}]})",
         "/objects/0/model"},
        {"model paths are relative",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "model": "C:/models/tv.glb"}]})",
         "/objects/0/model"},
        {"model paths use forward slashes",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "model": "models\\tv.glb"}]})",
         "/objects/0/model"},
        {"scale components > 0",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "primitive": "box", "scale": [1, 0, 1]}]})",
         "/objects/0/scale"},
        {"uniform scale > 0",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "primitive": "box", "scale": -2}]})",
         "/objects/0/scale"},
        {"material and inlineMaterial are exclusive",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "primitive": "box", "material": "MAT_Plastic_Gray", "inlineMaterial": {}}]})",
         "/objects/0/inlineMaterial"},
        {"inlineMaterial is an object",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "primitive": "box", "inlineMaterial": "MAT_x"}]})",
         "/objects/0/inlineMaterial"},
        {"material names are not empty",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "prefab": "cartridge", "materials": {"label": ""}}]})",
         "/objects/0/materials/label"},
        {"role keys are escaped in the JSON pointer",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "prefab": "cartridge", "materials": {"a/b~c": 3}}]})",
         "/objects/0/materials/a~1b~0c"},
        {"textures need a pattern, a file or a procedural texture",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "prefab": "cartridge", "textures": {"label": {"title": "T"}}}]})",
         "/objects/0/textures/label"},
        {"textures have one source only",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "prefab": "cartridge", "textures": {"label": {"pattern": "cover", "file": "t.png"}}}]})",
         "/objects/0/textures/label"},
        {"known texture patterns",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "prefab": "cartridge", "textures": {"label": {"pattern": "plasma"}}}]})",
         "/objects/0/textures/label/pattern"},
        {"texture seeds are non-negative integers",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "prefab": "cartridge", "textures": {"label": {"pattern": "cover", "seed": -1}}}]})",
         "/objects/0/textures/label/seed"},
        {"textures name one procedural texture only",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "prefab": "cartridge", "textures": {"label": {"procedural": "walnut/baseColor", "file": "t.png"}}}]})",
         "/objects/0/textures/label"},
        {"known procedural surfaces",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "prefab": "cartridge", "textures": {"label": {"procedural": "teak/baseColor"}}}]})",
         "/objects/0/textures/label/procedural"},
        {"procedural textures name a map",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "prefab": "cartridge", "textures": {"label": {"procedural": "walnut"}}}]})",
         "/objects/0/textures/label/procedural"},
        {"procedural textures take no title",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "prefab": "cartridge", "textures": {"label": {"procedural": "walnut/baseColor", "title": "T"}}}]})",
         "/objects/0/textures/label/title"},
        {"file textures take no title",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "prefab": "cartridge", "textures": {"label": {"file": "t.png", "title": "T"}}}]})",
         "/objects/0/textures/label/title"},
        {"params is an object",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "primitive": "box", "params": [1, 2]}]})",
         "/objects/0/params"},
        {"grid counts >= 1",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "primitive": "box", "grid": {"counts": [2, 0, 2], "spacing": [1, 1, 1]}}]})",
         "/objects/0/grid/counts/1"},
        {"grid counts are integers",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "primitive": "box", "grid": {"counts": [2.5, 1, 1], "spacing": [1, 1, 1]}}]})",
         "/objects/0/grid/counts/0"},
        {"grid spacing is required",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "primitive": "box", "grid": {"counts": [2, 2, 2]}}]})",
         "/objects/0/grid/spacing"},
        {"one grid within the instance budget",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "primitive": "box", "grid": {"counts": [1000, 1000, 1], "spacing": [1, 1, 1]}}]})",
         "/objects/0/grid/counts"},
        {"all grids within the instance budget",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "primitive": "box", "grid": {"counts": [100, 100, 9], "spacing": [1, 1, 1]}}, {"id": "b", "primitive": "box", "grid": {"counts": [100, 10, 11], "spacing": [1, 1, 1]}}]})",
         "/objects/1/grid/counts"},
        {"unknown object key",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "primitive": "box", "colour": "#FFFFFF"}]})",
         "/objects/0/colour"},
        {"flags are booleans",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "primitive": "box", "castsShadows": 1}]})",
         "/objects/0/castsShadows"},
        {"huge numbers are rejected",
         R"({"schemaVersion": 1, "name": "x", "objects": [{"id": "a", "primitive": "box", "position": [0, 1e39, 0]}]})",
         "/objects/0/position/1"},
        // Areas (I4 step 3).
        {"areas is an array", R"({"schemaVersion": 1, "name": "x", "areas": {"id": "a"}})", "/areas"},
        {"an area's id is required", R"({"schemaVersion": 1, "name": "x", "areas": [{"boxes": [{"min": [0, 0, 0], "max": [1, 1, 1]}]}]})",
         "/areas/0/id"},
        {"an area's boxes are required", R"({"schemaVersion": 1, "name": "x", "areas": [{"id": "a"}]})",
         "/areas/0/boxes"},
        {"an area has a box", R"({"schemaVersion": 1, "name": "x", "areas": [{"id": "a", "boxes": []}]})",
         "/areas/0/boxes"},
        {"a box's min is below its max",
         R"({"schemaVersion": 1, "name": "x", "areas": [{"id": "a", "boxes": [{"min": [0, 0, 0], "max": [1, 0, 1]}]}]})",
         "/areas/0/boxes/0/max"},
        {"unknown area key",
         R"({"schemaVersion": 1, "name": "x", "areas": [{"id": "a", "boxes": [{"min": [0, 0, 0], "max": [1, 1, 1]}], "lights": 2}]})",
         "/areas/0/lights"},
        {"area ids are unique",
         R"({"schemaVersion": 1, "name": "x", "areas": [{"id": "a", "boxes": [{"min": [0, 0, 0], "max": [1, 1, 1]}]}, {"id": "a", "boxes": [{"min": [1, 0, 0], "max": [2, 1, 1]}]}]})",
         "/areas/1/id"},
        {"a light names no area in a scene without areas",
         R"({"schemaVersion": 1, "name": "x", "lights": [{"type": "point", "position": [0, 1, 0], "area": "a"}]})",
         "/lights/0/area"},
        {"a light names its area in a scene with areas",
         R"({"schemaVersion": 1, "name": "x", "areas": [{"id": "a", "boxes": [{"min": [0, 0, 0], "max": [1, 1, 1]}]}], "lights": [{"type": "point", "position": [0, 1, 0]}]})",
         "/lights/0"},
        {"a light names an area of the scene",
         R"({"schemaVersion": 1, "name": "x", "areas": [{"id": "a", "boxes": [{"min": [0, 0, 0], "max": [1, 1, 1]}]}], "lights": [{"type": "point", "position": [0, 1, 0], "area": "b"}]})",
         "/lights/0/area"},
    };

    for (const InvalidCase& invalid : cases) {
        INFO("rule: " << invalid.rule);
        const auto result = parse(invalid.document);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().code == rv::ErrorCode::ValidationFailed);
        CHECK(result.error().context == std::string("test.json:") + invalid.pointer);
        CHECK_FALSE(result.error().message.empty());
    }
}

TEST_CASE("scene/scene_description: errors name the file and the JSON path", "[scene][scene_description]") {
    const auto result = parse(R"({"schemaVersion": 1, "name": "m0_testroom", "objects": [
        {"id": "room", "prefab": "room_shell"}, {"id": "shelf", "prefab": "shelf"}, {"id": "crt", "prefab": "crt"},
        {"id": "rug", "prefab": "rug", "params": "wide"}]})",
                              "m0_testroom.json");
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().context == "m0_testroom.json:/objects/3/params");
    CHECK(result.error().toString().find("expected an object") != std::string::npos);

    // Unknown keys list what is allowed.
    const auto unknown = parse(R"({"schemaVersion": 1, "name": "x", "camera": {"fov": 70}})");
    REQUIRE_FALSE(unknown.has_value());
    CHECK(unknown.error().message.find("horizontalFovDeg") != std::string::npos);

    // Non-object documents are rejected at the root.
    const auto root = rv::scene::parseSceneDescription(json::array(), "test.json");
    REQUIRE_FALSE(root.has_value());
    CHECK(root.error().context == "test.json:/");
}

TEST_CASE("scene/scene_description: files load with their name as error context", "[scene][scene_description]") {
    const rv::test::TempDir dir("rv-scene");

    const auto good = dir.file("good_scene.json");
    REQUIRE(rv::core::writeTextFileAtomic(good, R"(// header comment
        {"schemaVersion": 1, "name": "good", "objects": [{"id": "floor", "primitive": "plane"}]})")
                .has_value());
    const auto loaded = rv::scene::loadSceneDescription(good);
    REQUIRE(loaded.has_value());
    CHECK(loaded->objects.size() == 1);

    const auto invalid = dir.file("broken_scene.json");
    REQUIRE(rv::core::writeTextFileAtomic(invalid, R"({"schemaVersion": 1, "name": "x", "oops": true})").has_value());
    const auto broken = rv::scene::loadSceneDescription(invalid);
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().code == rv::ErrorCode::ValidationFailed);
    CHECK(broken.error().context == "broken_scene.json:/oops");

    const auto syntax = dir.file("syntax.json");
    REQUIRE(rv::core::writeTextFileAtomic(syntax, R"({"schemaVersion": 1,)").has_value());
    CHECK(rv::scene::loadSceneDescription(syntax).error().code == rv::ErrorCode::ParseError);

    // A number literal that overflows a double makes nlohmann::json throw out_of_range instead of parse_error:
    // still a ParseError with the file path, never an exception leaving the module.
    const auto overflow = dir.file("overflow.json");
    for (const std::string& literal : {std::string("1e400"), std::string(400, '9')}) {
        INFO("literal of " << literal.size() << " characters");
        REQUIRE(rv::core::writeTextFileAtomic(
                    overflow, R"({"schemaVersion": 1, "name": "x", "camera": {"far": )" + literal + "}}")
                    .has_value());
        const auto overflowed = rv::scene::loadSceneDescription(overflow);
        REQUIRE_FALSE(overflowed.has_value());
        CHECK(overflowed.error().code == rv::ErrorCode::ParseError);
        CHECK(overflowed.error().context == rv::core::pathToUtf8(overflow));
        CHECK(overflowed.error().message.find("overflow") != std::string::npos);
    }

    CHECK(rv::scene::loadSceneDescription(dir.file("missing.json")).error().code == rv::ErrorCode::NotFound);
}
