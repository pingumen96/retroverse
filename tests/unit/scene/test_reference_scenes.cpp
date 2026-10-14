#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include <rv/core/json.hpp>
#include <rv/core/math.hpp>
#include <rv/core/text.hpp>
#include <rv/scene/batch_planner.hpp>
#include <rv/scene/camera.hpp>
#include <rv/scene/collision.hpp>
#include <rv/scene/scene_description.hpp>

using Catch::Approx;
using nlohmann::json;
using rv::scene::LightType;
using rv::scene::SceneDescription;
using rv::scene::SceneObject;
using rv::scene::TextureSource;

namespace {

// --- Shared contracts (they must match the material library and the procedural geometry EXACTLY) ------------

const std::set<std::string> kSharedMaterials{
    "MAT_Wood_Walnut",       "MAT_Wood_Oak",       "MAT_Wood_Varnished",   "MAT_Concrete_Warm", "MAT_Wall_Painted",
    "MAT_Plaster_Used",      "MAT_Metal_Graphite", "MAT_Metal_Brushed",    "MAT_Plastic_Black", "MAT_Plastic_Gray",
    "MAT_Plastic_AgedIvory", "MAT_Plastic_Accent", "MAT_Glass_Display",    "MAT_Glass_CRT",     "MAT_Fabric_Sofa",
    "MAT_Carpet_Ornate",     "MAT_Cardboard_Box",  "MAT_Paper_Print",      "MAT_Neon_Diffuser", "MAT_Screen_Emissive",
    "MAT_Lamp_Shade",        "MAT_Label_Print",    "MAT_Fallback_Magenta",
};

/// Material roles of every procedural prefab.
const std::map<std::string, std::set<std::string>> kPrefabRoles{
    {"room_shell", {"floor", "wall", "ceiling", "trim"}},
    {"shelf", {"wood_primary", "wood_secondary"}},
    {"cartridge", {"plastic_shell", "label"}},
    {"crt", {"plastic_body", "plastic_bezel", "screen", "plastic_dark"}},
    {"tv_cabinet", {"wood_primary", "wood_secondary", "metal_trim"}},
    {"console_cartridge", {"plastic_body", "plastic_dark", "plastic_accent"}},
    {"poster_frame", {"frame", "picture"}},
    {"rug", {"fabric"}},
    {"floor_lamp", {"metal", "lamp_shade"}},
};

/// Parameter keys of every prefab and primitive.
const std::map<std::string, std::set<std::string>> kParamKeys{
    {"room_shell", {"size", "wallHeight", "wallThickness", "metersPerTile", "ceiling", "baseboardHeight", "doorway"}},
    {"shelf",
     {"width", "height", "depth", "levels", "boardThickness", "back", "plinthHeight", "slotSize", "slotGap",
      "slotMargin"}},
    {"cartridge", {"size", "chamfer", "labelMargin", "labelInset", "gripRidges"}},
    {"crt", {"width", "height", "depth", "screenAspect", "bezel", "screenInset", "backTaper"}},
    {"tv_cabinet", {"width", "height", "depth", "drawers", "legHeight"}},
    {"console_cartridge", {"width", "height", "depth"}},
    {"poster_frame", {"width", "height", "frameWidth", "depth"}},
    {"rug", {"width", "depth", "thickness"}},
    {"floor_lamp", {"height", "baseRadius", "shadeRadius", "shadeHeight"}},
    {"box", {"size", "uv", "metersPerTile"}},
    {"plane", {"size", "uvTiling"}},
    {"quad", {"size"}},
    {"sphere", {"radius", "segments", "rings"}},
    {"cylinder", {"radius", "height", "segments", "capped"}},
};

const glm::vec3 kDefaultCartridgeSize(0.125f, 0.085f, 0.020f);

/// Shelf slot layout from the shared parameter contract (defaults for missing keys).
struct ShelfLayout {
    float width = 1.2f;
    float height = 2.0f;
    int levels = 5;
    float boardThickness = 0.025f;
    bool back = true;
    float plinthHeight = 0.08f;
    glm::vec3 slotSize = kDefaultCartridgeSize;
    float slotGap = 0.004f;
    float slotMargin = 0.03f;

