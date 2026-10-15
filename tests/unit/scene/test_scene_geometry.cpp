#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <rv/assets/material_library.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/scene/scene_description.hpp>
#include <rv/scene/scene_geometry.hpp>

using nlohmann::json;
using rv::scene::SceneGeometry;
using rv::scene::SceneObjectGeometry;
using rv::scene::ScenePart;

namespace {

std::filesystem::path assetDir() {
    return rv::core::pathFromUtf8(RV_TEST_ASSET_DIR);
}

rv::assets::MaterialLibrary realMaterials() {
    auto library = rv::assets::MaterialLibrary::loadFromFile(assetDir() / "materials/base/materials.json");
    REQUIRE(library.has_value());
    return std::move(*library);
}

rv::scene::SceneDescription referenceScene(std::string_view name) {
    auto scene = rv::scene::loadSceneDescription(assetDir() / "style_tests/reference_scenes" /
                                                 rv::core::pathFromUtf8(std::string(name) + ".json"));
    REQUIRE(scene.has_value());
    return std::move(*scene);
}

const SceneObjectGeometry& object(const SceneGeometry& geometry, std::string_view id) {
    INFO("object " << id);
    const auto found = std::find_if(geometry.objects.begin(), geometry.objects.end(),
                                    [id](const SceneObjectGeometry& candidate) { return candidate.id == id; });
    REQUIRE(found != geometry.objects.end());
    return *found;
}

const ScenePart& part(const SceneObjectGeometry& owner, std::string_view role) {
    INFO("object " << owner.id << ", part " << role);
    const auto found = std::find_if(owner.parts.begin(), owner.parts.end(),
                                    [role](const ScenePart& candidate) { return candidate.role == role; });
    REQUIRE(found != owner.parts.end());
    return *found;
}

glm::vec4 linear(std::string_view hex) {
    const auto color = rv::core::hexToLinearRgb(hex);
    REQUIRE(color.has_value());
    return glm::vec4(*color, 1.0f);
}

} // namespace

TEST_CASE("scene/scene_geometry: the M0 test room resolves every part to a library material", "[scene][geometry]") {
    const auto materials = realMaterials();
    const auto scene = referenceScene("m0_testroom");
    const SceneGeometry geometry = rv::scene::buildSceneGeometry(scene, materials);

    REQUIRE(geometry.objects.size() == scene.objects.size());
    for (std::size_t i = 0; i < scene.objects.size(); ++i) {
        CHECK(geometry.objects[i].id == scene.objects[i].id); // file order
        CHECK_FALSE(geometry.objects[i].placeholder);
        CHECK(geometry.objects[i].instances.size() == 1);
    }
    CHECK(geometry.placeholderCount() == 0);
    CHECK(geometry.fallbackMaterialCount() == 0);
    CHECK(geometry.instanceCount() == scene.objects.size());

    const SceneObjectGeometry& room = object(geometry, "room");
    CHECK(part(room, "floor").material.name == "MAT_Wood_Oak");
    CHECK(part(room, "wall").material.name == "MAT_Wall_Painted");
    CHECK(rv::core::nearlyEqual(part(room, "wall").material.baseColorFactor, linear("#383735")));

    const SceneObjectGeometry& cartridge = object(geometry, "cartridge_2_3");
    REQUIRE(cartridge.parts.size() == 2);
    CHECK(part(cartridge, "plastic_shell").material.name == "MAT_Plastic_Gray");
    CHECK(part(cartridge, "label").material.name == "MAT_Label_Print");
    CHECK(part(cartridge, "label").mesh.name == "cartridge/label");
    CHECK(rv::core::nearlyEqual(cartridge.instances.front(), scene.findObject("cartridge_2_3")->transform));

    const SceneObjectGeometry& crt = object(geometry, "crt_tv");
    CHECK(part(crt, "screen").material.name == "MAT_Screen_Emissive");
    CHECK(part(crt, "plastic_body").material.name == "MAT_Plastic_AgedIvory");

    std::size_t triangles = 0;
    for (const SceneObjectGeometry& built : geometry.objects) {
        for (const ScenePart& builtPart : built.parts) {
            CHECK_FALSE(builtPart.mesh.empty());
            triangles += builtPart.mesh.triangleCount();
        }
    }
    CHECK(geometry.drawnTriangleCount() == triangles); // one instance per object
}