    static ShelfLayout fromParams(const json& params) {
        ShelfLayout layout;
        layout.width = params.value("width", layout.width);
        layout.height = params.value("height", layout.height);
        layout.levels = params.value("levels", layout.levels);
        layout.boardThickness = params.value("boardThickness", layout.boardThickness);
        layout.back = params.value("back", layout.back);
        layout.plinthHeight = params.value("plinthHeight", layout.plinthHeight);
        if (params.contains("slotSize")) {
            layout.slotSize = rv::core::jsonToVec3(params.at("slotSize"), "slotSize").value();
        }
        layout.slotGap = params.value("slotGap", layout.slotGap);
        layout.slotMargin = params.value("slotMargin", layout.slotMargin);
        return layout;
    }

    [[nodiscard]] float innerWidth() const { return width - 2.0f * boardThickness; }

    [[nodiscard]] int slotsPerLevel() const {
        return static_cast<int>(std::floor((innerWidth() - 2.0f * slotMargin + slotGap) / (slotSize.x + slotGap)));
    }

    /// Socket translation of Slot_<level>_<index>: bottom-center of the item on the board, in shelf space.
    [[nodiscard]] glm::vec3 slotBottomCenter(int level, int index) const {
        const float x =
            -0.5f * innerWidth() + slotMargin + 0.5f * slotSize.x + static_cast<float>(index) * (slotSize.x + slotGap);
        const float pitch = (height - boardThickness - plinthHeight) / static_cast<float>(levels);
        const float y = plinthHeight + boardThickness + static_cast<float>(level) * pitch;
        const float z = back ? 0.5f * boardThickness : 0.0f;
        return {x, y, z};
    }
};

std::filesystem::path referenceScenePath(const char* name) {
    return rv::core::pathFromUtf8(RV_TEST_ASSET_DIR "/style_tests/reference_scenes/") / (std::string(name) + ".json");
}

SceneDescription loadReference(const char* name) {
    auto scene = rv::scene::loadSceneDescription(referenceScenePath(name));
    INFO((scene ? std::string() : scene.error().toString()));
    REQUIRE(scene.has_value());
    return std::move(*scene);
}

std::vector<const SceneObject*> objectsOfKind(const SceneDescription& scene, std::string_view kind) {
    std::vector<const SceneObject*> result;
    for (const SceneObject& object : scene.objects) {
        if (object.source != SceneObject::Source::Model && object.kind == kind) {
            result.push_back(&object);
        }
    }
    return result;
}

glm::vec3 translationOf(const glm::mat4& transform) {
    return glm::vec3(transform[3]);
}

bool isPrimitive(const SceneObject& object) {
    return object.source == SceneObject::Source::Primitive;
}

/// Checks kinds, parameter keys, roles and material names of every object against the shared contracts.
void checkSharedContracts(const SceneDescription& scene) {
    for (const SceneObject& object : scene.objects) {
        INFO("object " << object.id);
        REQUIRE(object.source != SceneObject::Source::Model); // reference scenes are fully procedural
        const auto keys = kParamKeys.find(object.kind);
        REQUIRE(keys != kParamKeys.end());
        CHECK(isPrimitive(object) == !kPrefabRoles.contains(object.kind));
        for (const auto& param : object.params.items()) {
            INFO("param " << param.key());
            CHECK(keys->second.contains(param.key()));
        }

        if (!object.material.empty()) {
            CHECK(kSharedMaterials.contains(object.material));
        }
        for (const auto& entry : object.materialsByRole) {
            INFO("role " << entry.first);
            CHECK(kSharedMaterials.contains(entry.second));
        }

        if (isPrimitive(object)) {
            // One material per primitive: a library name or an inline definition.
            CHECK(object.materialsByRole.empty());
            CHECK(object.texturesByRole.empty());
            CHECK(object.material.empty() != object.inlineMaterial.is_null());
            continue;
        }
        const std::set<std::string>& roles = kPrefabRoles.at(object.kind);
        for (const auto& entry : object.materialsByRole) {
            INFO("role " << entry.first);
            CHECK(roles.contains(entry.first));
        }
        for (const auto& entry : object.texturesByRole) {
            INFO("texture role " << entry.first);
            CHECK(roles.contains(entry.first));
        }
        for (const std::string& role : roles) {
            INFO("role " << role);
            CHECK_FALSE(object.materialNameForRole(role).empty()); // every part gets a library material
        }
    }
}

/// Camera of a scene setup as the application builds it (16:9 viewport).
rv::scene::Camera cameraOf(const rv::scene::CameraSetup& setup) {
    rv::scene::Camera camera;
    camera.setLens(setup.lens);
    camera.setAspect(16.0f / 9.0f);
    camera.setPose(setup.position, rv::core::degreesToRadians(setup.yawDeg),
                   rv::core::degreesToRadians(setup.pitchDeg));
    return camera;
}

struct Footprint {
    glm::vec2 min;
    glm::vec2 max;
    float height;
};

/// XZ footprint of floor-standing furniture from its contract parameters (unrotated objects only).
Footprint footprintOf(const SceneObject& object) {
    const glm::vec3 position = translationOf(object.transform);
    glm::vec2 half(0.0f);
    float height = 0.0f;
    if (object.kind == "floor_lamp") {
        const float radius =
            std::max(object.params.value("baseRadius", 0.16f), object.params.value("shadeRadius", 0.22f));
        half = glm::vec2(radius);
        height = object.params.value("height", 1.6f);
    } else {
        half = glm::vec2(0.5f * object.params.at("width").get<float>(), 0.5f * object.params.at("depth").get<float>());
        height = object.params.at("height").get<float>();
    }
    return {glm::vec2(position.x, position.z) - half, glm::vec2(position.x, position.z) + half, height};
}

float gapBetween(const Footprint& a, const Footprint& b) {
    const float dx = std::max({0.0f, b.min.x - a.max.x, a.min.x - b.max.x});
    const float dz = std::max({0.0f, b.min.y - a.max.y, a.min.y - b.max.y});
    return std::sqrt(dx * dx + dz * dz);
}

} // namespace