TEST_CASE("scene/scene_geometry: scene textures reach exactly the parts of their role", "[scene][geometry]") {
    const auto materials = realMaterials();
    const auto scene = referenceScene("m0_testroom");
    const SceneGeometry geometry = rv::scene::buildSceneGeometry(scene, materials);

    std::size_t textured = 0;
    for (const SceneObjectGeometry& built : geometry.objects) {
        const rv::scene::SceneObject* source = scene.findObject(built.id);
        REQUIRE(source != nullptr);
        for (const ScenePart& builtPart : built.parts) {
            INFO("object " << built.id << ", part " << builtPart.role);
            const auto expected = source->texturesByRole.find(builtPart.role);
            REQUIRE(builtPart.texture.has_value() == (expected != source->texturesByRole.end()));
            if (builtPart.texture) {
                ++textured;
                CHECK(builtPart.texture->pattern == expected->second.pattern);
                CHECK(builtPart.texture->title == expected->second.title);
                CHECK(builtPart.texture->seed == expected->second.seed);
            }
        }
    }
    CHECK(textured == 24 + 1 + 2); // cartridge labels, the CRT screen, the two posters

    const ScenePart& label = part(object(geometry, "cartridge_1_0"), "label");
    REQUIRE(label.texture.has_value());
    CHECK(label.texture->kind == rv::scene::TextureSource::Kind::Pattern);
    CHECK(label.texture->pattern == "cover");
    CHECK(label.texture->title == "RETRO RALLY 16");
    CHECK(label.texture->seed == 101u);
    CHECK_FALSE(part(object(geometry, "cartridge_1_0"), "plastic_shell").texture.has_value());
    CHECK(part(object(geometry, "crt_tv"), "screen").texture->pattern == "color_bars");

    // The surfaces are shaped like the pictures they show: a landscape label, a 4:3 screen, a portrait poster.
    const glm::vec2 labelScale = rv::geometry::uvMetersPerUnit(label.mesh);
    CHECK(labelScale.x / labelScale.y == Catch::Approx(0.101f / 0.061f).epsilon(1e-3));
    const glm::vec2 screenScale = rv::geometry::uvMetersPerUnit(part(object(geometry, "crt_tv"), "screen").mesh);
    CHECK(screenScale.x / screenScale.y == Catch::Approx(4.0f / 3.0f).epsilon(2e-2));
    const glm::vec2 posterScale = rv::geometry::uvMetersPerUnit(part(object(geometry, "poster_left"), "picture").mesh);
    CHECK(posterScale.x / posterScale.y == Catch::Approx(0.44f / 0.64f).epsilon(1e-3));
}

TEST_CASE("scene/scene_geometry: primitives use their kind as role and grids expand", "[scene][geometry]") {
    const auto materials = realMaterials();
    const SceneGeometry geometry = rv::scene::buildSceneGeometry(referenceScene("instancing_stress"), materials);

    REQUIRE(geometry.objects.size() == 4);
    CHECK(geometry.instanceCount() == 1001);
    CHECK(geometry.meshCount() == 4);
    const SceneObjectGeometry& walnut = object(geometry, "boxes_walnut");
    REQUIRE(walnut.parts.size() == 1);
    CHECK(walnut.parts.front().role == "box");
    CHECK(walnut.parts.front().material.name == "MAT_Wood_Walnut");
    CHECK(walnut.instances.size() == 400);
    CHECK(object(geometry, "floor").parts.front().role == "plane");
    CHECK(geometry.drawnTriangleCount() == 1000 * 12 + 2);
}

TEST_CASE("scene/scene_geometry: inline materials are parsed with the library rules", "[scene][geometry]") {
    const auto materials = realMaterials();
    const SceneGeometry geometry = rv::scene::buildSceneGeometry(referenceScene("material_test"), materials);
    CHECK(geometry.fallbackMaterialCount() == 0);

    const ScenePart& sphere = object(geometry, "sphere_m2_r3").parts.front();
    CHECK(sphere.role == "sphere");
    CHECK_FALSE(sphere.fallbackMaterial);
    CHECK(sphere.material.name == "sphere_m2_r3.inlineMaterial");
    CHECK(rv::core::nearlyEqual(sphere.material.baseColorFactor, linear("#C24A3A")));
    CHECK(sphere.material.metallicFactor == 0.5f);
    CHECK(sphere.material.roughnessFactor == 0.7625f);
}