TEST_CASE("scene/reference_scenes: the three reference scenes load", "[scene][reference_scenes]") {
    for (const char* name : {"m0_testroom", "material_test", "instancing_stress"}) {
        INFO("scene " << name);
        const SceneDescription scene = loadReference(name);
        CHECK(scene.name == name);
        CHECK_FALSE(scene.objects.empty());
        CHECK_FALSE(scene.lights.empty());
        checkSharedContracts(scene);
    }
}

TEST_CASE("scene/reference_scenes: instancing_stress expands to exactly 1000 boxes in 3 batches",
          "[scene][reference_scenes]") {
    const SceneDescription scene = loadReference("instancing_stress");
    const auto boxes = objectsOfKind(scene, "box");
    REQUIRE(boxes.size() == 3);

    std::size_t boxInstances = 0;
    std::set<std::string> materials;
    std::vector<std::size_t> counts;
    for (const SceneObject* box : boxes) {
        CHECK(box->params == boxes[0]->params); // identical parameters: one shared mesh
        CHECK_FALSE(box->instances.empty());
        boxInstances += box->instanceTransforms().size();
        counts.push_back(box->instanceTransforms().size());
        materials.insert(box->material);
    }
    CHECK(boxInstances == 1000);
    CHECK(counts == std::vector<std::size_t>{400, 300, 300});
    CHECK(materials.size() == 3);

    // Feed the scene to the batch planner the way the application does: one mesh per (kind, params), one
    // material per name. Boxes give 3 batches of 400 + 300 + 300, the floor a fourth one.
    std::map<std::string, std::uint32_t> meshIds;
    std::map<std::string, std::uint32_t> materialIds;
    std::vector<rv::scene::RenderItem> items;
    for (const SceneObject& object : scene.objects) {
        const auto mesh =
            meshIds.emplace(object.kind + object.params.dump(), static_cast<std::uint32_t>(meshIds.size()));
        const auto material = materialIds.emplace(object.material, static_cast<std::uint32_t>(materialIds.size()));
        for (const glm::mat4& model : object.instanceTransforms()) {
            items.push_back({rv::scene::MeshHandle{mesh.first->second},
                             rv::scene::MaterialHandle{material.first->second}, rv::scene::ShaderKind::Pbr, model,
                             object.castsShadows});
        }
    }
    const rv::scene::BatchPlan plan = rv::scene::BatchPlanner{}.plan(items);
    CHECK(plan.instanceTransforms.size() == 1001);
    REQUIRE(plan.batches.size() == 4);
    std::multiset<std::uint32_t> batchSizes;
    for (const auto& batch : plan.batches) {
        batchSizes.insert(batch.instanceCount);
    }
    CHECK(batchSizes == std::multiset<std::uint32_t>{1, 300, 300, 400});

    // The camera frames every box.
    const rv::scene::Frustum frustum = cameraOf(scene.camera).frustum();
    for (const SceneObject* box : boxes) {
        for (const glm::mat4& model : box->instanceTransforms()) {
            CHECK(frustum.contains(translationOf(model)));
        }
    }
}

TEST_CASE("scene/reference_scenes: material_test sweeps roughness along X and metallic along Y",
          "[scene][reference_scenes]") {
    const SceneDescription scene = loadReference("material_test");
    const auto spheres = objectsOfKind(scene, "sphere");
    REQUIRE(spheres.size() == 25);
    CHECK(objectsOfKind(scene, "plane").size() == 1);
    CHECK(scene.lights.size() == 2);

    std::map<float, std::set<float>> roughnessByX;
    std::map<float, std::set<float>> metallicByY;
    const rv::scene::Frustum frustum = cameraOf(scene.camera).frustum();
    for (const SceneObject* sphere : spheres) {
        REQUIRE(sphere->inlineMaterial.is_object());
        CHECK(sphere->params.value("radius", 0.5f) == 0.35f);
        const glm::vec3 center = translationOf(sphere->transform);
        roughnessByX[center.x].insert(sphere->inlineMaterial.at("roughness").get<float>());
        metallicByY[center.y].insert(sphere->inlineMaterial.at("metallic").get<float>());
        CHECK(frustum.contains(center));
    }
    // Five columns and five rows, each with a single value, increasing along the axis.
    REQUIRE(roughnessByX.size() == 5);
    REQUIRE(metallicByY.size() == 5);
    float previous = -1.0f;
    for (const auto& column : roughnessByX) {
        REQUIRE(column.second.size() == 1);
        CHECK(*column.second.begin() > previous);
        previous = *column.second.begin();
    }
    CHECK(*roughnessByX.begin()->second.begin() == Approx(0.05f));
    CHECK(previous == Approx(1.0f));
    previous = -1.0f;
    for (const auto& row : metallicByY) {
        REQUIRE(row.second.size() == 1);
        CHECK(*row.second.begin() > previous);
        previous = *row.second.begin();
    }
    CHECK(*metallicByY.begin()->second.begin() == Approx(0.0f));
    CHECK(previous == Approx(1.0f));
}