TEST_CASE("scene/scene_geometry: missing or bad content falls back to visible magenta", "[scene][geometry]") {
    const auto materials = realMaterials();
    const json document = json::parse(R"({
        "schemaVersion": 1,
        "name": "fallbacks",
        "objects": [
            {"id": "unknown_material", "primitive": "box", "material": "MAT_Does_Not_Exist"},
            {"id": "no_material", "primitive": "quad"},
            {"id": "bad_inline", "primitive": "sphere", "inlineMaterial": {"roughness": 0.5, "shininess": 3}},
            {"id": "bad_params", "prefab": "shelf", "position": [1, 0, 2], "params": {"width": 99.0},
             "material": "MAT_Wood_Walnut"},
            {"id": "model", "model": "models/crt.glb", "grid": {"counts": [2, 1, 1], "spacing": [1, 1, 1]}},
            {"id": "per_role", "prefab": "cartridge", "material": "MAT_Plastic_Black",
             "materials": {"label": "MAT_Paper_Print"}}
        ]
    })");
    const auto scene = rv::scene::parseSceneDescription(document, "fallbacks.json");
    REQUIRE(scene.has_value());
    const SceneGeometry geometry = rv::scene::buildSceneGeometry(*scene, materials);
    REQUIRE(geometry.objects.size() == 6);
    const glm::vec4 magenta(1.0f, 0.0f, 1.0f, 1.0f);

    for (const char* id : {"unknown_material", "no_material", "bad_inline"}) {
        INFO(id);
        const SceneObjectGeometry& built = object(geometry, id);
        CHECK_FALSE(built.placeholder); // the geometry itself is fine
        REQUIRE(built.parts.size() == 1);
        CHECK(built.parts.front().fallbackMaterial);
        CHECK(built.parts.front().material.name == rv::assets::kFallbackMaterialName);
        CHECK(rv::core::nearlyEqual(built.parts.front().material.baseColorFactor, magenta));
    }

    for (const char* id : {"bad_params", "model"}) {
        INFO(id);
        const SceneObjectGeometry& built = object(geometry, id);
        CHECK(built.placeholder);
        REQUIRE(built.parts.size() == 1);
        const ScenePart& cube = built.parts.front();
        CHECK(cube.role == rv::scene::kPlaceholderRole);
        CHECK(cube.fallbackMaterial);
        CHECK(rv::core::nearlyEqual(cube.material.baseColorFactor, magenta));
        // A kPlaceholderSize cube standing on the object origin.
        CHECK(rv::core::nearlyEqual(cube.mesh.bounds.min, glm::vec3(-0.25f, 0.0f, -0.25f)));
        CHECK(rv::core::nearlyEqual(cube.mesh.bounds.max, glm::vec3(0.25f, 0.5f, 0.25f)));
    }
    CHECK(object(geometry, "model").instances.size() == 2); // the grid still applies to the placeholder
    CHECK(rv::core::nearlyEqual(glm::vec3(object(geometry, "bad_params").instances.front()[3]), glm::vec3(1, 0, 2)));

    const SceneObjectGeometry& perRole = object(geometry, "per_role");
    CHECK(part(perRole, "label").material.name == "MAT_Paper_Print");
    CHECK(part(perRole, "plastic_shell").material.name == "MAT_Plastic_Black");
    CHECK_FALSE(part(perRole, "label").fallbackMaterial);

    CHECK(geometry.placeholderCount() == 2);
    CHECK(geometry.fallbackMaterialCount() == 5);
}

TEST_CASE("scene/scene_geometry: a library without the file still has the built-in fallback", "[scene][geometry]") {
    const rv::assets::MaterialLibrary empty;
    const SceneGeometry geometry = rv::scene::buildSceneGeometry(referenceScene("instancing_stress"), empty);
    CHECK(geometry.fallbackMaterialCount() == geometry.meshCount());
    for (const SceneObjectGeometry& built : geometry.objects) {
        for (const ScenePart& builtPart : built.parts) {
            CHECK(builtPart.material.name == rv::assets::kFallbackMaterialName);
        }
    }
}