TEST_CASE("scene/reference_scenes: m0_testroom composition", "[scene][reference_scenes][m0_testroom]") {
    const SceneDescription scene = loadReference("m0_testroom");

    std::set<std::string> ids;
    for (const SceneObject& object : scene.objects) {
        CHECK(ids.insert(object.id).second);
    }

    // Room: 9 x 10 m, 3 m high, doorway in the south wall; the camera stands inside, near it, at eye height.
    const auto rooms = objectsOfKind(scene, "room_shell");
    REQUIRE(rooms.size() == 1);
    const json& room = rooms[0]->params;
    CHECK(room.at("size") == json::array({9.0, 10.0}));
    CHECK(room.at("wallHeight").get<float>() == 3.0f);
    CHECK(room.at("doorway").at("wall") == "south");
    CHECK(rooms[0]->materialNameForRole("floor") == "MAT_Wood_Oak");
    CHECK(rooms[0]->materialNameForRole("wall") == "MAT_Wall_Painted");
    CHECK(rooms[0]->materialNameForRole("trim") == "MAT_Wood_Walnut");
    CHECK(scene.camera.position.y == Approx(1.65f));
    CHECK(std::abs(scene.camera.position.x) < 4.5f - 0.3f);
    CHECK(scene.camera.position.z > 2.5f);
    CHECK(scene.camera.position.z < 5.0f - 0.3f);
    CHECK(scene.camera.yawDeg == 0.0f); // looking north (-Z)

    // The hero wall is in view: shelf, TV and lamp.
    const rv::scene::Frustum frustum = cameraOf(scene.camera).frustum();
    const SceneObject* shelf = scene.findObject("shelf_main");
    REQUIRE(shelf != nullptr);
    const auto crts = objectsOfKind(scene, "crt");
    const auto lamps = objectsOfKind(scene, "floor_lamp");
    const auto cabinets = objectsOfKind(scene, "tv_cabinet");
    REQUIRE(crts.size() == 1);
    REQUIRE(lamps.size() == 1);
    REQUIRE(cabinets.size() == 1);
    CHECK(frustum.contains(translationOf(shelf->transform) + glm::vec3(0.0f, 1.0f, 0.0f)));
    CHECK(frustum.contains(translationOf(crts[0]->transform) + glm::vec3(0.0f, 0.3f, 0.0f)));
    CHECK(frustum.contains(translationOf(lamps[0]->transform) + glm::vec3(0.0f, 1.45f, 0.0f)));

    // Shelf against the north wall at x = -1.5, TV cabinet at x = +1.8 with the CRT on top.
    CHECK(translationOf(shelf->transform).x == Approx(-1.5f));
    CHECK(translationOf(shelf->transform).z - 0.5f * shelf->params.at("depth").get<float>() > -5.0f);
    CHECK(translationOf(shelf->transform).z < -4.5f);
    CHECK(translationOf(cabinets[0]->transform).x == Approx(1.8f));
    CHECK(translationOf(crts[0]->transform).y == Approx(cabinets[0]->params.at("height").get<float>()));
    CHECK(crts[0]->materialNameForRole("screen") == "MAT_Screen_Emissive");
    CHECK(crts[0]->texturesByRole.at("screen").pattern == "color_bars");
    CHECK(objectsOfKind(scene, "console_cartridge").size() == 1);
    CHECK(objectsOfKind(scene, "rug").size() == 1);
    CHECK(objectsOfKind(scene, "poster_frame").size() == 2);

    // Everything collides except the rug and the posters; the rug casts no shadow.
    for (const SceneObject& object : scene.objects) {
        INFO("object " << object.id);
        CHECK(object.collides == (object.kind != "rug" && object.kind != "poster_frame"));
        if (object.kind == "poster_frame") {
            CHECK(object.texturesByRole.at("picture").pattern == "cover");
        }
    }
    CHECK_FALSE(objectsOfKind(scene, "rug")[0]->castsShadows);

    // Lighting: warm tungsten key, light magenta accent on the TV, the lamp's own bulb, low warm ambient.
    REQUIRE(scene.lights.size() == 3);
    const glm::vec3 tungsten = rv::core::hexToLinearRgb("#FFC88A").value();
    const glm::vec3 magenta = rv::core::hexToLinearRgb("#B34DCE").value();
    const auto findLight = [&scene](LightType type, const glm::vec3& color) {
        return std::count_if(scene.lights.begin(), scene.lights.end(), [&](const rv::scene::Light& light) {
            return light.type == type && rv::core::nearlyEqual(light.color, color);
        });
    };
    CHECK(findLight(LightType::Point, tungsten) == 2);
    CHECK(findLight(LightType::Spot, magenta) == 1);
    for (const rv::scene::Light& light : scene.lights) {
        INFO("light " << light.name);
        if (light.type == LightType::Spot) {
            const glm::vec3 screen = translationOf(crts[0]->transform) + glm::vec3(0.0f, 0.3f, 0.0f);
            const float cosAngle = glm::dot(light.direction, glm::normalize(screen - light.position));
            CHECK(cosAngle > std::cos(rv::core::degreesToRadians(light.innerConeDeg)));
            CHECK(light.intensity < 10.0f); // an accent, not a key light
        }
        CHECK(light.position.y > 0.0f);
        CHECK(light.position.y < room.at("wallHeight").get<float>());
    }
    const glm::vec3 lampPosition = translationOf(lamps[0]->transform);
    const float lampHeight = lamps[0]->params.at("height").get<float>();
    const float shadeHeight = lamps[0]->params.at("shadeHeight").get<float>();
    CHECK(std::count_if(scene.lights.begin(), scene.lights.end(), [&](const rv::scene::Light& light) {
              return std::abs(light.position.x - lampPosition.x) < 1e-4f &&
                     std::abs(light.position.z - lampPosition.z) < 1e-4f &&
                     light.position.y > lampHeight - shadeHeight && light.position.y < lampHeight;
          }) == 1);
    CHECK(scene.environment.ambientIntensity < 0.2f);
    CHECK(scene.environment.ambientSky.r > scene.environment.ambientSky.b); // warm
}

TEST_CASE("scene/reference_scenes: m0_testroom cartridges sit exactly in the shelf slots",
          "[scene][reference_scenes][m0_testroom]") {
    const SceneDescription scene = loadReference("m0_testroom");
    const SceneObject* shelf = scene.findObject("shelf_main");
    REQUIRE(shelf != nullptr);
    const ShelfLayout layout = ShelfLayout::fromParams(shelf->params);
    REQUIRE(layout.slotsPerLevel() == 8);
    CHECK(shelf->materialNameForRole("wood_primary") == "MAT_Wood_Walnut");
    CHECK(shelf->materialNameForRole("wood_secondary") == "MAT_Wood_Varnished");

    const auto cartridges = objectsOfKind(scene, "cartridge");
    REQUIRE(cartridges.size() == 24);
    std::set<std::pair<int, int>> usedSlots;
    std::set<std::string> titles;
    std::set<std::uint32_t> seeds;
    for (const SceneObject* cartridge : cartridges) {
        INFO("cartridge " << cartridge->id);
        const glm::vec3 size = cartridge->params.contains("size")
                                   ? rv::core::jsonToVec3(cartridge->params.at("size"), cartridge->id).value()
                                   : kDefaultCartridgeSize;
        CHECK(rv::core::nearlyEqual(size, layout.slotSize));
        // Upright with the label facing out of the shelf (+Z), like the shelf itself.
        for (int column = 0; column < 3; ++column) {
            CHECK(rv::core::nearlyEqual(glm::vec3(cartridge->transform[column]), glm::vec3(shelf->transform[column])));
        }

        // The cartridge pivot is its center: half its height above the slot's bottom-center point.
        const glm::vec3 center = translationOf(cartridge->transform);
        int matches = 0;
        for (int level = 0; level < layout.levels; ++level) {
            for (int index = 0; index < layout.slotsPerLevel(); ++index) {
                const glm::vec3 local = layout.slotBottomCenter(level, index) + glm::vec3(0.0f, 0.5f * size.y, 0.0f);
                const glm::vec3 expected(shelf->transform * glm::vec4(local, 1.0f));
                if (rv::core::nearlyEqual(center, expected, 1e-4f)) {
                    ++matches;
                    CHECK(usedSlots.insert({level, index}).second); // one cartridge per slot
                }
            }
        }
        CHECK(matches == 1);

        CHECK(cartridge->materialNameForRole("plastic_shell") == "MAT_Plastic_Gray");
        CHECK(cartridge->materialNameForRole("label") == "MAT_Label_Print");
        const TextureSource& label = cartridge->texturesByRole.at("label");
        CHECK(label.kind == TextureSource::Kind::Pattern);
        CHECK(label.pattern == "cover");
        CHECK_FALSE(label.title.empty());
        titles.insert(label.title);
        seeds.insert(label.seed);
    }
    CHECK(usedSlots.size() == 24);
    CHECK(titles.size() == 24); // every cover is distinct
    CHECK(seeds.size() == 24);
    CHECK(titles.contains("SUPER PLACEHOLDER WORLD"));
}