namespace {

const rv::scene::ColliderBox& colliderBox(const rv::scene::CollisionWorld& world, std::string_view tag) {
    INFO("collider " << tag);
    const auto boxes = world.boxes();
    const auto found =
        std::find_if(boxes.begin(), boxes.end(), [tag](const rv::scene::ColliderBox& box) { return box.tag == tag; });
    REQUIRE(found != boxes.end());
    return *found;
}

bool closeTo(const rv::core::Aabb& box, const glm::vec3& min, const glm::vec3& max) {
    return rv::core::nearlyEqual(box.min, min, 1e-4f) && rv::core::nearlyEqual(box.max, max, 1e-4f);
}

} // namespace

TEST_CASE("scene/scene_geometry: the collision world places every collider of every colliding object",
          "[scene][geometry][collision]") {
    const SceneGeometry geometry = rv::scene::buildSceneGeometry(referenceScene("m0_testroom"), realMaterials());
    CHECK(object(geometry, "room").colliders.size() == 7);
    CHECK(object(geometry, "shelf_main").colliders.size() == 1);
    CHECK(object(geometry, "crt_tv").colliders.size() == 1);
    // "collides": false (rug, posters) leaves no collider.
    CHECK(object(geometry, "rug").colliders.empty());
    CHECK(object(geometry, "poster_left").colliders.empty());
    CHECK(object(geometry, "poster_right").colliders.empty());

    const rv::scene::CollisionWorld world = rv::scene::buildCollisionWorld(geometry);
    // Six wall parts and the doorway stop, the shelf, 24 cartridges, the cabinet, the CRT, the console and the lamp.
    CHECK(world.boxes().size() == 36);
    // Prefab bounds moved to the object position: the 1.2 x 2.0 x 0.35 m shelf at (-1.5, 0, -4.8).
    CHECK(closeTo(colliderBox(world, "shelf_main").box, {-2.1f, 0.0f, -4.975f}, {-0.9f, 2.0f, -4.625f}));
    CHECK_FALSE(colliderBox(world, "shelf_main").turned.has_value());
    // The room's named walls; the doorway part above the opening stays out of the walker's way and a stop in the
    // outer face of the opening keeps the walker inside.
    CHECK(closeTo(colliderBox(world, "room/north").box, {-4.65f, 0.0f, -5.15f}, {4.65f, 3.0f, -5.0f}));
    CHECK(closeTo(colliderBox(world, "room/south_above").box, {-0.8f, 2.3f, 5.0f}, {0.8f, 3.0f, 5.15f}));
    CHECK(closeTo(colliderBox(world, "room/south_doorway").box, {-0.8f, 0.0f, 5.13f}, {0.8f, 2.3f, 5.15f}));
    // The console is turned by -8 degrees: it collides with its exact footprint, inside a larger world box.
    const rv::scene::ColliderBox& console = colliderBox(world, "console");
    CHECK(console.box.size().x > 0.24f);
    CHECK(console.box.size().z > 0.2f);
    CHECK(console.box.min.y == Catch::Approx(0.7f));
    REQUIRE(console.turned.has_value());
    CHECK(rv::core::nearlyEqual(glm::vec3(console.turned->center.x, 0.0f, console.turned->center.y),
                                glm::vec3(2.22f, 0.0f, -4.62f)));
    CHECK(rv::core::nearlyEqual(glm::vec3(console.turned->halfExtents.x, 0.0f, console.turned->halfExtents.y),
                                glm::vec3(0.12f, 0.0f, 0.1f)));
    const float turn = rv::core::degreesToRadians(-8.0f);
    CHECK(rv::core::nearlyEqual(glm::vec3(console.turned->axisX.x, 0.0f, console.turned->axisX.y),
                                glm::vec3(std::cos(turn), 0.0f, -std::sin(turn))));

    // Grids give one box per instance, tagged with the instance index.
    const SceneGeometry stress = rv::scene::buildSceneGeometry(referenceScene("instancing_stress"), realMaterials());
    const rv::scene::CollisionWorld boxes = rv::scene::buildCollisionWorld(stress);
    CHECK(boxes.boxes().size() == 1001); // 1000 boxes and the floor plane
    const rv::core::Aabb first = colliderBox(boxes, "boxes_walnut[0]").box;
    CHECK(closeTo(first, {-2.85f, 0.0f, -2.85f}, {-2.55f, 0.3f, -2.55f}));
    CHECK(closeTo(colliderBox(boxes, "boxes_walnut[1]").box, {-2.25f, 0.0f, -2.85f}, {-1.95f, 0.3f, -2.55f}));
}

TEST_CASE("scene/scene_geometry: placeholders collide like the furniture they replace",
          "[scene][geometry][collision]") {
    const json document = json::parse(R"({
        "schemaVersion": 1,
        "name": "placeholders",
        "objects": [
            {"id": "broken_shelf", "prefab": "shelf", "position": [1, 0, 2], "params": {"width": 99.0}},
            {"id": "ghost_model", "model": "models/crt.glb", "collides": false},
            {"id": "pair", "prefab": "crt", "grid": {"counts": [2, 1, 1], "spacing": [1, 1, 1]}}
        ]
    })");
    const auto scene = rv::scene::parseSceneDescription(document, "placeholders.json");
    REQUIRE(scene.has_value());
    const SceneGeometry geometry = rv::scene::buildSceneGeometry(*scene, realMaterials());
    REQUIRE(object(geometry, "broken_shelf").colliders.size() == 1);
    CHECK(object(geometry, "ghost_model").colliders.empty());

    const rv::scene::CollisionWorld world = rv::scene::buildCollisionWorld(geometry);
    REQUIRE(world.boxes().size() == 3);
    CHECK(closeTo(colliderBox(world, "broken_shelf").box, {0.75f, 0.0f, 1.75f}, {1.25f, 0.5f, 2.25f}));
    CHECK(closeTo(colliderBox(world, "pair[1]").box, {0.645f, 0.0f, -0.28f}, {1.355f, 0.61f, 0.28f}));
    CHECK(colliderBox(world, "pair[0]").box.max.x == Catch::Approx(0.355f));
}

TEST_CASE("scene/scene_geometry: a room turned about +Y keeps its interior free and its doorway open",
          "[scene][geometry][collision]") {
    // The m0_testroom shell turned by 30 degrees: its walls collide with their own footprints, not with world boxes
    // that would cover the interior.
    const json document = json::parse(R"({
        "schemaVersion": 1,
        "name": "turned_room",
        "objects": [
            {"id": "room", "prefab": "room_shell", "rotationDeg": [0, 30, 0],
             "params": {"size": [9.0, 10.0], "wallHeight": 3.0, "wallThickness": 0.15,
                        "doorway": {"wall": "south", "offset": 0.0, "width": 1.6, "height": 2.3}},
             "materials": {"floor": "MAT_Wood_Oak", "wall": "MAT_Wall_Painted", "ceiling": "MAT_Wood_Walnut",
                           "trim": "MAT_Wood_Walnut"}}
        ]
    })");
    const auto scene = rv::scene::parseSceneDescription(document, "turned_room.json");
    REQUIRE(scene.has_value());
    const rv::scene::CollisionWorld world =
        rv::scene::buildCollisionWorld(rv::scene::buildSceneGeometry(*scene, realMaterials()));
    REQUIRE(world.boxes().size() == 7);
    for (const rv::scene::ColliderBox& wall : world.boxes()) {
        INFO(wall.tag);
        CHECK(wall.turned.has_value());
    }

    // The room's own +X (east) and +Z (south, towards the doorway) in world space.
    const float yaw = rv::core::degreesToRadians(30.0f);
    const glm::vec3 east(std::cos(yaw), 0.0f, -std::sin(yaw));
    const glm::vec3 south(std::sin(yaw), 0.0f, std::cos(yaw));
    const rv::scene::Capsule capsule;
    const glm::vec3 center(0.0f);
    CHECK_FALSE(world.overlaps(center, capsule));
    // The walls stop the walker one capsule radius (0.3 m) before their interior faces, 4.5 and 5 m from the center.
    CHECK(rv::core::nearlyEqual(world.moveCapsule(center, 10.0f * east, capsule), 4.2f * east, 1e-3f));
    CHECK(rv::core::nearlyEqual(world.moveCapsule(center, -10.0f * east, capsule), -4.2f * east, 1e-3f));
    CHECK(rv::core::nearlyEqual(world.moveCapsule(center, -10.0f * south, capsule), -4.7f * south, 1e-3f));
    const glm::vec3 besideDoorway = 2.0f * east;
    CHECK(rv::core::nearlyEqual(world.moveCapsule(besideDoorway, 10.0f * south, capsule), besideDoorway + 4.7f * south,
                                1e-3f));
    // Through the doorway up to the stop in its outer face: 5.15 - 0.02 - 0.3 m from the center.
    CHECK(rv::core::nearlyEqual(world.moveCapsule(center, 10.0f * south, capsule), 4.83f * south, 1e-3f));
}