TEST_CASE("scene/reference_scenes: m0_testroom keeps 1.2 m free passages", "[scene][reference_scenes][m0_testroom]") {
    const SceneDescription scene = loadReference("m0_testroom");
    constexpr float kPassage = 1.2f;
    constexpr float kAgainstWall = 0.5f; // closer than this to a wall = placed against it, not a passage
    constexpr float kHalfWidth = 4.5f;
    constexpr float kHalfDepth = 5.0f;

    // Floor-standing colliders, as boxes for a walk test.
    std::vector<std::pair<std::string, Footprint>> furniture;
    rv::scene::CollisionWorld world;
    for (const SceneObject& object : scene.objects) {
        const glm::vec3 position = translationOf(object.transform);
        if (!object.collides || object.kind == "room_shell" || position.y != 0.0f) {
            continue;
        }
        INFO("object " << object.id);
        CHECK(rv::core::nearlyEqual(glm::mat3(object.transform)[0], glm::vec3(1.0f, 0.0f, 0.0f))); // unrotated
        const Footprint footprint = footprintOf(object);
        furniture.emplace_back(object.id, footprint);
        world.addBox(rv::core::Aabb::fromMinMax({footprint.min.x, 0.0f, footprint.min.y},
                                                {footprint.max.x, footprint.height, footprint.max.y}),
                     object.id);
        for (const float wallGap : {footprint.min.x + kHalfWidth, kHalfWidth - footprint.max.x,
                                    footprint.min.y + kHalfDepth, kHalfDepth - footprint.max.y}) {
            CHECK((wallGap < kAgainstWall || wallGap >= kPassage));
            CHECK(wallGap >= 0.0f); // inside the room
        }
    }
    REQUIRE(furniture.size() == 3); // shelf, TV cabinet, floor lamp
    for (std::size_t i = 0; i < furniture.size(); ++i) {
        for (std::size_t j = i + 1; j < furniture.size(); ++j) {
            INFO(furniture[i].first << " <-> " << furniture[j].first);
            CHECK(gapBetween(furniture[i].second, furniture[j].second) >= kPassage);
        }
    }

    // The walker spawns free and can walk from the doorway to the front of the hero wall.
    const rv::scene::Capsule capsule;
    const glm::vec3 spawn(scene.camera.position.x, 0.0f, scene.camera.position.z);
    CHECK_FALSE(world.overlaps(spawn, capsule));
    const glm::vec3 heroWall = world.moveCapsule(spawn, glm::vec3(0.0f, 0.0f, -8.0f), capsule);
    CHECK(heroWall.z < -4.0f);
}
